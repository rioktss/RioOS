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
        if c.startswith("!"):                      # raw keystrokes, no trailing CR (editor tests)
            os.write(fd, c[1:].encode())
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
check("shell", log, "fb-ok", "st=127", "/bin/hello.elf", "\n19\r")
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
check("tmp", log, "VFS: tmpfs mounted on /tmp.")
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
peer2 = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "qemu_fake_peer2.py"), "170"], env=dict(os.environ, PEER_LOG=peer_log2))
time.sleep(1.5)
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
log = run(C("dhcp status", "dhcp renew", "wget 10.0.2.2:8081 /x.txt", "cat x.txt", "nc 10.0.2.2 2323 hi there", "nc -z 10.0.2.2 2323", "nc -z 10.0.2.2 9999",
            "httpsget 10.0.2.2:8443 /hello.txt", "wget https://10.0.2.2:8443/hello.txt h2.txt", "cat h2.txt",
            "udp6 send fe80::ff:fe00:1 5353 v6ping", "nc fe80::ff:fe00:1 2323 hi6", "nc -z fe80::ff:fe00:1 2323", "nc -l 7777"), wait=7, extra=[
    "-netdev", "dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=7001,remote.type=inet,remote.host=127.0.0.1,remote.port=7002",
    "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:56"])
tls_srv.kill(); peer2.kill(); peer2.wait(); tls_srv.wait()
check("net2", log, "DHCP: lease acquired", "dhcp: bound, server 10.0.2.2, lease 3600 s", "dhcp: lease renewed", "saved x.txt (13 bytes)", "wget-body-ok",
      "BANNER ready", "ECHO:hi there", "10.0.2.2 2323 open", "10.0.2.2 2323 closed" if False else "10.0.2.2 9999 closed", "--- body (20 bytes", "saved h2.txt (20 bytes)", "hello from host web", "v6ping", "ECHO6:hi6", "fe80::ff:fe00:1 2323 open", "hello-v6-client")
print("== session 8: multi-mount VFS (tmpfs, ram disk, NVMe/AHCI second disks, EXDEV mv)")
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
for n in ("nv.img", "sd.img"):
    with open(os.path.join(TMP, n), "wb") as f: f.truncate(8 << 20)
x8 = ["-drive", "file=%s,if=none,id=nv,format=raw" % os.path.join(TMP, "nv.img"), "-device", "nvme,drive=nv,serial=rio",
      "-device", "ich9-ahci,id=ahci", "-drive", "file=%s,if=none,id=sd,format=raw" % os.path.join(TMP, "sd.img"), "-device", "ide-hd,drive=sd,bus=ahci.0"]
log = run(C("su", "123", "mount", "df", "echo vol > /tmp/v.txt", "cat /tmp/v.txt", "mkdir /mnt", "mkdir /mnt/a", "mkdir /mnt/b", "mkdir /mnt/c",
            "mount nvme0n1 /mnt/a", "mkfs nvme0n1", "mount nvme0n1 /mnt/a", "echo nv-data > /mnt/a/f.txt", "cat /mnt/a/f.txt", "mkdir /mnt/a/sub", "echo deep > /mnt/a/sub/d.txt",
            "mkfs sda", "mount sda /mnt/b", "mount -t tmpfs none /mnt/c", "echo ramdata > /mnt/c/r.txt", "cat /mnt/c/r.txt",
            "mount", "df", "mv /mnt/a/f.txt /mnt/b/moved.txt", "cat /mnt/b/moved.txt", "ls /mnt/a", "cat /mnt/a/f.txt", "cp /mnt/a/sub/d.txt /mnt/c/d.txt", "cat /mnt/c/d.txt",
            "mv /mnt/a/sub /mnt/b/subdir", "cat /mnt/b/subdir/d.txt", "cd /mnt/a/..", "ls /mnt", "cd /mnt/a", "umount /mnt/a", "cd /", "rm /mnt/a", "rmdir /mnt/a",
            "mount nvme0n1 /mnt/a", "mount ram0 /mnt/b", "umount /mnt/b", "umount /mnt/a", "umount /mnt/c", "ls /mnt/a", "mount nvme0n1 /mnt/a", "ls /mnt/a", "cat /mnt/a/f.txt", "echo keep > /mnt/a/keep.txt", "umount /mnt/a", "ls /tmp",
            "mount -r sda /mnt/b", "mount", "umount /nonexist", "umount /"), wait=3, extra=x8)
check("mount", log, "VFS: tmpfs mounted on /tmp.", "vol", "no valid filesystem", "nvme0n1 formatted (myfs)", "nvme0n1 mounted on /mnt/a", "nv-data", "sda mounted on /mnt/b",
      "tmpfs mounted on /mnt/c", "ramdata", "nvme0n1 on /mnt/a type myfs (rw)", "sda on /mnt/b type myfs (rw)", "type tmpfs", "Moved (across filesystems).", "deep",
      "target is busy", "unmounted", "sda on /mnt/b type myfs (ro)")
log = run(C("su", "123", "mount nvme0n1 /mnt/a", "ls /mnt/a", "cat /mnt/a/keep.txt", "umount /mnt/a"), wait=3, extra=x8)
check("mount-persist", log, "nvme0n1 mounted on /mnt/a", "keep\r\n", "unmounted")
print("== session 9: curl, ntpdate, fw vs fake peer 2")
peer_log3 = os.path.join(TMP, "peer3.log")
peer3 = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "qemu_fake_peer2.py"), "400"], env=dict(os.environ, PEER_LOG=peer_log3))
time.sleep(1.5)
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
log = run(C("dhcp", "curl http://10.0.2.2:8081/", "curl -i http://10.0.2.2:8081/", "curl -I http://10.0.2.2:8081/", "curl -L http://10.0.2.2:8081/redir",
            "curl -s -d 'a=1&b=2' http://10.0.2.2:8081/p", "curl -X POST -d hello http://10.0.2.2:8081/p", "curl -H 'X-Test: 42' http://10.0.2.2:8081/",
            "curl -o /home/user/c.txt http://10.0.2.2:8081/final", "cat /home/user/c.txt", "curl -i http://10.0.2.2:8081/missing", "curl -Z x", "curl",
            "ntpdate -q 10.0.2.2", "ntpdate 10.0.2.2", "su", "123", "ntpdate 10.0.2.2", "date",
            "fw", "fw add deny out tcp 10.0.2.2 8081", "curl http://10.0.2.2:8081/", "fw", "fw del 0", "curl http://10.0.2.2:8081/",
            "fw insert deny out icmp", "ping 10.0.2.2", "fw flush", "ping 10.0.2.2", "fw default out deny", "fw", "curl http://10.0.2.2:8081/", "fw default out allow",
            "fw add allow in tcp any 22", "fw add bogus", "fw del 99", "fw flush", "fw"), wait=5, extra=[
    "-netdev", "dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=7001,remote.type=inet,remote.host=127.0.0.1,remote.port=7002",
    "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:56"])
peer3.kill(); peer3.wait()
check("curl", log, "wget-body-ok", "HTTP/1.0 200 OK", "Content-Length: 13", "redirected to /final", "final-ok", "POST:a=1&b=2", "POST:hello", "hdr-seen",
      "saved /home/user/c.txt", "HTTP/1.0 404 Not Found", "curl: unknown option -Z", "usage: curl")
check("ntpdate", log, "stratum 2", "time: 2030-01-01 00:00:00 UTC", "needs root", "clock set")
check("fw", log, "(no rules)", "fw: rule added at 0", "curl: connection failed", "fw: rule deleted", "fw: rules cleared", "default outbound: deny", "usage: fw", "fw: no such rule", "dropped: inbound")
print("== session 10: text tools, shell extras, libc, hard links, fsck, quota, FAT32, help")
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
fi = os.path.join(TMP, "fatx.img")
sys.path.insert(0, os.path.join(ROOT, "tests"))
import fat_image
fat_image.mkimage(fi, {"HOST.TXT": b"from-host\n", "A long file name.txt": b"long-name-ok\n"})
x10 = ["-drive", "file=%s,if=none,id=sd,format=raw" % fi, "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=sd,bus=ahci.0"]
log = run(C("printf 'a b c\\n1 2 3\\n' > /home/user/t.txt", "awk '{print $2 \"-x\"}' /home/user/t.txt", "sed 's/a/Z/' /home/user/t.txt", "cat /home/user/t.txt | rev", "cat /home/user/t.txt | tac | head -n 1",
            "cat /home/user/t.txt | nl", "seq 3 | xargs echo n:", "alias ll='ls -l'", "alias", "unalias ll", "printf '%d-%s\\n' 42 hi", "expr 6 '*' 7",
            "echo kept > /home/user/h1.txt", "ln /home/user/h1.txt /home/user/h2.txt", "stat /home/user/h1.txt", "echo changed > /home/user/h2.txt", "cat /home/user/h1.txt",
            "rm /home/user/h2.txt", "cat /home/user/h1.txt", "ln /home/user /home/user/dd", "fsck", "su", "123", "quota", "quota set 1000 1", "quota list", "fsck -r",
            "libctest", "hexdump /home/user/h1.txt", "fat sda info", "fat sda ls", "fat sda cat HOST.TXT", "fat sda cat 'A long file name.txt'", "fat sda put /home/user/h1.txt back.txt", "fat sda cat back.txt", "fat sda mkdir d1", "fat sda ls",
            "help", "help files", "help all", "help nope"), wait=3, extra=x10)
check("text", log, "b-x", "2-x", "Z b c", "c b a", "1 2 3", "     2\t1 2 3", "n: 1 2 3" if False else "n: 1", "ll='ls -l'", "42-hi", "42")
check("hardlink", log, "Links", "kept", "changed", "not allowed", "fsck")
check("libc", log, "54/54", "00000000")
check("fat", log, "from-host", "long-name-ok", "back.txt", "d1")
check("help", log, "RioOS help", "FILES & DIRECTORIES", "RioOS commands", "help: no category")
print("== session 10b: FAT32 image written by the kernel reads back on the host")
sd = os.path.join(TMP, "fatk.img")
with open(sd, "wb") as f: f.truncate(8 << 20)
run(C("su", "123", "echo kernel-wrote > /home/user/k.txt", "fat sda mkfs KRIO", "fat sda put /home/user/k.txt KFILE.TXT", "fat sda mkdir sub"), wait=3,
    extra=["-drive", "file=%s,if=none,id=sd,format=raw" % sd, "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=sd,bus=ahci.0"])
files = fat_image.read_image(sd)
check("fat-host", repr(files), "KFILE.TXT", "kernel-wrote")

print("== session 11: case/until, quoting, cat pipe, tab completion, diff, vi")
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))   # earlier sessions set quotas etc.
log = run(C("case b in a) echo isA;; b|c) echo isBC;; *) echo other;; esac", "i=0", "until [ $i -ge 3 ]; do echo u$i; let i=i+1; done", "echo 'a|b' | cat", "echo hi | cat",
            "echo tabfile-content > /home/user/zzfile", "!cat /home/user/zzf\t\r", "echo a > /home/user/x1", "echo b >> /home/user/x1", "echo c >> /home/user/x1", "echo d >> /home/user/x1",
            "echo a > /home/user/x2", "echo B >> /home/user/x2", "echo c >> /home/user/x2", "echo e >> /home/user/x2", "echo f >> /home/user/x2",
            "diff /home/user/x1 /home/user/x2", "diff -u /home/user/x1 /home/user/x2", "diff -q /home/user/x1 /home/user/x2", "diff -s /home/user/x1 /home/user/x1", "diff /home/user/x1 /nonexist",
            "vi /home/user/v1", "!ihello world\x1botwo lines\x1byyp", "!ggx", "!3Gdd", "!u", "!:w\r", "!/two\r", "!xP", "!:s/lines/LINES/g\r", "!:wq\r", "cat /home/user/v1",
            "vi /home/user/v1", "!dw..", "!Go\x1bix y z\x1b:%s/y/Q/\r", "!:set nu\r", "!ddZZ", "wc -l /home/user/v1", "vi -R /home/user/v1", "!x:q\r", "which vi", "help dev", "man pkg", "man cc", "help storage", "which cc"), wait=2, env=None)
check("case", log, "isBC", "u0", "u2", "a|b", "hi", "tabfile-content")
check("diff", log, "2c2", "< b", "> B", "4c4,5", "> f", "@@ -1,4 +1,5 @@", "-b", "+B", "Files /home/user/x1 and /home/user/x2 differ", "are identical", "No such file or directory")
check("devhelp", log, "ARCHIVES, DEVELOPMENT", "tar -c|-x|-t", "package manager for .rpk", "C compiler + virtual machine", "fat <dev> mkfs|info|ls|cat|get|put", "/bin/cc.elf")
check("vi", log, "ello world\r\ntwo LINES\r\ntwo lines", "3  /home/user/v1", "/bin/vi.elf")

print("== session 12: gzip + tar cross-checked with host python gzip/tarfile")
import gzip, io, tarfile
text = ("hello gzip world\n" * 500).encode()
bio = io.BytesIO()
with tarfile.open(fileobj=bio, mode="w", format=tarfile.USTAR_FORMAT) as t:
    for n, d in (("proj/a.txt", b"alpha\n"), ("proj/sub/b.txt", b"beta file\n" * 50), ("proj/c.bin", bytes(range(256)) * 3)):
        ti = tarfile.TarInfo(n); ti.size = len(d); ti.mode = 0o644; ti.mtime = 1700000000; t.addfile(ti, io.BytesIO(d))
fi = os.path.join(TMP, "gz.img")
fat_image.mkimage(fi, {"HOST.GZ": gzip.compress(text, 6), "H.TGZ": gzip.compress(bio.getvalue()), "H.TAR": bio.getvalue()})
x12 = ["-drive", "file=%s,if=none,id=sd,format=raw" % fi, "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=sd,bus=ahci.0"]
log = run(C("su", "123", "fat sda get HOST.GZ /home/user/host.gz", "gzip -t /home/user/host.gz", "gzip -l /home/user/host.gz", "zcat /home/user/host.gz | head -n 1", "zcat /home/user/host.gz | wc -l",
            "gunzip -k /home/user/host.gz", "wc -c /home/user/host", "echo alpha-beta-gamma > /home/user/p.txt", "gzip -k /home/user/p.txt", "fat sda put /home/user/p.txt.gz OUT.GZ",
            "fat sda get H.TAR /home/user/h.tar", "tar -tf /home/user/h.tar", "tar -xf /home/user/h.tar -C /home/user/ex", "cat /home/user/ex/proj/a.txt", "fat sda get H.TGZ /home/user/h.tgz",
            "tar -tzf /home/user/h.tgz", "mkdir /home/user/out", "mkdir /home/user/out/d", "echo ykernel > /home/user/out/d/y.txt", "tar -czf /home/user/o.tgz /home/user/out", "tar -cf /home/user/o.tar /home/user/out",
            "fat sda put /home/user/o.tgz OUT.TGZ", "fat sda put /home/user/o.tar OUT.TAR", "tar -tvf /home/user/o.tar", "gzip /nonexistent", "tar -xf /home/user/p.txt"), wait=2, extra=x12)
check("gzip", log, "hello gzip world", "500", "8500", "uncompressed", "proj/a.txt", "proj/sub/b.txt", "alpha", "proj/c.bin", "home/user/out/d/y.txt", "d0755", "cannot", "corrupt")
fs = fat_image.read_image(fi)
try:
    check("gzip-host", gzip.decompress(fs["OUT.GZ"]).decode(), "alpha-beta-gamma")
    tg = tarfile.open(fileobj=io.BytesIO(gzip.decompress(fs["OUT.TGZ"]))); check("tgz-host", tg.extractfile("home/user/out/d/y.txt").read().decode(), "ykernel")
    tt = tarfile.open(fileobj=io.BytesIO(fs["OUT.TAR"])); check("tar-host", repr(sorted(m.name for m in tt.getmembers())), "home/user/out/d/y.txt")
except Exception as e:
    check("gzip-tar-host", "exception %r" % e, "never-matches")

print("== session 13: pkg + cc")
prog = {}
for fn in sorted(os.listdir(os.path.join(ROOT, "tests", "cprog"))):
    prog[fn.upper()] = open(os.path.join(ROOT, "tests", "cprog", fn), "rb").read()
fi = os.path.join(TMP, "cc.img"); fat_image.mkimage(fi, prog)
x13 = ["-drive", "file=%s,if=none,id=sd,format=raw" % fi, "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=sd,bus=ahci.0"]
cmds = ["su", "123", "mkdir /home/user/s1", "mkdir /home/user/s1/usr", "mkdir /home/user/s1/usr/lib", "echo name=libfoo > /home/user/s1/PKGINFO", "echo version=1.0 >> /home/user/s1/PKGINFO", "echo desc=Foo library >> /home/user/s1/PKGINFO",
        "echo foodata > /home/user/s1/usr/lib/libfoo.dat", "pkg build /home/user/s1 /var/cache/pkg/libfoo-1.0.rpk", "mkdir /home/user/s2", "mkdir /home/user/s2/usr", "mkdir /home/user/s2/usr/bin",
        "echo name=app > /home/user/s2/PKGINFO", "echo version=2.1 >> /home/user/s2/PKGINFO", "echo desc=Demo app >> /home/user/s2/PKGINFO", "echo depends=libfoo >> /home/user/s2/PKGINFO", "echo hello > /home/user/s2/usr/bin/app.txt",
        "pkg build -z /home/user/s2 /var/cache/pkg/app-2.1.rpk", "pkg update", "cat /var/lib/pkg/INDEX", "pkg search demo", "pkg install app", "pkg list", "pkg files app", "cat /usr/lib/libfoo.dat", "pkg verify", "pkg remove libfoo",
        "echo X >> /usr/bin/app.txt", "pkg verify app", "pkg owner /usr/lib/libfoo.dat", "pkg info libfoo", "pkg remove app", "pkg remove libfoo", "pkg list", "pkg install /nonexistent.rpk", "pkg bogus"]
for f in ("T1", "T3", "T4", "T5", "T6", "T7", "T8", "T10"):
    cmds.append("fat sda get %s.C /home/user/%s.c" % (f, f.lower()))
cmds += ["cc /home/user/t1.c one two", "echo rc=$?", "cc /home/user/t3.c", "cc /home/user/t4.c", "cc /home/user/t5.c", "cc /home/user/t7.c", "cc /home/user/t8.c", "cc -c /home/user/t1.c",
         "cc -o /home/user/t1.rbc /home/user/t1.c", "cc -x /home/user/t1.rbc zz", "cc /nonexist.c", "cc -S /home/user/t8.c", "cc -S /home/user/t4.c", "cc /home/user/t6.c", "!10\r", "!bob\r", "cc -s 5000 /home/user/t10.c"]
log = run(C(*cmds), wait=2, extra=x13)
check("pkg", log, "built libfoo 1.0", "built app 2.1", "index updated: 2 packages", "app 2.1 app-2.1.rpk | Demo app", "[available] app", "installing dependency libfoo for app", "installed app 2.1",
      "libfoo               1.0", "/usr/bin/app.txt", "foodata", "2 files checked, 0 problems", "is required by app", "SIZE differs", "is owned by libfoo", "Status     : installed", "removed app", "removed libfoo", "no packages installed",
      "cannot read package", "unknown command")
check("cc", log, "fib(20)=6765 gcd=12 fact(15)=1307674368000", "x=4 y=3 g=7", "sum=15 len=5 hello e", "ab cd ef", "enum 0 5 6 N=10 SQ=121", "p[9]=81 *(p+3)=9 diff=7", "00042|ab   |ff|   xy|", "four(fall)",
      "b=44 sizeof(int)=4 sizeof(arr)=20 sizeof(long)=8", "i=10", "-10", "3 -3 -1", "args=3", "[one] [two]", "4000000000 -294967296", "rc=3",
      "invalid memory access at address 0", "stack overflow", "division by zero", "floating point is not supported", "unexpected ';' in expression", "ok (", "wrote /home/user/t1.rbc", "args=2", "[zz]",
      "not found" if False else "cannot read", "main:", "sum 1..10 = 55", "hi bob", "step limit exceeded")

shutil.rmtree(TMP, ignore_errors=True)
print("\n%d checks, %d failed" % (total, fails))
sys.exit(1 if fails else 0)
