#!/usr/bin/python3 -I
"""kl-msc - a USB mass-storage "stick" in userspace (FunctionFS) that LOGS everything.

Why userspace: the Kaffelogic Nano 7 (older board, USB-A host port) is a very minimal USB host. Logging every
enumeration step, reset and SCSI command is what made it possible to find out why it rejected the kernel gadget,
and kl-sassi uses the log (WRITE10 lines) to know whether the roaster is currently writing.

Log: /var/log/kl-msc.log (fsync per line)
Usage: kl-msc.py [--vid 0xabcd] [--pid 0x1234] [--ro]
"""
import argparse, os, struct, sys, threading, time, errno

IMG = "/var/lib/kaffelogic/nano7.img"
LOG = "/var/log/kl-msc.log"
G = "/sys/kernel/config/usb_gadget/kl"
FFS = "/dev/ffs-kl"
BLK = 512

_lf = None
_lock = threading.Lock()
T0 = time.monotonic()


def log(*a):
    line = f"{time.strftime('%H:%M:%S')} +{time.monotonic() - T0:8.3f} " + " ".join(str(x) for x in a)
    with _lock:
        _lf.write(line + "\n")
        _lf.flush()
        os.fsync(_lf.fileno())
    print(line, flush=True)


def w(path, val):
    with open(path, "w") as f:
        f.write(val)


def sh(cmd):
    os.system(cmd + " >/dev/null 2>&1")


# ---------------------------------------------------------------- configfs gadget
def teardown():
    if not os.path.isdir(G):
        return
    try:
        w(f"{G}/UDC", "")
    except OSError:
        pass
    sh(f"umount {FFS}")
    for p in (f"{G}/configs/c.1/ffs.kl",):
        if os.path.islink(p):
            os.unlink(p)
    for d in (f"{G}/configs/c.1/strings/0x409", f"{G}/configs/c.1", f"{G}/functions/ffs.kl",
              f"{G}/strings/0x409", G):
        try:
            os.rmdir(d)
        except OSError:
            pass


def setup_gadget(a):
    sh("modprobe -r g_mass_storage")
    sh("modprobe dwc2")
    sh("modprobe libcomposite")
    sh("modprobe usb_f_fs")
    teardown()
    os.makedirs(G)
    w(f"{G}/idVendor", a.vid)
    w(f"{G}/idProduct", a.pid)
    w(f"{G}/bcdDevice", "0x0100")
    w(f"{G}/bcdUSB", "0x0200")
    os.makedirs(f"{G}/strings/0x409")
    w(f"{G}/strings/0x409/manufacturer", "General")
    w(f"{G}/strings/0x409/product", "UDisk")
    w(f"{G}/strings/0x409/serialnumber", a.serial)
    os.makedirs(f"{G}/configs/c.1/strings/0x409")
    w(f"{G}/configs/c.1/MaxPower", "100")
    w(f"{G}/configs/c.1/bmAttributes", "0x80")
    os.makedirs(f"{G}/functions/ffs.kl")
    os.symlink(f"{G}/functions/ffs.kl", f"{G}/configs/c.1/ffs.kl")
    os.makedirs(FFS, exist_ok=True)
    if os.system(f"mount -t functionfs kl {FFS}"):
        raise SystemExit("mounting functionfs failed")


def descriptors():
    intf = struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 1)

    def ep(addr, mps):
        return struct.pack("<BBBBHB", 7, 5, addr, 2, mps, 0)

    fs = intf + ep(0x81, 64) + ep(0x02, 64)
    hs = intf + ep(0x81, 512) + ep(0x02, 512)
    body = struct.pack("<II", 3, 3) + fs + hs  # fs_count, hs_count
    hdr = struct.pack("<III", 3, 12 + len(body), 1 | 2)  # MAGIC_V2, len, HAS_FS|HAS_HS
    return hdr + body


def strings():
    s = b"Mass Storage\x00"
    body = struct.pack("<H", 0x0409) + s
    return struct.pack("<IIII", 2, 16 + len(body), 1, 1) + body


# ---------------------------------------------------------------- SCSI
class Disk:
    def __init__(self, path, ro):
        self.fd = os.open(path, os.O_RDONLY if ro else os.O_RDWR)
        self.ro = ro
        self.nblk = os.fstat(self.fd).st_size // BLK
        self.sense = (0, 0, 0)  # key, asc, ascq

    def set_sense(self, k=0, asc=0, ascq=0):
        self.sense = (k, asc, ascq)


INQ = (bytes([0x00, 0x80, 0x04, 0x02, 31, 0, 0, 0]) + b"General " + b"UDisk           " + b"5.00")

NAMES = {0x00: "TEST_UNIT_READY", 0x03: "REQUEST_SENSE", 0x12: "INQUIRY", 0x1A: "MODE_SENSE6",
         0x1B: "START_STOP", 0x1E: "PREVENT_ALLOW", 0x23: "READ_FORMAT_CAP", 0x25: "READ_CAPACITY10",
         0x28: "READ10", 0x2A: "WRITE10", 0x2F: "VERIFY10", 0x35: "SYNC_CACHE", 0x5A: "MODE_SENSE10",
         0x9E: "READ_CAPACITY16", 0xA0: "REPORT_LUNS"}


class BOT:
    def __init__(self, disk):
        self.d = disk
        self.ep_in = self.ep_out = None
        self.n = 0
        self.rd_blocks = 0

    def open_eps(self):
        self.ep_in = os.open(f"{FFS}/ep1", os.O_RDWR)
        self.ep_out = os.open(f"{FFS}/ep2", os.O_RDWR)

    def read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = os.read(self.ep_out, min(n - len(buf), 16384))
            if not chunk:
                break
            buf += chunk
        return buf

    def csw(self, tag, residue, status):
        os.write(self.ep_in, struct.pack("<IIIB", 0x53425355, tag, residue, status))
        log(f"   CSW tag={tag} res={residue} st={status} delivered")

    def loop(self):
        while True:
            log("   waiting for CBW")
            cbw = os.read(self.ep_out, 512)
            if len(cbw) != 31 or cbw[:4] != b"USBC":
                log(f"BAD CBW len={len(cbw)} {cbw[:32].hex()}")
                continue
            _, tag, dlen, flags, lun, cblen = struct.unpack("<IIIBBB", cbw[:15])
            cb = cbw[15:15 + cblen]
            self.handle(tag, dlen, flags & 0x80, lun, cb)

    def handle(self, tag, dlen, din, lun, cb):
        op = cb[0]
        name = NAMES.get(op, f"OP_{op:02x}")
        self.n += 1
        d = self.d
        data, status = b"", 0
        extra = ""
        if op == 0x12:  # INQUIRY
            evpd = cb[1] & 1
            if evpd:
                page = cb[2]
                data = bytes([0, page, 0, 0]) if page != 0 else bytes([0, 0, 0, 1, 0])
                extra = f"evpd page={page:02x}"
            else:
                data = INQ
        elif op == 0x00:
            pass
        elif op == 0x03:
            k, asc, ascq = d.sense
            data = bytes([0x70, 0, k, 0, 0, 0, 0, 10, 0, 0, 0, 0, asc, ascq, 0, 0, 0, 0])
            extra = f"sense={k:x}/{asc:02x}/{ascq:02x}"
            d.set_sense()
        elif op == 0x25:
            data = struct.pack(">II", d.nblk - 1, BLK)
        elif op == 0x9E:
            data = struct.pack(">QI", d.nblk - 1, BLK) + bytes(20)
        elif op == 0x23:
            data = bytes([0, 0, 0, 8]) + struct.pack(">I", d.nblk) + bytes([0x02, 0, 0x02, 0])
        elif op in (0x1A, 0x5A):
            wp = 0x80 if d.ro else 0
            page = cb[2] & 0x3F
            extra = f"page={page:02x}"
            if op == 0x1A:
                data = bytes([3, 0, wp, 0])
            else:
                data = bytes([0, 6, 0, wp, 0, 0, 0, 0])
        elif op in (0x1B, 0x1E, 0x35, 0x2F):
            extra = cb[:6].hex()
        elif op == 0x28:
            lba, n = struct.unpack(">IxH", cb[2:9])
            extra = f"lba={lba} n={n}"
            if lba + n > d.nblk:
                status = 1
                d.set_sense(5, 0x21, 0)
            else:
                data = os.pread(d.fd, n * BLK, lba * BLK)
                self.rd_blocks += n
        elif op == 0x2A:
            lba, n = struct.unpack(">IxH", cb[2:9])
            extra = f"lba={lba} n={n}"
            payload = self.read_exact(dlen)
            if d.ro:
                status = 1
                d.set_sense(7, 0x27, 0)
            else:
                os.pwrite(d.fd, payload[:n * BLK], lba * BLK)
            log(f"#{self.n} {name} {extra} dlen={dlen} st={status}")
            self.csw(tag, max(0, dlen - len(payload)), status)
            return
        else:
            status = 1
            d.set_sense(5, 0x20, 0)
            extra = cb.hex()

        if din and dlen:
            out = data[:dlen]
            residue = dlen - len(out)
            if out:
                os.write(self.ep_in, out)
            else:
                os.write(self.ep_in, b"")  # no data -> ZLP, then CSW
            if out and residue and status == 0 and len(out) < dlen and len(out) % 64 == 0:
                os.write(self.ep_in, b"")  # ZLP to terminate the transfer
        else:
            residue = dlen if (not din and dlen and op != 0x2A) else 0
            if not din and dlen:
                self.read_exact(dlen)
        quiet = op == 0x28 and status == 0
        if not quiet or self.n < 400:
            log(f"#{self.n} {name} {extra} dlen={dlen} {'IN' if din else 'OUT'} sent={len(data[:dlen]) if din else 0} st={status}")
        self.csw(tag, residue, status)


# ---------------------------------------------------------------- ep0 (events + class requests)
EV = {0: "BIND", 1: "UNBIND", 2: "ENABLE", 3: "DISABLE", 4: "SETUP", 5: "SUSPEND", 6: "RESUME"}


def ep0_loop(ep0, bot):
    worker = None
    while True:
        ev = os.read(ep0, 12 * 4)
        for i in range(0, len(ev), 12):
            rt, rq, wv, wi, wl, typ = struct.unpack("<BBHHHB", ev[i:i + 9])
            name = EV.get(typ, typ)
            if typ == 4:
                log(f"SETUP bmRequestType={rt:02x} bRequest={rq:02x} wValue={wv:04x} wIndex={wi} wLength={wl}")
                if rt == 0xA1 and rq == 0xFE:  # GET_MAX_LUN
                    os.write(ep0, b"\x00")
                elif rt == 0x21 and rq == 0xFF:  # Bulk-Only Reset
                    os.read(ep0, 0)
                elif rt & 0x80:
                    try:
                        os.write(ep0, b"")
                    except OSError:
                        pass
                else:
                    try:
                        os.read(ep0, wl)
                    except OSError:
                        pass
            else:
                log(f"EVENT {name}")
                if typ == 2 and (worker is None or not worker.is_alive()):
                    worker = threading.Thread(target=run_bulk, args=(bot,), daemon=True)
                    worker.start()


def run_bulk(bot):
    try:
        bot.open_eps()
        log("bulk endpoints open")
        bot.loop()
    except OSError as e:
        log(f"bulk stopped: {errno.errorcode.get(e.errno, e.errno)} (after {bot.n} commands, READ blocks {bot.rd_blocks})")
        for fd in (bot.ep_in, bot.ep_out):
            try:
                os.close(fd)
            except Exception:
                pass


def udc_watch():
    p = [x for x in os.listdir("/sys/class/udc")][0]
    last = None
    while True:
        try:
            s = open(f"/sys/class/udc/{p}/state").read().strip()
            sp = open(f"/sys/class/udc/{p}/current_speed").read().strip()
        except OSError:
            s, sp = "?", "?"
        if (s, sp) != last:
            log(f"UDC state={s} speed={sp}")
            last = (s, sp)
        time.sleep(0.05)


def main():
    global _lf
    ap = argparse.ArgumentParser()
    ap.add_argument("--vid", default="0xabcd")
    ap.add_argument("--pid", default="0x1234")
    ap.add_argument("--serial", default="0123456789AB")
    ap.add_argument("--ro", action="store_true")
    a = ap.parse_args()
    _lf = open(LOG, "a")
    log(f"==== kl-msc start vid={a.vid} pid={a.pid} ro={a.ro} boot={open('/proc/sys/kernel/random/boot_id').read()[:8]}")
    setup_gadget(a)
    ep0 = os.open(f"{FFS}/ep0", os.O_RDWR)
    os.write(ep0, descriptors())
    os.write(ep0, strings())
    bot = BOT(Disk(IMG, a.ro))
    threading.Thread(target=udc_watch, daemon=True).start()
    udc = os.listdir("/sys/class/udc")[0]
    w(f"{G}/UDC", udc)
    log(f"gadget bound to {udc}, {bot.d.nblk} blocks")
    try:
        ep0_loop(ep0, bot)
    finally:
        teardown()


if __name__ == "__main__":
    main()
