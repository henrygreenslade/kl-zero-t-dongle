#!/usr/bin/python3 -I
"""SASSI test client that behaves like Studio (as far as we understand Studio 7.6.4).
sassi_client.py HOST[:PORT] [ls PATH | get PATH [OUT] | put LOCAL PATH | rm PATH | info CODE | idle SECS]..."""
import base64, datetime, socket, sys, time

def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def dt(t): return f"{t.year:04d}{t.month:02d}{t.day:02d}{t.isoweekday()%7}{t.hour:02d}{t.minute:02d}{t.second:02d}"

class C:
    def __init__(s, host, port):
        s.s = socket.create_connection((host, port), 3); s.s.settimeout(12); s.buf = b""; s.crc0 = None; s.t0 = time.time()
        s.s.sendall(b"\r")
        p = s.rx()
        assert p[0] == "KL*2" and len(p) == 13, p
        s.maxp = int(p[9]); print("CONNECTION_REQUEST:", p[2:-1])
        s.tx(3, {2: 10, 3: (1 << 8) | 2, 4: dt(datetime.datetime.utcnow()), 5: 1}, 5)
        p = s.rx(); assert p[0] == "KL*4" and len(p) == 3, p; print("TIME_SYNC_ACK ok")
    def tx(s, typ, payload=None, nfields=1):
        body = f"KL*{typ}|{int((time.time()-s.t0)*1000):x}|"
        if payload is not None:
            body += "|".join(str(payload.get(i, "")) for i in range(2, nfields + 1)) + "|"
        b = body.encode(); s.s.sendall(b + f"{crc16(b, s.crc0 or 0):04x}".encode() + b"\r")
    def rx(s, skip=(130,)):
        while True:
            while b"\r" not in s.buf:
                d = s.s.recv(65536)
                if not d: raise SystemExit("EOF from server")
                s.buf += d
            raw, s.buf = s.buf.split(b"\r", 1)
            if not raw: continue
            parts = raw.decode().split("|")
            if parts[0] == "KL*2": s.crc0 = int(parts[11], 16)
            assert f"{crc16(raw[:-4], s.crc0):04x}" == parts[-1], ("CRC", raw[:80])
            t = int(parts[0][3:])
            if t in skip: s.tx(131); continue
            if t == 30: print("NOTIFY_STATUS", parts[2:-1]); continue
            return parts
    def multi(s, resp, nf):
        data = b""; seq = 1
        while True:
            p = s.rx(); assert p[0] == f"KL*{resp}" and len(p) == nf + 2, p[:5]
            oc = int(p[3])
            if oc not in (0, 128): return oc, None, p
            assert int(p[5]) == seq, ("seq", p[5], seq); seq += 1
            data += base64.b64decode(p[6])
            if oc == 128: return 0, data, p
            s.tx(1)
    def ls(s, path):
        s.tx(5, {2: path, 4: 1}, 4); oc, data, p = s.multi(6, 6)
        return oc, [tuple(l.split("\t")) for l in (data or b"").decode().split("\r") if "\t" in l]
    def get(s, path):
        s.tx(7, {2: path}, 2); oc, data, p = s.multi(8, 6); return oc, data, p[4]
    def put(s, data, path, mtime):
        can = ((s.maxp - (3+3+1+16+1+len(path)+1+3+1+15+1+5+1+4+1)) * 3) // 4
        ch = [data[i:i+can] for i in range(0, len(data), can)] or [b""]
        for i, c in enumerate(ch):
            last = i == len(ch) - 1
            s.tx(9, {2: path, 3: 1 | (128 if last else 0), 4: dt(mtime), 5: i + 1, 6: base64.b64encode(c).decode()}, 6)
            p = s.rx()
            if last or p[0] != "KL*1": assert p[0] == "KL*10", p; return int(p[3])
    def rm(s, path):
        s.tx(11, {2: path}, 3); p = s.rx(); assert p[0] == "KL*12", p; return int(p[3])
    def info(s, code):
        s.tx(13, {2: "", 3: code}, 3); p = s.rx(); assert p[0] == "KL*14" and int(p[3]) == code, p; return p[2]

if __name__ == "__main__":
    h, _, pt = sys.argv[1].partition(":"); c = C(h, int(pt or 9056)); a = sys.argv[2:]
    while a:
        cmd = a.pop(0)
        if cmd == "ls":
            oc, l = c.ls(a.pop(0)); print(f"ls outcome={oc} entries={len(l)}"); [print("  ", e) for e in l[:6]]
        elif cmd == "get":
            path = a.pop(0); t = time.time(); oc, d, m = c.get(path)
            print(f"get {path} outcome={oc} bytes={None if d is None else len(d)} mtime={m} {time.time()-t:.2f}s")
            if d is not None: open("/dev/shm/sassi_last_get", "wb").write(d)
        elif cmd == "put":
            loc = a.pop(0); path = a.pop(0); print("put outcome", c.put(open(loc, "rb").read(), path, datetime.datetime(2026, 11, 12, 20, 30, 41)))
        elif cmd == "rm": print("rm outcome", c.rm(a.pop(0)))
        elif cmd == "info": k = int(a.pop(0)); print(f"info {k}:", c.info(k))
        elif cmd == "idle":
            end = time.time() + float(a.pop(0)); c.s.settimeout(1); n = 0
            while time.time() < end:
                try: p = c.rx(skip=()); n += 1; c.tx(131) if p[0] == "KL*130" else None
                except socket.timeout: pass
            print("idle: keepalive packets", n)
