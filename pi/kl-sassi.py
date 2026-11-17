#!/usr/bin/python3 -I
"""kl-sassi - a Pi Zero pretending to be the Kaffelogic "WiFi module" for Kaffelogic Studio (SASSI protocol, TCP 9056).

Studio connects as a client; we present ourselves as a roaster behind a TCP bridge and serve the files of the stick
image (/var/lib/kaffelogic/nano7.img) that the real roaster sees through kl-msc. Protocol notes: docs/PROTOCOL.md
(derived from observing/analysing Studio 7.6.4 for interoperability).

Reading: straight from the image (our own read-only FAT32 reader) while the roaster has it attached.
Writing (profiles / log deletion): stop kl-msc -> loop mount -> change -> umount -> start kl-msc
(the roaster sees the stick being unplugged and re-plugged). If the roaster is writing (roasting) -> BUSY.
Anything outside the profile/log folders goes to an overlay directory OUTSIDE the image (never firmware onto the stick).
Writes to the image are OFF by default (KL_WRITE=0).
"""
import base64, binascii, datetime, os, select, shutil, socket, struct, subprocess, sys, threading, time

IMG = os.environ.get("KL_IMG", "/var/lib/kaffelogic/nano7.img")
STATE = os.environ.get("KL_STATE", "/var/lib/kl-sassi")
LOG = os.environ.get("KL_LOG", "/var/log/kl-sassi.log")
MSC_LOG = os.environ.get("KL_MSC_LOG", "/var/log/kl-msc.log")
MSC_UNIT = os.environ.get("KL_MSC_UNIT", "kl-msc")          # "" = do not stop anything (tests)
PORT = int(os.environ.get("KL_PORT", "9056"))
SERIAL = os.environ.get("KL_SERIAL", "KLZERO0001")
IMG_WRITE = os.environ.get("KL_WRITE", "0") == "1"           # 0 = never write/delete inside the stick image (answers DENIED)
KEEPALIVE = os.environ.get("KL_KEEPALIVE", "bridge")        # bridge = BRIDGE_NOTIFY_STATUS, cr = bare \r
MNT = STATE + "/mnt"
OVERLAY = STATE + "/overlay"
TRASH = STATE + "/trash"

PROFILE_DIR = "kaffelogic/roast-profiles"
LOG_DIR = "kaffelogic/roast-logs"
IMAGE_DIRS = (PROFILE_DIR, LOG_DIR)
MAX_PACKET = 8192
BUSY_WINDOW_S = 90          # a WRITE10 from the roaster within the last N s = it is roasting/writing
WRITE_IDLE_S = 3.0          # this many s without a new write -> umount and hand the stick back to the roaster

# packet types
ACK, CONN_REQ, TIME_SYNC, TIME_SYNC_ACK = 1, 2, 3, 4
DIR_REQ, DIR_RESP, FILE_REQ, FILE_RESP, FILE_PUSH, FILE_PUSH_ACK = 5, 6, 7, 8, 9, 10
FILE_DEL, FILE_DEL_ACK, INFO_REQ, INFO_RESP, ACTION, ACTION_ACK = 11, 12, 13, 14, 15, 16
NOTIFY_STATUS, NOTIFY_STATUS_ACK, NOTIFY_FILE, NOTIFY_FILE_ACK = 30, 31, 32, 33
B_FILE_PUSH, B_FILE_PUSH_ACK, B_INFO_REQ, B_INFO_RESP, B_ACTION, B_ACTION_ACK = 109, 110, 113, 114, 115, 116
B_NOTIFY, B_NOTIFY_ACK = 130, 131
# outcome
OK, NOT_VALID, NOT_FOUND, SYS_ERR, DENIED, CANNOT_SAVE, CANNOT_DELETE, SEQ_ERR, BUSY, LAST = 0, 2, 3, 4, 30, 50, 53, 101, 103, 128
# info
INFO_BUSY, INFO_NOT_BUSY = 6, 7
INFO_FOLDER_UPDATED, INFO_SYSTEM, INFO_FILESYS, INFO_OPSTATUS, INFO_BRIDGE_STATUS = 1, 3, 4, 9, 100
ACT_CONN_CONTROL, ACT_FS_CLEANUP = 4, 110

_lf = None
_llock = threading.Lock()


def log(*a):
    line = time.strftime("%m-%d %H:%M:%S ") + " ".join(str(x) for x in a)
    with _llock:
        if _lf:
            _lf.write(line + "\n"); _lf.flush()
    print(line, flush=True)


# ------------------------------------------------------------------ CRC / dates
def crc16(data, crc=0):
    return binascii.crc_hqx(data, crc)      # CRC-16/XMODEM (poly 0x1021), same as Studio's crc_xmodem_update


def sassi_dt(t):
    """datetime → YYYYMMDDwHHMMSS (w = isoweekday % 7)."""
    return f"{t.year:04d}{t.month:02d}{t.day:02d}{t.isoweekday() % 7}{t.hour:02d}{t.minute:02d}{t.second:02d}"


def parse_dt(s):
    return datetime.datetime(int(s[0:4]), int(s[4:6]), int(s[6:8]), int(s[9:11]), int(s[11:13]), int(s[13:15]))


# ------------------------------------------------------------------ FAT32 (read-only)
class Fat:
    """Minimal FAT32 reader (superfloppy). Re-reads BPB/FAT on every use: the roaster writes underneath us."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY)
        b = os.pread(self.fd, 512, 0)
        self.bps, self.spc, self.rsv, self.nfat = struct.unpack_from("<HBHB", b, 11)
        self.fatsz, = struct.unpack_from("<I", b, 36)
        self.root, = struct.unpack_from("<I", b, 44)
        self.fsinfo, = struct.unpack_from("<H", b, 48)
        tot16, = struct.unpack_from("<H", b, 19)
        tot32, = struct.unpack_from("<I", b, 32)
        self.total = tot16 or tot32
        self.csz = self.bps * self.spc
        self.data0 = (self.rsv + self.nfat * self.fatsz) * self.bps
        self.nclus = (self.total * self.bps - self.data0) // self.csz
        if self.bps not in (512, 1024, 2048, 4096) or not self.spc or not self.fatsz:
            raise OSError("not FAT32")

    def close(self):
        os.close(self.fd)

    def _next(self, c):
        v, = struct.unpack("<I", os.pread(self.fd, 4, self.rsv * self.bps + 4 * c))
        return v & 0x0FFFFFFF

    def _chain(self, c, limit=1 << 22):
        out = []
        while 2 <= c < 0x0FFFFFF8 and len(out) < limit:
            out.append(c); c = self._next(c)
        return out

    def _read_chain(self, c, size=None):
        buf = bytearray()
        for k in self._chain(c):
            buf += os.pread(self.fd, self.csz, self.data0 + (k - 2) * self.csz)
            if size is not None and len(buf) >= size:
                break
        return bytes(buf if size is None else buf[:size])

    def _entries(self, cluster):
        raw = self._read_chain(cluster)
        lfn = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []; continue
            attr = e[11]
            if attr & 0x3F == 0x0F:
                lfn.append((e[0] & 0x3F, e[1:11] + e[14:26] + e[28:32])); continue
            if attr & 0x08:
                lfn = []; continue
            if lfn:
                name = b"".join(p for _, p in sorted(lfn)).decode("utf-16-le", "replace")
                name = name.split("\x00")[0].rstrip("￿")
            else:
                base = e[0:8].decode("cp437", "replace").rstrip()
                ext = e[8:11].decode("cp437", "replace").rstrip()
                if e[12] & 0x08: base = base.lower()
                if e[12] & 0x10: ext = ext.lower()
                name = base + ("." + ext if ext else "")
            lfn = []
            if name in (".", ".."):
                continue
            hi, = struct.unpack_from("<H", e, 20)
            tm, dt, lo, size = struct.unpack_from("<HHHI", e, 22)
            try:
                mt = datetime.datetime(1980 + (dt >> 9), (dt >> 5) & 15 or 1, dt & 31 or 1,
                                       tm >> 11, (tm >> 5) & 63, min((tm & 31) * 2, 59))
            except ValueError:
                mt = datetime.datetime(1980, 1, 1)
            yield {"name": name, "dir": bool(attr & 0x10), "cluster": (hi << 16) | lo, "size": size, "mtime": mt}

    def _find(self, path):
        cur = {"name": "", "dir": True, "cluster": self.root, "size": 0, "mtime": datetime.datetime(1980, 1, 1)}
        for part in [p for p in path.split("/") if p]:
            if not cur["dir"]:
                return None
            for e in self._entries(cur["cluster"]):
                if e["name"].lower() == part.lower():
                    cur = e; break
            else:
                return None
        return cur

    def listdir(self, path):
        d = self._find(path)
        if d is None or not d["dir"]:
            return None
        return list(self._entries(d["cluster"]))

    def read(self, path):
        e = self._find(path)
        if e is None or e["dir"]:
            return None
        return self._read_chain(e["cluster"], e["size"]) if e["size"] else b"", e["mtime"]

    def usage(self):
        """(used, size) in bytes - scans the FAT (FSInfo may be stale)."""
        import array
        a = array.array("I")
        a.frombytes(os.pread(self.fd, 4 * (self.nclus + 2), self.rsv * self.bps))
        free = a[2:].count(0)
        return (self.nclus - free) * self.csz, self.nclus * self.csz


# ------------------------------------------------------------------ storage (image + overlay)
def norm(path):
    parts = [p for p in path.replace("\\", "/").split("/") if p not in ("", ".")]
    if any(p == ".." for p in parts):
        raise ValueError("..")
    return "/".join(parts)


def in_image(path):
    low = path.lower()
    return any(low == d or low.startswith(d + "/") for d in IMAGE_DIRS)


def roaster_busy(window=BUSY_WINDOW_S):
    """Did the roaster write to the stick within the last `window` seconds? (last WRITE10 line of kl-msc.log)"""
    try:
        with open(MSC_LOG, "rb") as f:
            f.seek(0, 2); n = f.tell(); f.seek(max(0, n - 65536))
            tail = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return False
    for line in reversed(tail):
        if "WRITE10" in line:
            try:
                h, m, s = (int(x) for x in line[:8].split(":"))
            except ValueError:
                return False
            now = time.localtime()
            age = (now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec - (h * 3600 + m * 60 + s)) % 86400
            return age < window
    return False


class Store:
    def __init__(self):
        self.mounted = False
        self.last_write = 0.0
        self.lock = threading.RLock()

    # --- reading
    def listdir(self, path):
        """-> [(name, is_dir, mtime, size)] or None if it does not exist."""
        with self.lock:
            if in_image(path):
                if self.mounted:
                    p = self._mp(path)
                    if p is None or not os.path.isdir(p):
                        return None
                    out = []
                    for e in os.scandir(p):
                        st = e.stat()
                        out.append((e.name, e.is_dir(), datetime.datetime.utcfromtimestamp(int(st.st_mtime)), st.st_size))
                    return out
                f = Fat(IMG)
                try:
                    l = f.listdir(path)
                finally:
                    f.close()
                return None if l is None else [(e["name"], e["dir"], e["mtime"], e["size"]) for e in l]
            p = os.path.join(OVERLAY, path)
            if not os.path.isdir(p):
                return None
            out = []
            for e in os.scandir(p):
                st = e.stat()
                out.append((e.name, e.is_dir(), datetime.datetime.utcfromtimestamp(int(st.st_mtime)), st.st_size))
            return out

    def read(self, path):
        """-> (bytes, mtime) or None."""
        with self.lock:
            if in_image(path):
                if self.mounted:
                    p = self._mp(path)
                    if p is None or not os.path.isfile(p):
                        return None
                    with open(p, "rb") as fh:
                        return fh.read(), datetime.datetime.utcfromtimestamp(int(os.stat(p).st_mtime))
                f = Fat(IMG)
                try:
                    return f.read(path)
                finally:
                    f.close()
            p = os.path.join(OVERLAY, path)
            if not os.path.isfile(p):
                return None
            with open(p, "rb") as fh:
                return fh.read(), datetime.datetime.utcfromtimestamp(int(os.stat(p).st_mtime))

    def usage(self):
        with self.lock:
            if self.mounted:
                s = os.statvfs(MNT)
                return (s.f_blocks - s.f_bfree) * s.f_frsize, s.f_blocks * s.f_frsize
            f = Fat(IMG)
            try:
                return f.usage()
            finally:
                f.close()

    # --- writing
    def _mp(self, path):
        """Path inside the mount, case-insensitive (like FAT)."""
        cur = MNT
        for part in path.split("/"):
            try:
                names = os.listdir(cur)
            except OSError:
                return None
            hit = [n for n in names if n.lower() == part.lower()]
            cur = os.path.join(cur, hit[0] if hit else part)
        return cur

    def _begin(self):
        if self.mounted:
            return OK
        if not IMG_WRITE:
            log("write to the image requested but KL_WRITE=0 -> denied")
            return DENIED
        if roaster_busy():
            return BUSY
        os.makedirs(MNT, exist_ok=True)
        if MSC_UNIT:
            subprocess.run(["systemctl", "stop", MSC_UNIT], check=False)
        r = subprocess.run(["mount", "-o", "loop,rw,tz=UTC,shortname=mixed,utf8,flush", IMG, MNT],
                           capture_output=True, text=True)
        if r.returncode:
            log("mount failed:", r.stderr.strip())
            if MSC_UNIT:
                subprocess.run(["systemctl", "start", MSC_UNIT], check=False)
            return SYS_ERR
        self.mounted = True
        log("image: mounted for writing (the roaster temporarily lost the stick)")
        return OK

    def finish(self, force=False):
        """umount + hand the stick back to the roaster once writes have settled."""
        with self.lock:
            if not self.mounted or (not force and time.monotonic() - self.last_write < WRITE_IDLE_S):
                return
            subprocess.run(["sync"], check=False)
            for _ in range(5):
                if subprocess.run(["umount", MNT], capture_output=True).returncode == 0:
                    break
                time.sleep(0.5)
            else:
                log("umount failed - NOT starting kl-msc while the image is mounted"); return
            self.mounted = False
            if MSC_UNIT:
                subprocess.run(["systemctl", "start", MSC_UNIT], check=False)
            log("image: unmounted, the stick is back with the roaster")

    def write(self, path, data, mtime):
        with self.lock:
            if in_image(path):
                low = path.lower()
                if not (low.startswith(PROFILE_DIR + "/") and low.endswith(".kpro")):
                    return DENIED
                r = self._begin()
                if r != OK:
                    return r
                p = self._mp(path)
            else:
                p = os.path.join(OVERLAY, path)
            try:
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p + ".tmp~", "wb") as fh:
                    fh.write(data)
                os.replace(p + ".tmp~", p)
                ts = mtime.replace(tzinfo=datetime.timezone.utc).timestamp()
                os.utime(p, (ts, ts))
            except OSError as e:
                log("write failed", path, e); return CANNOT_SAVE
            finally:
                self.last_write = time.monotonic()
            return OK

    def delete(self, path):
        with self.lock:
            if in_image(path):
                low = path.lower()
                if not ((low.startswith(PROFILE_DIR + "/") and low.endswith(".kpro")) or
                        (low.startswith(LOG_DIR + "/") and low.endswith(".klog"))):
                    return DENIED
                got = self.read(path)
                if got is None:
                    return NOT_FOUND
                r = self._begin()
                if r != OK:
                    return r
                os.makedirs(TRASH, exist_ok=True)       # keep a copy before every delete
                with open(os.path.join(TRASH, time.strftime("%Y%m%d-%H%M%S-") + os.path.basename(path)), "wb") as fh:
                    fh.write(got[0])
                p = self._mp(path)
            else:
                p = os.path.join(OVERLAY, path)
                if not os.path.isfile(p):
                    return NOT_FOUND
            try:
                os.remove(p)
            except OSError as e:
                log("delete failed", path, e); return CANNOT_DELETE
            finally:
                self.last_write = time.monotonic()
            return OK


# ------------------------------------------------------------------ SASSI session
class Closed(Exception):
    pass


class Session:
    def __init__(self, sock, peer, store):
        self.s, self.peer, self.store = sock, peer, store
        self.buf = b""
        self.t0 = time.monotonic()
        self.crc0 = 0
        self.ts_base = 0            # after TIME_SYNC our packet timestamps follow Studio's clock
        self.synced = False
        self.last_tx = 0.0
        self.last_connreq = 0.0
        self.push = {}              # path → [expected_seq, bytearray]
        self.sig = None
        self.sig_at = 0.0
        self.pending = []           # packets that arrived while we were waiting for an ACK
        self.live = None            # live log: {"path", "sent"}
        self.live_seen = None       # (path, size) of the newest log at the previous check
        self.live_at = 0.0

    # --- wire
    def send(self, typ, *fields):
        body = f"KL*{typ}|{self.ts_base + int((time.monotonic() - self.t0) * 1000):x}|".encode()
        if fields:
            body += "|".join(str(f) for f in fields).encode("utf-8") + b"|"
        pkt = body + f"{crc16(body, self.crc0):04x}".encode() + b"\r"
        try:
            self.s.sendall(pkt)
        except OSError as e:
            raise Closed(str(e))
        self.last_tx = time.monotonic()
        if typ not in (B_NOTIFY, NOTIFY_FILE, FILE_RESP, DIR_RESP):      # not the data packets: keeps the log small (it is written to the SD card)
            log(">>", pkt[:160].decode("utf-8", "replace").rstrip("\r") + ("…" if len(pkt) > 160 else ""))

    def recv_packet(self, timeout):
        """-> list of fields (parts) or None on timeout. Empty packet (bare '\\r') -> []."""
        end = time.monotonic() + timeout
        while b"\r" not in self.buf:
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.s], [], [], left)
            if not r:
                return None
            try:
                d = self.s.recv(65536)
            except OSError as e:
                raise Closed(str(e))
            if not d:
                raise Closed("eof")
            self.buf += d
        raw, self.buf = self.buf.split(b"\r", 1)
        raw = raw.strip(b"\n")
        if not raw:
            return []
        if b"|" not in raw or not raw.startswith(b"KL*"):
            log("<< ;;", raw[:80]); return []
        if f"{crc16(raw[:-4], self.crc0):04x}".encode() != raw[-4:].lower():
            log("<< bad CRC", raw[:120]); return []
        parts = raw.decode("utf-8", "replace").split("|")
        if parts[0] not in ("KL*%d" % ACK, "KL*%d" % B_NOTIFY_ACK):
            log("<<", raw[:160].decode("utf-8", "replace") + ("…" if len(raw) > 160 else ""))
        return parts

    def wait_ack(self):
        end = time.monotonic() + 10
        while time.monotonic() < end:
            p = self.recv_packet(end - time.monotonic())
            if p is None:
                break
            if p and p[0] == "KL*%d" % ACK:
                return True
            if p:
                self.pending.append(p)
        log("timeout waiting for PACKET_ACK")
        return False

    def send_multi(self, typ, data, mk, name=""):
        """Multi-packet: mk(outcome, seq, b64) -> fields. Studio ACKs after every non-final packet."""
        can_fit = max(48, ((MAX_PACKET - (3 + 3 + 1 + 16 + 1 + len(name.encode()) + 1 + 3 + 1 + 15 + 1 + 5 + 1 + 4 + 1)) * 3) // 4)
        chunks = [data[i:i + can_fit] for i in range(0, len(data), can_fit)] or [b""]
        for i, ch in enumerate(chunks):
            last = i == len(chunks) - 1
            self.send(typ, *mk(LAST if last else OK, i + 1, base64.b64encode(ch).decode()))
            if not last and not self.wait_ack():
                return False
        return True

    # --- handlers
    def conn_request(self):
        self.last_connreq = time.monotonic()
        # platform 11 = TCP bridge; capabilities bit7 = "user info" with style 0 (Kaffelogic)
        self.send(CONN_REQ, 11, 128, SERIAL, 1, "Nano 7", "kaffelogic.com", "Kaffelogic Nano 7 (stick via Pi Zero)",
                  MAX_PACKET, 255, f"{self.crc0:x}")

    def firmware(self):
        """Roaster firmware version as the roaster itself writes it in the most recent log (highest logNNNN.klog)."""
        import re
        try:
            names = [n for n, d, _, _ in (self.store.listdir(LOG_DIR) or []) if not d and re.fullmatch(r"log\d+\.klog", n, re.I)]
            if not names:
                return None
            got = self.store.read(LOG_DIR + "/" + max(names, key=lambda n: int(re.sub(r"\D", "", n))))
            m = re.search(rb"^firmware_version:([0-9][0-9.]*)\s*$", got[0][:4000], re.M) if got else None
            return m.group(1).decode() if m else None
        except (OSError, ValueError) as e:
            log("firmware():", e); return None

    def info(self, code):
        if code == INFO_OPSTATUS:
            return f"sassi_file_lock:{1 if roaster_busy() else 0}"
        if code == INFO_SYSTEM:
            fw = self.firmware()                           # from the latest klog; if not found, NO 'firmware' key
            return f"model:Nano 7;serial:{SERIAL}" + (f";firmware:{fw}" if fw else "")
        if code == INFO_FILESYS:
            used, size = self.store.usage()
            return f"used:{used};size:{size}"
        return ""

    def handle(self, p):
        typ = int(p[0][3:])
        f = p[2:-1]                                        # fields 2..N
        g = lambda i: f[i - 2] if len(f) > i - 2 else ""   # field by its protocol number
        if typ == TIME_SYNC:
            self.synced = True
            try:
                self.ts_base = int(p[1], 16) - int((time.monotonic() - self.t0) * 1000)
            except ValueError:
                pass
            self.send(TIME_SYNC_ACK)
            log(f"Studio connected from {self.peer} (Studio time {g(4)})")
        elif typ == DIR_REQ:
            path = norm(g(2))
            l = self.store.listdir(path)
            if l is None:
                self.send(DIR_RESP, path, NOT_FOUND, g(4), 0, ""); return
            body = "\r".join(f"{'>' if d else ' '}\t{n}\t{sassi_dt(m)}\t{sz}" for n, d, m, sz in l
                             if not n.startswith(".") and not n.endswith("~")).encode("utf-8")
            self.send_multi(DIR_RESP, body, lambda oc, seq, b: (path, oc, g(4), seq, b), path)
        elif typ == FILE_REQ:
            path = norm(g(2))
            got = self.store.read(path)
            if got is None:
                self.send(FILE_RESP, path, NOT_FOUND, "", 0, ""); return
            data, mt = got
            self.send_multi(FILE_RESP, data, lambda oc, seq, b: (path, oc, sassi_dt(mt), seq, b), path)
        elif typ in (FILE_PUSH, B_FILE_PUSH):
            ack = FILE_PUSH_ACK if typ == FILE_PUSH else B_FILE_PUSH_ACK
            path = norm(g(2)); action = int(g(3) or 0); seq = int(g(5) or 1)
            st = self.push.get(path)
            if seq == 1:
                st = self.push[path] = [1, bytearray()]
            if st is None or seq != st[0]:
                self.push.pop(path, None); self.send(ack, path, SEQ_ERR); return
            st[1] += base64.b64decode(g(6)); st[0] += 1
            if not action & LAST:
                self.send(ACK); return
            data = bytes(self.push.pop(path)[1])
            if typ == B_FILE_PUSH:
                oc = DENIED
            else:
                try:
                    mt = parse_dt(g(4))
                except (ValueError, IndexError):
                    mt = datetime.datetime.utcnow()
                oc = self.store.write(path, data, mt)
            log(f"push {path} {len(data)} bytes → outcome {oc}")
            self.send(ack, path, oc)
        elif typ == FILE_DEL:
            path = norm(g(2))
            oc = self.store.delete(path)
            log(f"delete {path} → outcome {oc}")
            self.send(FILE_DEL_ACK, path, oc)
        elif typ == INFO_REQ:
            code = int(g(3) or 0)
            self.send(INFO_RESP, self.info(code), code)
        elif typ == B_INFO_REQ:
            code = int(g(3) or 0)
            self.send(B_INFO_RESP, f"rssi:-50;fw_version:9.9.9;model_number:Pi Zero W (kl-sassi);serial_number:{SERIAL}" if code in (100, 101) else "", code)
        elif typ == ACTION:
            code = int(g(3) or 0)
            if code == ACT_FS_CLEANUP:
                self.send(ACTION_ACK, 0, OK, "")
            elif code == ACT_CONN_CONTROL:
                self.send(ACTION_ACK, g(2), OK, "")
            else:
                log(f"ACTION {code} data={g(2)!r} -> denied (not implemented)")
                self.send(ACTION_ACK, g(2), DENIED, "")
        elif typ == B_ACTION:
            code = int(g(3) or 0)
            self.send(B_ACTION_ACK, g(2), OK if code == ACT_CONN_CONTROL else DENIED, "")
            if code == ACT_CONN_CONTROL:
                raise Closed("Studio is closing the connection")
        elif typ in (ACK, NOTIFY_STATUS_ACK, NOTIFY_FILE_ACK, B_NOTIFY_ACK):
            pass
        else:
            log("unknown packet type", typ)

    def live_tick(self):
        """Live log: the roaster writes the klog incrementally (measured: 1 line/s, a WRITE10 every ~0.5 s).
        We send BUSY, then NOTIFY_FILE: first the whole file (it starts with log_file_name:), then only new lines.
        When writes stop for 20 s: NOT_BUSY with the path -> Studio downloads the final file."""
        now = time.monotonic()
        if not self.synced or now - self.live_at < 1.0 or self.store.mounted:
            return
        self.live_at = now
        if not roaster_busy(20):
            if self.live:
                log(f"live: end {self.live['path']} ({self.live['sent']} bytes sent)")
                self.send(NOTIFY_STATUS, self.live["path"], INFO_NOT_BUSY)
                self.live = None
            self.live_seen = None
            return
        import re
        names = [(n, sz) for n, d, _, sz in (self.store.listdir(LOG_DIR) or []) if not d and re.fullmatch(r"log\d+\.klog", n, re.I)]
        if not names:
            return
        name, size = max(names, key=lambda x: int(re.sub(r"\D", "", x[0])))
        path = LOG_DIR + "/" + name
        seen, self.live_seen = self.live_seen, (path, size)
        if self.live is None or self.live["path"] != path:
            # start only if we saw the SAME file grow between two checks
            if not (seen and seen[0] == path and size > seen[1]):
                return
        got = self.store.read(path)
        if got is None:
            return
        data = got[0]
        end = data.rfind(b"\n") + 1
        if self.live is None or self.live["path"] != path:
            if not data.startswith(b"log_file_name:"):
                return
            log(f"live: start {path}")
            self.send(NOTIFY_STATUS, "", INFO_BUSY)
            self.live = {"path": path, "sent": 0}
        if end <= self.live["sent"] or b"\0" in data[self.live["sent"]:end]:
            return
        chunk = data[self.live["sent"]:end]
        if self.send_multi(NOTIFY_FILE, chunk, lambda oc, seq, b: (path, oc, "", seq, b), path):
            self.live["sent"] = end

    def dir_signature(self):
        sig = []
        for d in IMAGE_DIRS:
            l = self.store.listdir(d) or []
            sig.append(sorted((n, sz, m) for n, isd, m, sz in l if not isd))
        return sig

    def periodic(self):
        now = time.monotonic()
        if self.synced and now - self.last_tx > 5:
            # While a live log is active we do NOT send BRIDGE_NOTIFY: it makes Studio "renew" the connection, the new
            # session forgets the live state and the final NOT_BUSY is never sent (seen 8 s after the last line).
            if KEEPALIVE == "bridge" and not self.live:
                self.send(B_NOTIFY, "rssi:-50", INFO_BRIDGE_STATUS)
            else:
                self.s.sendall(b"\r"); self.last_tx = now
        if self.synced and now - self.sig_at > 15 and not self.store.mounted and not roaster_busy():
            self.sig_at = now
            try:
                sig = self.dir_signature()
            except OSError as e:
                log("dir_signature:", e); return
            if self.sig is not None and sig != self.sig:
                log("stick folders changed -> NOTIFY FOLDER_UPDATED")
                self.send(NOTIFY_STATUS, "", INFO_FOLDER_UPDATED)
            self.sig = sig

    def run(self):
        self.conn_request()
        while True:
            p = self.pending.pop(0) if self.pending else self.recv_packet(1.0)
            if p == []:
                if not self.synced and time.monotonic() - self.last_connreq > 1.0:
                    self.conn_request()
            elif p:
                try:
                    self.handle(p)
                except (ValueError, IndexError) as e:
                    log("bad packet:", e, p[:4])
            self.periodic()
            try:
                self.live_tick()
            except OSError as e:
                log("live_tick:", e)
            self.store.finish()


# ------------------------------------------------------------------ real roast start time
DATES = STATE + "/roast-dates.json"


def roast_dates_watch():
    """The roaster has no clock (every .klog is dated 2001-01-01). While it is writing a log we record when that
    roast STARTED = now - (seconds in the last data line). Only with an NTP-synchronised clock. Independent of Studio.
    State: roast-dates.json {logNNNN.klog: {"start": epoch, "iso": "..."}}."""
    import json, re
    while True:
        time.sleep(5)
        try:
            if not roaster_busy(20) or not os.path.exists("/run/systemd/timesync/synchronized"):
                continue
            try:
                dates = json.load(open(DATES))
            except (OSError, ValueError):
                dates = {}
            f = Fat(IMG)
            try:
                names = [e["name"] for e in f.listdir(LOG_DIR) or [] if re.fullmatch(r"log\d+\.klog", e["name"], re.I)]
                if not names:
                    continue
                name = max(names, key=lambda n: int(re.sub(r"\D", "", n)))
                if name in dates:
                    continue
                data = f.read(LOG_DIR + "/" + name)[0]
            finally:
                f.close()
            if not data.startswith(b"log_file_name:"):
                continue
            t = None
            for line in reversed(data[:data.rfind(b"\n")].splitlines()[-5:]):
                m = re.match(rb"(\d+\.\d+)\t", line)
                if m:
                    t = float(m.group(1)); break
            if t is None:
                continue
            start = time.time() - t
            dates[name] = {"start": round(start), "iso": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(start))}
            with open(DATES + ".tmp", "w") as fh:
                json.dump(dates, fh, indent=1, sort_keys=True)
            os.replace(DATES + ".tmp", DATES)
            log(f"roast {name}: started {dates[name]['iso']}")
        except Exception as e:
            log("roast_dates_watch:", type(e).__name__, e)


# ------------------------------------------------------------------ LLMNR: "KaffelogicWM" -> our IP
def llmnr():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", 5355))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton("224.0.0.252") + socket.inet_aton("0.0.0.0"))
    except OSError as e:
        log("LLMNR disabled:", e); return
    while True:
        try:
            q, addr = s.recvfrom(512)
            if len(q) < 17 or q[2] & 0x80:
                continue
            n = q[12]; name = q[13:13 + n].decode("ascii", "replace")
            qtype, = struct.unpack_from(">H", q, 14 + n)
            if name.lower() != "kaffelogicwm" or q[13 + n] != 0 or qtype not in (1, 255):
                continue
            t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); t.connect((addr[0], 9)); ip = t.getsockname()[0]; t.close()
            question = q[12:18 + n]
            ans = q[:2] + b"\x80\x00\x00\x01\x00\x01\x00\x00\x00\x00" + question + \
                b"\xc0\x0c\x00\x01\x00\x01" + struct.pack(">IH", 30, 4) + socket.inet_aton(ip)
            s.sendto(ans, addr)
            log(f"LLMNR: {addr[0]} asked for KaffelogicWM -> {ip}")
        except OSError as e:
            log("LLMNR:", e); time.sleep(1)


def main():
    global _lf
    os.makedirs(OVERLAY, exist_ok=True)
    _lf = open(LOG, "a")
    store = Store()
    if os.path.ismount(MNT):                       # leftover from an abrupt power cut
        store.mounted = True; store.finish(force=True)
    threading.Thread(target=roast_dates_watch, daemon=True).start()
    if "--no-llmnr" not in sys.argv:
        threading.Thread(target=llmnr, daemon=True).start()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", PORT)); srv.listen(2)
    log(f"kl-sassi listening on :{PORT}, image {IMG}, writes to the image: {'YES' if IMG_WRITE else 'no'}")
    while True:
        c, peer = srv.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        ses = Session(c, peer[0], store)
        try:
            ses.run()
        except Closed as e:
            log(f"connection from {peer[0]} closed: {e}")
        except Exception as e:                     # one session must never take the server down
            log(f"session error {peer[0]}: {type(e).__name__}: {e}")
        finally:
            try: c.close()
            except OSError: pass
            store.finish(force=True)


if __name__ == "__main__":
    main()
