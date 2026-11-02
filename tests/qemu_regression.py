#!/usr/bin/env python3
"""RioOS QEMU regression runner (needs qemu-system-aarch64; build first: linux/build.sh).

usage: [QEMU_BIOS=<pc-bios dir>] python3 tests/qemu_regression.py [path-to-qemu-system-aarch64]
Runs real boots on a throw-away copy of storage/disk.img and checks the console output.
Sessions: core/ELF/job-control/scripting/security/FS, storage backends (NVMe, AHCI, root-on-NVMe),
SMP (-smp 4), network (DHCP, TCP server, IPv6, ping) against tests/qemu_fake_peer.py, shell extras, USB xHCI,
network extras + HTTPS against tests/qemu_fake_peer2.py.
"""
import os, pty, select, shutil, subprocess, sys, tempfile, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = sys.argv[1] if len(sys.argv) > 1 else shutil.which("qemu-system-aarch64") or "qemu-system-aarch64"
KERNEL = os.path.join(ROOT, "build", "kernel.elf")
TMP = tempfile.mkdtemp(prefix="rioos-reg-")
fails = 0
total = 0


def run(cmds, extra=(), disk=True, boot=8, wait=2, env=None, base_disk=None, monitor=None):
    args = [QEMU] + (["-L", os.environ["QEMU_BIOS"]] if os.environ.get("QEMU_BIOS") else []) + ["-M", "virt,gic-version=2,virtualization=on", "-global", "virtio-mmio.force-legacy=false",
            "-cpu", "cortex-a72", "-m", "128M", "-nographic", "-monitor", ("unix:%s,server,nowait" % monitor) if monitor else "none", "-kernel", KERNEL, "-no-reboot"]
    if disk:
        args += ["-drive", "if=none,format=raw,file=%s,id=vd0" % (base_disk or os.path.join(TMP, "disk.img")),
                 "-device", "virtio-blk-device,drive=vd0"]
    args += list(extra)
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
    msock = None
    if monitor:
        import socket
        msock = socket.socket(socket.AF_UNIX); msock.connect(monitor); time.sleep(0.5); msock.recv(4096)
    for c, w in cmds:
        if c.startswith("@"):                      # QEMU monitor command (e.g. "@sendkey a")
            for part in c[1:].split(";"):
                msock.send((part.strip() + "\n").encode()); time.sleep(0.25)
            log += "\n>>> " + c + "\n" + rd(w or wait)
            continue
        os.write(fd, (c + "\r").encode())
        log += "\n>>> " + c + "\n" + rd(w or wait)
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    return log


def check(name, log, *needles):
    global fails, total
    for n in needles:
        total += 1
        if n in log:
            print("  PASS  %s: %s" % (name, n))
        else:
            fails += 1
            print("  FAIL  %s: missing %r" % (name, n))


def C(*a):
    return [(c, None) for c in a]


shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
print("== session 1: core, ELF, jobs, scripting, security, FS")
log = run(C("ktest", "dltest", "id", "echo hello > /home/user/a.txt", "ln -s a.txt /home/user/l1", "cat /home/user/l1",
            "readlink /home/user/l1", "ls /home/user", "N=0", "while [ $N -lt 3 ]; do let N=N+1; done", "echo n=$N",
            "for i in 1 2; do echo it$i; done", "hello | upper", "spin 1 &", "jobs", "wait",
            "cat nofile || echo fb-ok", "frobnicate", "echo st=$?", "seq 5 | sort -r | head -n 2", "ls /bin | grep elf | wc -l", "echo a:b:c | cut -d : -f 2", "sleep 1", "which hello", "su", "123", "useradd bob", "pw", "pw", "users", "cat /etc/passwd", "mount -o remount,ro", "touch /x",
            "mount -o remount,rw", "jtest /jt.txt", "mount"), wait=3)
check("core", log, "ktest: 28/28 passed", "dltest ALL PASS", "uid=1000(user)")
check("fs", log, "[LINK] l1 -> a.txt", "hello", "a.txt", "remounted read-only", "remounted read-write", "journal        : on")
check("script", log, "n=3", "it1", "it2")
check("shell", log, "fb-ok", "st=127", "/bin/hello.elf", "\n9\r")
check("security", log, "Switched to root", "bob  uid=1001", "$rio$3000$", "User created")
print("== session 1b: reboot recovers the journal")
log = run(C("cat /jt.txt", "mount"))
check("recovery", log, "journal replayed", "recovered after replay")
print("== session 2: SMP")
log = run(C("smp", "smp test"), extra=["-smp", "4"], wait=3)
check("smp", log, "Online CPUs       : 4", "smp test: PASS")
print("== session 3: NVMe + AHCI extra disks")
for n in ("nv.img", "sd.img"):
    with open(os.path.join(TMP, n), "wb") as f: f.truncate(8 << 20)
log = run(C("lspci", "nvme selftest", "sata selftest"), extra=[
    "-drive", "file=%s,if=none,id=nv,format=raw" % os.path.join(TMP, "nv.img"), "-device", "nvme,drive=nv,serial=rio",
    "-device", "ich9-ahci,id=ahci", "-drive", "file=%s,if=none,id=sd,format=raw" % os.path.join(TMP, "sd.img"),
    "-device", "ide-hd,drive=sd,bus=ahci.0"], wait=3)
check("pci", log, "NVM controller (NVMe)", "SATA controller (AHCI)", "nvme selftest: PASS", "sata selftest: PASS")
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "root_nv.img"))
log = run(C("mount", "echo nvmeok > /home/user/nv.txt", "cat /home/user/nv.txt"), disk=False, extra=[
    "-drive", "file=%s,if=none,id=nv,format=raw" % os.path.join(TMP, "root_nv.img"), "-device", "nvme,drive=nv,serial=rio"], wait=3)
check("root-on-nvme", log, "NVMe (ready)", "nvmeok")
print("== session 4: network (DHCP, TCP server, IPv6) vs fake peer")
peer_log = os.path.join(TMP, "peer.log")
peer = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "qemu_fake_peer.py"), "75"], env=dict(os.environ, PEER_LOG=peer_log))
time.sleep(1)
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
log = run(C("dhcp", "echo hello > /home/user/a.txt", "httpd 8080 1", "ping6 fe80::ff:fe00:1", "ping 10.0.2.2"), wait=8, extra=[
    "-netdev", "dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=7001,remote.type=inet,remote.host=127.0.0.1,remote.port=7002",
    "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:56"])
peer.wait()
plog = open(peer_log).read()
check("net", log, "dhcp: bound", "10.0.2.77", "httpd: served /home/user/a.txt", "Reply from fe80::ff:fe00:1", "Reply from 10.0.2.2")
check("net-peer", plog, "HTTP/1.0 200 OK", "v6: echo reply from kernel", "v6: NA from kernel")
print("== session 5: shell extras (redirects, wildcards, $(), functions, /tmp, free)")
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
log = run(C("echo hi > /home/user/r1.txt", "echo more >> /home/user/r1.txt", "cat /home/user/r1.txt", "cat /nope 2> /home/user/e.txt", "cat /home/user/e.txt",
            "echo q > /home/user/r2.txt", "cat /home/user/r*.txt", "echo $(echo nest) x", "echo `seq 3`", "greet() { echo hello $1; }", "greet bob",
            "add() { let r=$1+$2; echo sum=$r; return 3; }", "add 2 5", "echo rc=$?", "echo t > /tmp/t.txt", "ls /tmp", "ls /home/user/r?.txt", "free",
            "for i in $(seq 100); do mkdir /home/user/n$i; done", "ls /home/user/n100 && echo many-ok"), wait=3)
check("shell2", log, "hi\r\nmore", "cat: file not found", "==> /home/user/r1.txt <==", "nest x", "1 2 3", "hello bob", "sum=7", "rc=3", "[FILE] t.txt", "many-ok", "Heap   :")
log = run(C("ls /tmp", "ls /home/user/r1.txt"))
check("tmp", log, "VFS: /tmp cleared (volatile).")
print("== session 6: USB xHCI keyboard + mouse")
log = run(C("usb", "usb selftest", "@sendkey u;sendkey s;sendkey b;sendkey spc;sendkey s;sendkey e;sendkey l;sendkey f;sendkey t;sendkey e;sendkey s;sendkey t;sendkey ret",
            "@mouse_move 10 5;mouse_button 1", "usb"), extra=["-device", "qemu-xhci,id=x", "-device", "usb-kbd,bus=x.0", "-device", "usb-mouse,bus=x.0"],
          wait=3, monitor=os.path.join(TMP, "mon.sock"))
check("usb", log, "xHCI ready, ports 8, keyboards 1, mice 1", "HID keyboard", "HID mouse", "usb selftest: PASS", "key events: 13", "mouse: x=10 y=5 buttons=1")
print("== session 7: network extras (nc, wget, telnet, DHCP renew, HTTPS/TLS 1.3) vs fake peer 2")
import signal
cert, key = os.path.join(TMP, "c.pem"), os.path.join(TMP, "k.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=rio.test"], stderr=subprocess.DEVNULL)
docroot = os.path.join(TMP, "www"); os.makedirs(docroot)
open(os.path.join(docroot, "hello.txt"), "w").write("hello from host web\n")
tls_srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "tls_test_server.py"), cert, key, docroot])
peer_log2 = os.path.join(TMP, "peer2.log")
peer2 = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "qemu_fake_peer2.py"), "70"], env=dict(os.environ, PEER_LOG=peer_log2))
time.sleep(1.5)
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
log = run(C("dhcp status", "dhcp renew", "wget 10.0.2.2:8081 /x.txt", "cat x.txt", "nc 10.0.2.2 2323 hi there", "nc -z 10.0.2.2 2323", "nc -z 10.0.2.2 9999",
            "httpsget 10.0.2.2:8443 /hello.txt", "wget https://10.0.2.2:8443/hello.txt h2.txt", "cat h2.txt"), wait=7, extra=[
    "-netdev", "dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=7001,remote.type=inet,remote.host=127.0.0.1,remote.port=7002",
    "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:56"])
tls_srv.kill(); peer2.kill(); peer2.wait(); tls_srv.wait()
check("net2", log, "DHCP: lease acquired", "dhcp: bound, server 10.0.2.2, lease 3600 s", "dhcp: lease renewed", "saved x.txt (13 bytes)", "wget-body-ok",
      "BANNER ready", "ECHO:hi there", "10.0.2.2 2323 open", "10.0.2.2 2323 closed" if False else "10.0.2.2 9999 closed", "--- body (20 bytes", "saved h2.txt (20 bytes)", "hello from host web")
shutil.rmtree(TMP, ignore_errors=True)
print("\n%d checks, %d failed" % (total, fails))
sys.exit(1 if fails else 0)
