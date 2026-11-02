#!/usr/bin/env python3
"""RioOS QEMU regression runner (needs qemu-system-aarch64; build first: linux/build.sh).

usage: [QEMU_BIOS=<pc-bios dir>] python3 tests/qemu_regression.py [path-to-qemu-system-aarch64]
Runs real boots on a throw-away copy of storage/disk.img and checks the console output.
Sessions: core/ELF/job-control/scripting/security/FS, storage backends (NVMe, AHCI, root-on-NVMe),
SMP (-smp 4), network (DHCP, TCP server, IPv6, ping) against tests/qemu_fake_peer.py.
"""
import os, pty, select, shutil, subprocess, sys, tempfile, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = sys.argv[1] if len(sys.argv) > 1 else shutil.which("qemu-system-aarch64") or "qemu-system-aarch64"
KERNEL = os.path.join(ROOT, "build", "kernel.elf")
TMP = tempfile.mkdtemp(prefix="rioos-reg-")
fails = 0
total = 0


def run(cmds, extra=(), disk=True, boot=8, wait=2, env=None, base_disk=None):
    args = [QEMU] + (["-L", os.environ["QEMU_BIOS"]] if os.environ.get("QEMU_BIOS") else []) + ["-M", "virt,gic-version=2,virtualization=on", "-global", "virtio-mmio.force-legacy=false",
            "-cpu", "cortex-a72", "-m", "128M", "-nographic", "-monitor", "none", "-kernel", KERNEL, "-no-reboot"]
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
    for c, w in cmds:
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
            "su", "123", "useradd bob", "pw", "pw", "users", "cat /etc/passwd", "mount -o remount,ro", "touch /x",
            "mount -o remount,rw", "jtest /jt.txt", "mount"), wait=3)
check("core", log, "ktest: 28/28 passed", "dltest ALL PASS", "uid=1000(user)")
check("fs", log, "[LINK] l1 -> a.txt", "hello", "a.txt", "remounted read-only", "remounted read-write", "journal        : on")
check("script", log, "n=3", "it1", "it2")
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
shutil.rmtree(TMP, ignore_errors=True)
print("\n%d checks, %d failed" % (total, fails))
sys.exit(1 if fails else 0)
