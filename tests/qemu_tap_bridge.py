#!/usr/bin/env python3
"""TAP <-> QEMU `-netdev dgram` bridge with a tiny DHCP server, so the guest (10.0.2.77) talks to REAL host services
on 10.0.2.2 (Linux TCP stack; python ssh/ftp/http(s) servers) and the host can connect to servers running in the guest.
usage: qemu_tap_bridge.py   (root + /dev/net/tun needed; QEMU: -netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=7001,
remote.type=inet,remote.host=127.0.0.1,remote.port=7002 -device virtio-net-device,netdev=n0,mac=52:54:00:12:34:56)"""
import fcntl, os, select, socket, struct, sys, time
IF = b"rio1"
HOST_IP, GUEST_IP = "10.0.2.2", "10.0.2.77"
tap = os.open("/dev/net/tun", os.O_RDWR)
fcntl.ioctl(tap, 0x400454ca, struct.pack("16sH", IF, 0x0002 | 0x1000))
cs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sa = lambda ip: struct.pack("H2s4s8x", socket.AF_INET, b"\0\0", socket.inet_aton(ip))
fcntl.ioctl(cs, 0x8916, struct.pack("16s", IF) + sa(HOST_IP))
fcntl.ioctl(cs, 0x891c, struct.pack("16s", IF) + sa("255.255.255.0"))
fcntl.ioctl(cs, 0x8914, struct.pack("16sH", IF, 0x1 | 0x40))
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("127.0.0.1", 7002)); u.setblocking(False)
DEBUG = os.environ.get("RIOBR_DEBUG")
def show(d, f):
    if not DEBUG: return
    et = f[12:14]
    if et == b"\x08\x06": print(d, "ARP op", f[21], flush=True)
    elif et == b"\x08\x00" and f[23] == 6:
        ihl = (f[14] & 15) * 4; t = f[14 + ihl:]
        tcplen = struct.unpack("!H", f[16:18])[0] - ihl
        pseudo = f[26:34] + b"\x00\x06" + struct.pack("!H", tcplen)
        ok = csum(pseudo + f[14 + ihl:14 + ihl + tcplen]) == 0
        print(d, "csum", "ok" if ok else "BAD", "TCP", socket.inet_ntoa(f[26:30]), struct.unpack("!H", t[0:2])[0], "->", socket.inet_ntoa(f[30:34]), struct.unpack("!H", t[2:4])[0], "flags", hex(t[13]), "seq", struct.unpack("!I", t[4:8])[0] % 100000, "ack", struct.unpack("!I", t[8:12])[0] % 100000, "paylen", len(f) - 14 - ihl - (t[12] >> 4) * 4, flush=True)
def send_guest(f):
    show("h->g", f); u.sendto(f, ("127.0.0.1", 7001))
PM = bytes.fromhex("020000000001")

def csum(d):
    if len(d) % 2: d += b"\0"
    t = sum(struct.unpack("!%dH" % (len(d) // 2), d))
    while t >> 16: t = (t & 0xffff) + (t >> 16)
    return (~t) & 0xffff

def dhcp(frame):
    b = frame[14 + 20 + 8:]
    xid = b[4:8]; mac = b[28:34]; opts = b[240:]; t = None; i = 0
    while i < len(opts) and opts[i] != 255:
        if opts[i] == 0: i += 1; continue
        if opts[i] == 53: t = opts[i + 2]
        i += 2 + opts[i + 1]
    if t not in (1, 3): return
    rt = 2 if t == 1 else 5
    r = bytearray(240); r[0] = 2; r[1] = 1; r[2] = 6; r[4:8] = xid; r[10] = 0x80
    r[16:20] = socket.inet_aton(GUEST_IP); r[20:24] = socket.inet_aton(HOST_IP); r[28:34] = mac; r[236:240] = bytes([0x63, 0x82, 0x53, 0x63])
    h = socket.inet_aton(HOST_IP)
    o = bytes([53, 1, rt, 54, 4]) + h + bytes([1, 4, 255, 255, 255, 0, 3, 4]) + h + bytes([6, 4]) + h + bytes([51, 4, 0, 0, 0x0e, 0x10, 255])
    udp = struct.pack("!HHHH", 67, 68, 8 + len(r) + len(o), 0) + bytes(r) + o
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1, 0x4000, 64, 17, 0, h, bytes([255] * 4))
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    send_guest(b"\xff" * 6 + PM + b"\x08\x00" + ip + udp)

while True:
    r, _, _ = select.select([tap, u], [], [], 1.0)
    if tap in r:
        try: send_guest(os.read(tap, 65536))
        except OSError: pass
    if u in r:
        try: f, _ = u.recvfrom(65536)
        except BlockingIOError: continue
        if len(f) >= 14 + 20 + 8 and f[12:14] == b"\x08\x00" and f[23] == 17 and f[36:38] == b"\x00\x43":
            dhcp(f); continue
        show('g->h', f); os.write(tap, f)
