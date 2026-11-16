#!/usr/bin/env python3
"""RioOS device tests on QEMU: USB xHCI (hubs, hot-plug, keyboard/mouse/tablet), virtio-gpu, virtio-sound, virtio-9p.
Every session is skipped (SKIP line, no failure) when this QEMU build lacks the device; virtio-9p needs a QEMU built with virtfs.
usage: python3 tests/qemu_devices.py [path-to-qemu-system-aarch64] [session ...]     sessions: usb gpu sound 9p
Prints PASS/FAIL lines and a final 'devices: N checks, M failed' line (exit 1 on any FAIL)."""
import filecmp, hashlib, json, os, pty, select, shutil, socket, struct, subprocess, sys, tempfile, time, tty, wave, math, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1] in ("usb", "gpu", "sound", "9p") else shutil.which("qemu-system-aarch64") or "qemu-system-aarch64"
ONLY = [a for a in sys.argv[1:] if a in ("usb", "gpu", "sound", "9p")]
KERNEL = os.path.join(ROOT, "build", "kernel.elf")
TMP = tempfile.mkdtemp(prefix="rioos-dev-")
fails = 0
total = 0
_devhelp = None


def have_device(name):
    global _devhelp
    if _devhelp is None:
        args = [QEMU] + (["-L", os.environ["QEMU_BIOS"]] if os.environ.get("QEMU_BIOS") else []) + ["-M", "virt", "-device", "help"]
        _devhelp = subprocess.run(args, capture_output=True, text=True).stdout
    return ('name "%s"' % name) in _devhelp


def have_audiodev(name):
    r = subprocess.run([QEMU, "-audiodev", "help"], capture_output=True, text=True)
    return name in (r.stdout + r.stderr).split()


def check(name, log, *needles):
    global fails, total
    for n in needles:
        total += 1
        if n in log:
            print("  PASS  %s: %s" % (name, n))
        else:
            fails += 1
            print("  FAIL  %s: missing %r" % (name, n))


def ok(name, cond, detail=""):
    global fails, total
    total += 1
    if cond: print("  PASS  %s" % name)
    else: fails += 1; print("  FAIL  %s %s" % (name, detail))


def run(cmds, extra=(), boot=8, wait=2, qmp=False):
    """cmds: list of (text, wait) | text starting with '@' = HMP monitor command(s) separated by ';' | '%' + JSON = QMP command |
    a callable = host-side action. The guest is attached to a fresh copy of storage/disk.img."""
    disk = os.path.join(TMP, "disk.img")
    shutil.copy(os.path.join(ROOT, "storage", "disk.img"), disk)
    mon = os.path.join(TMP, "mon.sock"); qsock = os.path.join(TMP, "qmp.sock")
    for p in (mon, qsock):
        if os.path.exists(p): os.unlink(p)
    args = [QEMU] + (["-L", os.environ["QEMU_BIOS"]] if os.environ.get("QEMU_BIOS") else []) + [
        "-M", "virt,gic-version=2,virtualization=on", "-global", "virtio-mmio.force-legacy=false", "-cpu", "cortex-a72", "-m", "128M",
        "-nographic", "-monitor", "unix:%s,server,nowait" % mon, "-kernel", KERNEL, "-no-reboot",
        "-drive", "if=none,format=raw,file=%s,id=vd0" % disk, "-device", "virtio-blk-device,drive=vd0"]
    if qmp: args += ["-qmp", "unix:%s,server,nowait" % qsock]
    args += list(extra)
    if os.environ.get("RIO_SMP") and "-smp" not in args: args += ["-smp", os.environ["RIO_SMP"]]
    args += os.environ.get("RIO_QEMU_EXTRA", "").split()
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(QEMU, args)
    tty.setraw(fd)

    def rd(t):
        out = b""; end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try: out += os.read(fd, 65536)
                except OSError: break
        return out.decode(errors="replace")

    log = rd(boot)
    ms = socket.socket(socket.AF_UNIX)
    for _ in range(50):
        try: ms.connect(mon); break
        except OSError: time.sleep(0.1)
    time.sleep(0.5); ms.recv(4096)
    qs = None
    if qmp:
        qs = socket.socket(socket.AF_UNIX); qs.connect(qsock); time.sleep(0.3); qs.recv(4096)
        qs.send(b'{"execute":"qmp_capabilities"}\n'); time.sleep(0.3); qs.recv(4096)
    for c, w in cmds:
        if callable(c):
            c(); log += "\n>>> (host action)\n"; time.sleep(0.2); continue
        if c.startswith("%"):
            qs.send((c[1:] + "\n").encode()); time.sleep(0.4); qs.recv(65536)
            log += "\n>>> qmp\n" + rd(w or 0.5); continue
        if c.startswith("@"):
            for part in c[1:].split(";"):
                try: ms.send((part.strip() + "\n").encode())
                except OSError: break
                time.sleep(0.25)
            log += "\n>>> " + c + "\n" + rd(w or wait)
            continue
        os.write(fd, (c + "\r").encode())
        log += "\n>>> " + c + "\n" + rd(w or wait)
    if os.environ.get("DEV_DEBUG"): print(log.replace("\r", ""))
    try: os.kill(pid, 9)
    except OSError: pass
    try: os.waitpid(pid, 0)
    except OSError: pass
    return log


def C(*a):
    return [(c, None) for c in a]


def keys(*ks):
    return "@" + ";".join("sendkey " + k for k in ks)


def qabs(x, y):
    return "%" + json.dumps({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": x}}, {"type": "abs", "data": {"axis": "y", "value": y}}]}})


def qrel(dx, dy):
    return "%" + json.dumps({"execute": "input-send-event", "arguments": {"events": [
        {"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}]}})


def qbtn(b, down):
    return "%" + json.dumps({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"button": b, "down": down}}]}})


def skip(what, why):
    print("SKIP  %s: %s" % (what, why))


# ------------------------------------------------------------------------------------------------------------------
def session_usb():
    print("== usb: xHCI hubs (2 tiers), HID keyboard/mouse/tablet, hot-plug, Caps/NumLock/keypad/repeat")
    for d in ("qemu-xhci", "usb-hub", "usb-kbd", "usb-mouse", "usb-tablet"):
        if not have_device(d): skip("usb", "QEMU lacks " + d); return
    hubs = ["-device", "qemu-xhci,id=x",
            "-device", "usb-hub,bus=x.0,port=1,id=ha", "-device", "usb-kbd,bus=x.0,port=1.1",
            "-device", "usb-hub,bus=x.0,port=1.2,id=hb", "-device", "usb-mouse,bus=x.0,port=1.2.1",
            "-device", "usb-tablet,bus=x.0,port=2"]
    log = run(C("usb", "usb selftest", keys("h", "i", "ret"), qrel(7, 3), qabs(32767, 0), "usb"), hubs, wait=2, qmp=True)
    check("usb-tree", log, "keyboards 1, mice 1, tablets 1, hubs 2", "port 5 speed", "hub (8 ports)", "port 5.1 ", "port 5.2.1 ", "HID keyboard", "HID mouse", "HID tablet",
          "usb selftest: PASS", "transfer errors 0 (recovered 0)")
    check("usb-input", log, "Unknown command: hi", "key events: 3", "mouse: x=7 y=3", "tablet: x=32767 y=0", "pointer events: 2")

    # hot-plug on a hub port and on a root port, hot-unplug, hub removal (children go with it), recovery of a re-used slot
    cmds = C("usb", "@device_add usb-kbd,bus=x.0,port=1.3,id=k1", "usb", keys("o", "k", "ret"), "usb",
             "@device_add usb-mouse,bus=x.0,port=3,id=m1", "usb scan", qrel(5, 6), "usb",
             "@device_del k1", "usb", "@device_del m1", "usb",
             "@device_add usb-kbd,bus=x.0,port=1.3,id=k2", "usb", keys("z", "ret"), "usb",
             "@device_del ha", "usb")
    log = run(cmds, ["-device", "qemu-xhci,id=x", "-device", "usb-hub,bus=x.0,port=1,id=ha"], wait=1.5, qmp=True)
    i = log.index(">>> @device_add usb-kbd,bus=x.0,port=1.3,id=k1")
    parts = log[i:].split(">>> usb")
    check("usb-hotplug", log, "devices 1", "port 5.3 ", "Unknown command: ok", "devices 3", "HID mouse", "mouse: x=5 y=6", "Unknown command: z", "removed 4")
    ok("usb-hotplug: all removed", "devices 0" in parts[-1], parts[-1][:300])
    ok("usb-hotplug: no errors", "transfer errors 0" in log)

    # keyboard layer
    cmds = C(keys("e", "c", "h", "o", "spc", "shift-a", "shift-b", "ret"),
             keys("caps_lock", "e", "c", "h", "o", "spc", "a", "b", "c", "ret"),
             keys("caps_lock", "e", "c", "h", "o", "spc", "shift-a", "d", "ret"),
             keys("e", "c", "h", "o", "spc", "kp_1", "kp_add", "kp_2", "kp_subtract", "kp_3", "kp_multiply", "kp_divide", "kp_decimal", "kp_0", "ret"),
             "echo first", keys("up", "ret"), "echo e1")
    cmds += [("@sendkey x 1800", 3)] + C("ret", "usb")
    log = run(cmds, ["-device", "qemu-xhci,id=x", "-device", "usb-kbd,bus=x.0"], wait=1.5)
    check("usb-keys", log, "echo AB", "ECHO ABC", "echo Ad", "echo 1+2-3*/.0", "Unknown command: xxxxxxxxxxxxxxxxxxxx", "transfer errors 0")
    ok("usb-keys: history (Up)", log.count("first") >= 3, "")


def session_gpu():
    print("== gpu: virtio-gpu 2D scanout")
    if not have_device("virtio-gpu-device"): skip("gpu", "QEMU lacks virtio-gpu-device"); return
    shot = os.path.join(TMP, "shot.ppm")
    log = run(C("fbinfo", "echo HELLO-GPU", "ls /", "@screendump " + shot, "fbinfo"), ["-device", "virtio-gpu-device"], wait=2)
    check("gpu", log, "Driver  : virtio-gpu (host scanout", "enabled", "Geometry: 1024x768 pixels", "HELLO-GPU")
    ok("gpu: flushes advance", re.search(r"flushes (\d+)\)", log) is not None and int(re.findall(r"flushes (\d+)\)", log)[-1]) > int(re.findall(r"flushes (\d+)\)", log)[0]))
    try:
        d = open(shot, "rb").read()
        hdr = d.split(b"\n", 3)
        w, h = map(int, hdr[1].split())
        px = hdr[3]
        lit = sum(1 for i in range(0, len(px), 3) if px[i] or px[i + 1] or px[i + 2])
        ok("gpu: screendump is the guest terminal (1024x768, text pixels present)", (w, h) == (1024, 768) and lit > 5000, "size %dx%d lit %d" % (w, h, lit))
    except Exception as e:
        ok("gpu: screendump readable", False, str(e))
    # without any display device the kernel stays on the serial console
    log = run(C("fbinfo"), [], wait=2)
    check("gpu-none", log, "Backend : none (serial console only)")


def make_wav(path, rate, ch, secs, freq, bits=16):
    w = wave.open(path, "wb"); w.setnchannels(ch); w.setsampwidth(bits // 8); w.setframerate(rate)
    n = int(rate * secs); data = bytearray()
    for i in range(n):
        v = math.sin(2 * math.pi * freq * i / rate)
        data += (struct.pack("<h", int(v * 12000)) if bits == 16 else struct.pack("B", int(128 + v * 100))) * ch
    w.writeframes(bytes(data)); w.close()


def session_sound():
    print("== sound: virtio-sound PCM playback (beep + WAV from a 9p share)")
    if not have_device("virtio-sound-device"): skip("sound", "QEMU lacks virtio-sound-device"); return
    if not have_audiodev("wav"): skip("sound", "QEMU lacks the wav audio backend"); return
    out = os.path.join(TMP, "snd_out.wav")
    share = os.path.join(TMP, "snd_share"); os.makedirs(share, exist_ok=True)
    make_wav(os.path.join(share, "st44.wav"), 44100, 2, 0.5, 440)
    make_wav(os.path.join(share, "mono22.wav"), 22050, 1, 0.4, 660)
    make_wav(os.path.join(share, "st48.wav"), 48000, 2, 0.3, 880)
    make_wav(os.path.join(share, "u8.wav"), 8000, 1, 0.2, 440, bits=8)
    make_wav(os.path.join(share, "r12k.wav"), 12000, 1, 0.1, 440)          # rate the device does not offer
    make_wav(os.path.join(share, "ch4.wav"), 44100, 4, 0.1, 440)           # more channels than the device has
    open(os.path.join(share, "bad.wav"), "wb").write(b"not a wav file at all" * 10)
    open(os.path.join(share, "empty.wav"), "wb").close()
    extra = ["-audiodev", "wav,id=a,path=%s" % out, "-device", "virtio-sound-device,audiodev=a"]
    have9p = have_device("virtio-9p-device")
    if have9p: extra += ["-fsdev", "local,id=fs0,path=%s,security_model=none" % share, "-device", "virtio-9p-device,fsdev=fs0,mount_tag=snd"]
    cmds = C("sound info")
    if have9p:
        cmds += C("su", "123", "mkdir -p /mnt/s", "mount9p snd /mnt/s", "logout", "sound play /mnt/s/st44.wav", "sound play /mnt/s/mono22.wav", "sound play /mnt/s/st48.wav",
                  "sound play /mnt/s/u8.wav", "sound play /mnt/s/r12k.wav", "sound play /mnt/s/ch4.wav", "sound play /mnt/s/bad.wav", "sound play /mnt/s/empty.wav", "sound play /mnt/s/nofile.wav")
    cmds += C("sound beep 440 100", "sound beep 99999", "sound beep x", "sound play", "sound info", "@quit")
    log = run(cmds, extra, wait=3)
    check("sound-info", log, "virtio-sound: jacks", "output stream 0: channels 1-2, PCM S16", "44100", "48000", "errors 0")
    check("sound-beep", log, "sound: played 440 Hz for 100 ms", "sound: freq 20-20000 Hz", "usage: sound beep")
    if have9p:
        check("sound-wav", log, "sound: played 22050 frames (44100 Hz, 2 ch)", "sound: played 8820 frames (22050 Hz, 1 ch)", "sound: played 14400 frames (48000 Hz, 2 ch)",
              "sound: not a 16-bit PCM WAV file", "sound: cannot read file", "sound: unsupported or invalid rate/channels/length")
    time.sleep(1)
    try:
        w = wave.open(out)
        n = w.getnframes(); rate = w.getframerate(); ch = w.getnchannels()
        samples = struct.unpack("<%dh" % (n * ch), w.readframes(n))
        expect = (0.5 + 0.4 + 0.3 if have9p else 0) + 0.1
        ok("sound: host captured %.2f s of audio (expected %.2f)" % (n / rate, expect), abs(n / rate - expect) < 0.15, "%d frames @%d" % (n, rate))
        ok("sound: captured audio is not silence", max(abs(s) for s in samples) > 8000)
    except Exception as e:
        ok("sound: captured wav readable", False, str(e))


def session_9p():
    print("== 9p: virtio-9p (9P2000.L) shares mounted into the VFS")
    if not have_device("virtio-9p-device"): skip("9p", "QEMU lacks virtio-9p-device (build QEMU with virtfs, needs libattr)"); return
    share = os.path.join(TMP, "p9share")
    shutil.rmtree(share, ignore_errors=True)
    os.makedirs(os.path.join(share, "subdir")); os.makedirs(os.path.join(share, "tree", "a", "b"))
    open(os.path.join(share, "hello.txt"), "w").write("hello from host\n")
    open(os.path.join(share, "subdir", "deep.txt"), "w").write("deep-content\n")
    open(os.path.join(share, "gone.txt"), "w").write("delete me\n")
    open(os.path.join(share, "tree", "a", "b", "leaf.txt"), "w").write("leaf\n")
    os.symlink("hello.txt", os.path.join(share, "link.txt"))
    big = bytes((i * 31 + (i >> 8) * 7) & 255 for i in range(1200 * 1024))
    open(os.path.join(share, "big.bin"), "wb").write(big)
    open(os.path.join(share, "ünï.txt"), "w").write("utf8 name\n")

    def host_edit():
        open(os.path.join(share, "late.txt"), "w").write("late-from-host\n")
        open(os.path.join(share, "hello.txt"), "w").write("hello from host, edited!!\n")
        os.unlink(os.path.join(share, "gone.txt"))
    extra = ["-fsdev", "local,id=fs0,path=%s,security_model=none" % share, "-device", "virtio-9p-device,fsdev=fs0,mount_tag=share0"]
    cmds = C("9p", "su", "123", "mkdir -p /mnt/p9", "mount9p share0 /mnt/p9", "mount", "ls /mnt/p9", "cat /mnt/p9/hello.txt", "cat /mnt/p9/subdir/deep.txt",
             "readlink /mnt/p9/link.txt", "cat /mnt/p9/link.txt",
             "echo guest-data > /mnt/p9/new.txt", "cat /mnt/p9/new.txt", "mkdir /mnt/p9/d2", "echo x > /mnt/p9/d2/f.txt", "chmod 600 /mnt/p9/d2/f.txt",
             "mv /mnt/p9/new.txt /mnt/p9/d2/renamed.txt", "ln -s hello.txt /mnt/p9/sl", "cat /mnt/p9/sl", "ls /mnt/p9/d2",
             "cp /mnt/p9/big.bin /tmp/big.copy", "cp /tmp/big.copy /mnt/p9/big2.bin", "ls -l /mnt/p9/big2.bin",
             "rm -r /mnt/p9/tree", "ls /mnt/p9", "echo more >> /mnt/p9/d2/renamed.txt", "cat /mnt/p9/d2/renamed.txt",
             host_edit, "sleep 2", "cat /mnt/p9/late.txt", "cat /mnt/p9/hello.txt", "cat /mnt/p9/gone.txt", "df",
             "sync", "umount /mnt/p9", "ls /mnt/p9", "mount9p -r share0 /mnt/p9", "cat /mnt/p9/hello.txt", "echo z > /mnt/p9/ro.txt", "rm /mnt/p9/hello.txt", "umount /mnt/p9",
             "mount9p nosuchtag /mnt/p9", "mount9p share0 /nonexistent", "mount")
    log = run([(c, None) if not isinstance(c, tuple) else c for c in cmds], extra, wait=2)
    check("9p-mount", log, "virtio-9p shares", "share0", "share0 (9p) mounted on /mnt/p9", "hello.txt", "subdir", "ünï.txt")
    check("9p-read", log, "hello from host", "deep-content", "[LINK] link.txt -> hello.txt", "\nhello.txt\r")
    check("9p-write", log, "guest-data", "renamed.txt", "f.txt", "guest-data\r\nmore", "more")
    check("9p-hostchange", log, "late-from-host", "hello from host, edited!!", "cat: file not found")
    check("9p-umount", log, "unmounted")
    check("9p-ro", log, "share0 (9p) mounted on /mnt/p9 (ro)", "redirect: write failed", "rm: remove failed")
    check("9p-errors", log, "no virtio-9p share with tag 'nosuchtag'", "mount point not found")
    # what the guest left on the host
    ok("9p host: new file moved into d2", open(os.path.join(share, "d2", "renamed.txt")).read() == "guest-data\nmore\n")
    ok("9p host: chmod 600 reached the server", (os.stat(os.path.join(share, "d2", "f.txt")).st_mode & 0o777) == 0o600)
    ok("9p host: symlink created", os.path.islink(os.path.join(share, "sl")) and os.readlink(os.path.join(share, "sl")) == "hello.txt")
    ok("9p host: 1.2 MiB file written back byte-exact", os.path.exists(os.path.join(share, "big2.bin")) and open(os.path.join(share, "big2.bin"), "rb").read() == big)
    ok("9p host: rm -r removed the tree", not os.path.exists(os.path.join(share, "tree")))
    ok("9p host: ro mount changed nothing", not os.path.exists(os.path.join(share, "ro.txt")) and os.path.exists(os.path.join(share, "hello.txt")))


SESSIONS = {"usb": session_usb, "gpu": session_gpu, "sound": session_sound, "9p": session_9p}
if not os.path.exists(KERNEL):
    print("kernel not built (run linux/build.sh)"); sys.exit(2)
for name, fn in SESSIONS.items():
    if ONLY and name not in ONLY: continue
    fn()
shutil.rmtree(TMP, ignore_errors=True)
print("devices: %d checks, %d failed" % (total, fails))
sys.exit(1 if fails else 0)
