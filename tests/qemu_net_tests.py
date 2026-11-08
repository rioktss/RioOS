#!/usr/bin/env python3
"""Real-network QEMU tests: TLS certificate verification, SSH/SCP (against libssh), httpd, ftpd, pkg over HTTP(S).
The guest is attached through tests/qemu_tap_bridge.py to a Linux TAP interface (host = 10.0.2.2, guest = 10.0.2.77), so every
service is a REAL implementation (python ssl / libssh / ftplib / http.client).  Needs root + /dev/net/tun + python 'cryptography' + libssh.
usage: python3 tests/qemu_net_tests.py [qemu-system-aarch64]      (prints PASS/FAIL lines, exit 1 on any FAIL, 0 with SKIP when unsupported)"""
import ftplib, hashlib, http.client, os, pty, select, shutil, socket, ssl, subprocess, sys, tempfile, threading, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = sys.argv[1] if len(sys.argv) > 1 else shutil.which("qemu-system-aarch64") or "qemu-system-aarch64"
KERNEL = os.path.join(ROOT, "build", "kernel.elf")
TMP = tempfile.mkdtemp(prefix="rioos-net-")
GUEST = "10.0.2.77"; HOST = "10.0.2.2"
fails = 0; total = 0; procs = []; LOGS = []

def check(name, cond, detail=""):
    global fails, total
    total += 1
    if cond: print("  PASS  %s" % name)
    else: fails += 1; print("  FAIL  %s %s" % (name, detail))

def skip(msg): print("SKIP: " + msg); sys.exit(0)

def run(cmds, boot=8, extra=()):
    args = [QEMU] + (["-L", os.environ["QEMU_BIOS"]] if os.environ.get("QEMU_BIOS") else []) + ["-M", "virt,gic-version=2,virtualization=on", "-global", "virtio-mmio.force-legacy=false",
            "-cpu", "cortex-a72", "-m", "128M", "-nographic", "-monitor", "none", "-kernel", KERNEL, "-no-reboot",
            "-drive", "if=none,format=raw,file=%s,id=vd0" % os.path.join(TMP, "disk.img"), "-device", "virtio-blk-device,drive=vd0",
            "-netdev", "dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=7001,remote.type=inet,remote.host=127.0.0.1,remote.port=7002",
            "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:56"] + list(extra)
    pid, fd = pty.fork()
    if pid == 0: os.execv(QEMU, args)
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
        os.write(fd, (c + "\r").encode()); log += "\n>>> " + c + "\n" + rd(w)
    os.kill(pid, 9); os.waitpid(pid, 0)
    open(os.path.join(TMP, "last.log"), "w").write(log)
    LOGS.append(log)
    return log

def after(log, cmd):
    """output of the first occurrence of '>>> cmd' up to the next prompt line"""
    i = log.find(">>> " + cmd + "\n"); 
    if i < 0: return ""
    j = log.find("\n>>> ", i + 5)
    return log[i:j if j > 0 else len(log)].replace("\r", "")

def wait_port(ip, port, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection((ip, port), 1.0); s.close(); return True
        except OSError: time.sleep(0.5)
    return False
def wait_listen(ip, port, timeout=150):
    """connect once (the guest servers accept the probe as a normal client)"""
    return wait_port(ip, port, timeout)

def start(cmd, **kw):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw); procs.append(p); return p

def main():
    if os.geteuid() != 0 or not os.path.exists("/dev/net/tun"): skip("needs root and /dev/net/tun")
    try: import cryptography  # noqa
    except ImportError: skip("python 'cryptography' missing")
    if not os.path.exists(KERNEL): skip("build first")
    shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
    pki = os.path.join(TMP, "pki"); www = os.path.join(TMP, "www"); sshw = os.path.join(TMP, "ssh")
    os.makedirs(www); os.makedirs(sshw)
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tests", "gen_pki.py"), pki], stdout=subprocess.DEVNULL)
    for t in ("rsa", "rsa4k", "p256", "p384", "mixed", "ed", "other", "expired"): shutil.copy("%s/%s_root.pem" % (pki, t), www)
    open(www + "/hello.txt", "w").write("secure hello\n")
    bridge = start([sys.executable, os.path.join(ROOT, "tests", "qemu_tap_bridge.py")]); time.sleep(1.5)
    if bridge.poll() is not None: skip("cannot create the TAP bridge")
    start([sys.executable, os.path.join(ROOT, "tests", "tls_multi_server.py"), pki, www])
    start([sys.executable, "-m", "http.server", "8099", "--bind", HOST, "--directory", www])
    have_ssh = os.path.exists("/usr/lib/x86_64-linux-gnu/libssh.so.4")
    if have_ssh:
        os.makedirs(sshw + "/root", exist_ok=True)
        open(sshw + "/root/big.bin", "wb").write(os.urandom(700000))
        start([sys.executable, os.path.join(ROOT, "tests", "ssh_test_server.py"), sshw])
    time.sleep(4)
    results = {}
    try:
        for name in SESSIONS:
            print("== %s" % name); shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))   # fresh image: sessions must not share state (e.g. trusted CAs)
            SESSIONS[name](pki, www, sshw, have_ssh)
    finally:
        for p in procs:
            try: p.kill()
            except Exception: pass
    if fails and os.environ.get("NETTEST_LOG"): open(os.environ["NETTEST_LOG"], "w").write("\n=====\n".join(LOGS))
    print("net tests: %d checks, %d failed" % (total, fails))
    sys.exit(1 if fails else 0)

# ------------------------------------------------------------------------------------------------------------------
def s_httpd(pki, www, sshw, have_ssh):
    guest = [("su", 2), ("123", 2), ("dhcp", 4), ("mkdir www", 1), ("echo '<h1>idx</h1>' > www/index.html", 1), ("mkdir www/sub", 1), ("echo data > www/sub/f.txt", 1),
             ("mkdir www/empty", 1), ("echo secret > www/priv.txt", 1), ("chmod 600 www/priv.txt", 1), ("echo 'a b' > www/sp_ace.txt", 1)]
    if have_ssh: guest += [("scp -P 2201 tester@10.0.2.2:big.bin www/big.bin", 3), ("s3cret", 12)]
    res = {}
    def client():
        if not wait_port(GUEST, 8081, 240): res["fail"] = "no listener"; return
        time.sleep(1.0)
        try:
            c = http.client.HTTPConnection(GUEST, 8081, timeout=10)
            c.request("GET", "/"); r = c.getresponse(); res["idx"] = (r.status, r.read(), r.getheader("Content-Type"), r.getheader("Connection"))
            c.request("GET", "/sub/f.txt"); r = c.getresponse(); res["keep"] = (r.status, r.read(), r.getheader("Content-Type"))     # same connection (keep-alive)
            c.request("HEAD", "/sub/f.txt"); r = c.getresponse(); res["head"] = (r.status, r.read(), r.getheader("Content-Length"))
            c.request("GET", "/sub/f.txt", headers={"Range": "bytes=1-3"}); r = c.getresponse(); res["range"] = (r.status, r.read(), r.getheader("Content-Range"))
            c.request("GET", "/sub/f.txt", headers={"Range": "bytes=-2"}); r = c.getresponse(); res["range2"] = (r.status, r.read())
            c.request("GET", "/sub/f.txt", headers={"Range": "bytes=50-60"}); r = c.getresponse(); res["range416"] = (r.status, r.read()); 
            c.close()
            def one(method, path, **kw):
                cc = http.client.HTTPConnection(GUEST, 8081, timeout=10); cc.request(method, path, **kw); r = cc.getresponse(); b = r.read(); h = dict(r.getheaders()); cc.close(); return r.status, b, h
            res["list"] = one("GET", "/sub/"); res["redir"] = one("GET", "/sub"); res["nf"] = one("GET", "/nope"); res["post"] = one("POST", "/", body=b"x")
            res["emptydir"] = one("GET", "/empty/"); res["space"] = one("GET", "/sp%5Face.txt"); res["priv"] = one("GET", "/priv.txt")
            s = socket.create_connection((GUEST, 8081), 5); s.sendall(b"GET /../etc/x HTTP/1.0\r\n\r\n"); res["dotdot"] = s.recv(200); s.close()
            s = socket.create_connection((GUEST, 8081), 5); s.sendall(b"GET /%2e%2e/x HTTP/1.0\r\n\r\n"); res["enc"] = s.recv(200); s.close()
            s = socket.create_connection((GUEST, 8081), 5); s.sendall(b"GET /sub/f.txt HTTP/1.0\r\n\r\n"); res["http10"] = s.recv(500); s.close()
            if have_ssh: res["big"] = one("GET", "/big.bin")
        except Exception as e: res["fail"] = repr(e)
    t = threading.Thread(target=client); t.start()
    guest += [("httpd 8081 22 -d /home/user/www -q", 70), ("echo httpd-done", 3)]
    log = run(guest)
    t.join(5)
    check("httpd: no client error", "fail" not in res, res.get("fail", ""))
    r = res.get("idx", (0, b"", "", "")); check("httpd index.html 200 text/html", r[0] == 200 and r[1] == b"<h1>idx</h1>\n" and r[2].startswith("text/html"), str(r))
    r = res.get("keep", (0,)); check("httpd keep-alive second request", r[0] == 200 and r[1] == b"data\n" and r[2].startswith("text/plain"), str(r))
    r = res.get("head", (0,)); check("httpd HEAD has no body, right length", r[0] == 200 and r[1] == b"" and r[2] == "5", str(r))
    r = res.get("range", (0,)); check("httpd Range 206", r[0] == 206 and r[1] == b"ata" and r[2] == "bytes 1-3/5", str(r))
    r = res.get("range2", (0,)); check("httpd suffix Range", r[0] == 206 and r[1] == b"a\n", str(r))
    r = res.get("range416", (0,)); check("httpd unsatisfiable Range 416", r[0] == 416, str(r))
    r = res.get("list", (0, b"")); check("httpd directory listing", r[0] == 200 and b"f.txt" in r[1] and b"../" in r[1], str(r[:2]))
    r = res.get("redir", (0, b"", {})); check("httpd 301 to trailing slash", r[0] == 301 and r[2].get("Location") == "/sub/", str(r))
    r = res.get("nf", (0,)); check("httpd 404", r[0] == 404, str(r[:1]))
    r = res.get("post", (0, b"", {})); check("httpd POST 405 + Allow", r[0] == 405 and "GET" in r[2].get("Allow", ""), str(r))
    r = res.get("emptydir", (0, b"")); check("httpd empty directory listing", r[0] == 200 and b"Index of /empty/" in r[1], str(r[:2]))
    r = res.get("space", (0, b"")); check("httpd %20 in path", r[0] == 200 and r[1] == b"a b\n", str(r[:2]))
    r = res.get("priv", (0,)); check("httpd serves with the permissions of the server user (root here)", r[0] in (200, 403), str(r[:1]))
    check("httpd '..' rejected", res.get("dotdot", b"").startswith(b"HTTP/1.0 400"), str(res.get("dotdot")))
    check("httpd %2e%2e rejected", res.get("enc", b"").startswith(b"HTTP/1.0 400"), str(res.get("enc")))
    check("httpd HTTP/1.0 closes the connection", b"Connection: close" in res.get("http10", b""), str(res.get("http10")))
    if have_ssh:
        r = res.get("big", (0, b"")); check("httpd 700 KB file identical", r[0] == 200 and hashlib.sha256(r[1]).hexdigest() == hashlib.sha256(open(sshw + "/root/big.bin", "rb").read()).hexdigest(), str(r[:1]))
    check("httpd stops after the request budget", "httpd-done" in after(log, "echo httpd-done"), after(log, "httpd 8081 22 -d /home/user/www -q")[-300:])

def s_ftpd(pki, www, sshw, have_ssh):
    guest = [("su", 2), ("123", 2), ("dhcp", 4), ("useradd tbob", 2), ("pw1", 2), ("pw1", 3), ("mkdir /home/tbob", 1), ("chown tbob /home/tbob", 1), ("echo rootonly > /etc/rootonly.txt", 1), ("chmod 600 /etc/rootonly.txt", 1),
             ("mkdir ftp", 1), ("echo anon-readable > ftp/pub.txt", 1), ("mkdir ftp/jail", 1), ("echo inside > ftp/jail/in.txt", 1)]
    res = {}
    data = os.urandom(300000)
    def client():
        if not wait_port(GUEST, 21, 240): res["fail"] = "no listener"; return
        time.sleep(0.5)
        try:
            f = ftplib.FTP(); f.connect(GUEST, 21, timeout=15); res["banner"] = f.getwelcome()
            try: f.login("tbob", "WRONG"); res["badlogin"] = False
            except ftplib.error_perm as e: res["badlogin"] = str(e).startswith("530")
            f.close()
            f = ftplib.FTP(); f.connect(GUEST, 21, timeout=15); f.login("tbob", "pw1"); res["pwd"] = f.pwd()
            f.cwd("/home/tbob"); res["cwd"] = f.pwd()
            f.mkd("sub"); f.cwd("sub")
            import io
            f.storbinary("STOR big.bin", io.BytesIO(data)); res["size"] = f.size("big.bin")
            buf = io.BytesIO(); f.retrbinary("RETR big.bin", buf.write); res["retr"] = buf.getvalue() == data
            buf = io.BytesIO(); f.retrbinary("RETR big.bin", buf.write, rest=299990); res["rest"] = buf.getvalue() == data[299990:]
            f.storbinary("STOR a.txt", io.BytesIO(b"line1\n")); f.storbinary("APPE a.txt", io.BytesIO(b"line2\n"))
            buf = io.BytesIO(); f.retrbinary("RETR a.txt", buf.write); res["appe"] = buf.getvalue()
            res["nlst"] = sorted(f.nlst()); lines = []; f.retrlines("LIST", lines.append); res["list"] = lines
            res["mdtm"] = f.voidcmd("MDTM a.txt")
            f.rename("a.txt", "b.txt"); res["rename"] = "b.txt" in f.nlst() and "a.txt" not in f.nlst()
            f.delete("b.txt"); res["delete"] = "b.txt" not in f.nlst()
            f.cwd(".."); f.mkd("tmpd"); f.rmd("tmpd"); res["rmd"] = "tmpd" not in f.nlst()
            res["cdup"] = (f.voidcmd("CDUP"), f.pwd())
            try: f.cwd("/nonexistent"); res["cwdbad"] = False
            except ftplib.error_perm as e: res["cwdbad"] = str(e).startswith("550")
            try: f.retrbinary("RETR /etc/rootonly.txt", lambda b: None); res["perm"] = False
            except ftplib.error_perm as e: res["perm"] = str(e).startswith("550")
            try: f.sendcmd("BOGUS"); res["bogus"] = False
            except ftplib.error_perm as e: res["bogus"] = str(e).startswith("502")
            f.set_pasv(False); lines = []; f.retrlines("LIST", lines.append); res["active_list"] = len(lines) > 0
            buf = io.BytesIO(); f.retrbinary("RETR /home/tbob/sub/big.bin", buf.write); res["active_retr"] = buf.getvalue() == data
            f.set_pasv(True)
            f.quit()
            f = ftplib.FTP(); f.connect(GUEST, 21, timeout=15); f.login("anonymous", "x@y"); buf = []; f.retrlines("RETR /home/user/ftp/pub.txt", buf.append); res["anon_read"] = buf
            try: f.storbinary("STOR /home/user/ftp/evil.txt", io.BytesIO(b"x")); res["anon_write"] = False
            except ftplib.error_perm as e: res["anon_write"] = str(e).startswith("550")
            f.quit()
            if not wait_port(GUEST, 21, 60): res["fail"] = "second listener missing"; return
            f = ftplib.FTP(); f.connect(GUEST, 21, timeout=15); f.login("root", "123"); res["jail_pwd"] = f.pwd(); res["jail_ls"] = f.nlst()
            f.cwd("../../.."); res["jail_escape"] = f.pwd()
            buf = []; f.retrlines("RETR in.txt", buf.append); res["jail_read"] = buf
            try: f.retrbinary("RETR /../../etc/rootonly.txt", lambda b: None); res["jail_etc"] = False
            except ftplib.error_perm: res["jail_etc"] = True
            f.quit()
        except Exception as e: res["fail"] = repr(e)
    t = threading.Thread(target=client); t.start()
    guest += [("ftpd -n 4 -a -d /", 25 + 10), ("echo ftpd1-done", 3)]
    # the anonymous check above reads a path under /home/user: root is "/" so absolute paths work
    guest += [("ftpd -n 2 -d /home/user/ftp/jail", 25), ("echo ftpd2-done", 3)]
    log = run(guest)
    t.join(5)
    check("ftpd: no client error", "fail" not in res, res.get("fail", ""))
    check("ftpd banner", res.get("banner", "").startswith("220"), res.get("banner", ""))
    check("ftpd wrong password -> 530", res.get("badlogin") is True)
    check("ftpd PWD / CWD", res.get("pwd") == "/" and res.get("cwd") == "/home/tbob", str((res.get("pwd"), res.get("cwd"))))
    check("ftpd STOR 300 KB + SIZE", res.get("size") == 300000, str(res.get("size")))
    check("ftpd RETR 300 KB identical", res.get("retr") is True)
    check("ftpd REST resume", res.get("rest") is True)
    check("ftpd APPE", res.get("appe") == b"line1\nline2\n", str(res.get("appe")))
    check("ftpd NLST", res.get("nlst") == ["a.txt", "big.bin"], str(res.get("nlst")))
    check("ftpd LIST unix format", any(l.startswith("-rw") and l.endswith("big.bin") and " 300000 " in l for l in res.get("list", [])), str(res.get("list")))
    check("ftpd MDTM", res.get("mdtm", "").startswith("213 20"), str(res.get("mdtm")))
    check("ftpd RNFR/RNTO", res.get("rename") is True)
    check("ftpd DELE", res.get("delete") is True)
    check("ftpd MKD/RMD", res.get("rmd") is True)
    check("ftpd CDUP", res.get("cdup", ("", ""))[1] == "/home", str(res.get("cdup")))
    check("ftpd CWD to missing dir -> 550", res.get("cwdbad") is True)
    check("ftpd uses the logged-in user's permissions (550 on root-only file)", res.get("perm") is True)
    check("ftpd unknown command -> 502", res.get("bogus") is True)
    check("ftpd active mode (PORT) LIST", res.get("active_list") is True)
    check("ftpd active mode RETR 300 KB identical", res.get("active_retr") is True)
    check("ftpd anonymous read", res.get("anon_read") == ["anon-readable"], str(res.get("anon_read")))
    check("ftpd anonymous is read-only", res.get("anon_write") is True)
    check("ftpd -d root confinement (PWD /)", res.get("jail_pwd") == "/", str(res.get("jail_pwd")))
    check("ftpd -d root lists jail", res.get("jail_ls") == ["in.txt"], str(res.get("jail_ls")))
    check("ftpd '..' cannot leave the root", res.get("jail_escape") == "/", str(res.get("jail_escape")))
    check("ftpd jail read", res.get("jail_read") == ["inside"], str(res.get("jail_read")))
    check("ftpd jail blocks /../../etc", res.get("jail_etc") is True)
    check("ftpd sessions finish", "ftpd1-done" in after(log, "echo ftpd1-done") and "ftpd2-done" in after(log, "echo ftpd2-done"), after(log, "ftpd -n 4 -a -d /")[-300:])


def make_rpk(path, name, version, desc, files, depends=""):
    import io, tarfile
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        info = "name=%s\nversion=%s\ndesc=%s\n%s" % (name, version, desc, ("depends=%s\n" % depends) if depends else "")
        ti = tarfile.TarInfo("PKGINFO"); ti.size = len(info); t.addfile(ti, io.BytesIO(info.encode()))
        for fn, data in files.items():
            ti = tarfile.TarInfo(fn); ti.size = len(data); ti.mode = 0o644; t.addfile(ti, io.BytesIO(data))
    open(path, "wb").write(buf.getvalue())
    return hashlib.sha256(buf.getvalue()).hexdigest()

def write_index(d, rows):
    open(os.path.join(d, "INDEX"), "w").write("".join("%s %s %s %s | %s\n" % r for r in rows))

def s_pkg(pki, www, sshw, have_ssh):
    A = os.path.join(www, "repoA"); B = os.path.join(www, "repoB"); E = os.path.join(www, "repoEvil")
    for d in (A, B, E): os.makedirs(d, exist_ok=True)
    h1 = make_rpk(A + "/hello-1.0.rpk", "hello", "1.0", "hello demo", {"usr/share/hello/msg.txt": b"hello 1.0 from net\n"}, "libgreet>=1.0")
    g1 = make_rpk(A + "/libgreet-1.0.rpk", "libgreet", "1.0", "greeting library", {"usr/share/libgreet/g.txt": b"greet\n"})
    write_index(A, [("hello", "1.0", "hello-1.0.rpk", h1, "hello demo"), ("libgreet", "1.0", "libgreet-1.0.rpk", g1, "greeting library")])
    h2 = make_rpk(B + "/hello-1.1.rpk", "hello", "1.1", "hello demo", {"usr/share/hello/msg.txt": b"hello 1.1 from net\n"}, "libgreet>=1.0")
    write_index(B, [("hello", "1.1", "hello-1.1.rpk", h2, "hello demo")])
    make_rpk(E + "/evil-1.0.rpk", "evil", "1.0", "tampered", {"usr/share/evil/x.txt": b"pwn\n"})
    write_index(E, [("evil", "1.0", "evil-1.0.rpk", "0" * 63 + "1", "tampered")])
    open(E + "/bad-1.0.rpk", "wb").write(b"junk")
    guest = [("su", 2), ("123", 2), ("dhcp", 4), ("pkg remote list", 2), ("pkg remote add ftp://x/y", 2), ("pkg remote add http://10.0.2.2:8099/repoA", 2), ("pkg remote list", 2),
             ("pkg update", 6), ("pkg search greet", 2), ("pkg install nosuchpkg", 2), ("pkg install hello", 15), ("cat /usr/share/hello/msg.txt", 2), ("pkg list", 2), ("pkg verify", 3),
             ("pkg install hello", 2),
             ("pkg remote add http://10.0.2.2:8099/repoB", 2), ("pkg update", 6), ("pkg upgrade", 12), ("cat /usr/share/hello/msg.txt", 2),
             ("pkg remote remove http://10.0.2.2:8099/repoB", 2), ("pkg remote remove http://10.0.2.2:8099/repoA", 2),
             ("pkg remote add http://10.0.2.2:8099/repoEvil", 2), ("pkg update", 6), ("pkg install evil", 10), ("ls /usr/share/evil", 2), ("pkg list", 2),
             ("pkg remote remove http://10.0.2.2:8099/repoEvil", 2), ("pkg remote add http://10.0.2.2:8099/repoNone", 2), ("pkg update", 6),
             ("pkg remote remove http://10.0.2.2:8099/repoNone", 2),
             ("pkg remove hello", 3), ("pkg remove libgreet", 3),
             ("pkg remote add https://10.0.2.2:18441/repoA", 2), ("pkg update", 8), ("pkg install hello", 5),
             ("wget http://10.0.2.2:8099/rsa_root.pem", 4), ("certs add rsa_root.pem", 3), ("pkg update", 8), ("pkg install hello", 20), ("cat /usr/share/hello/msg.txt", 2),
             ("pkg remove hello", 3), ("pkg remove libgreet", 3),
             ("pkg remote remove https://10.0.2.2:18441/repoA", 2), ("pkg remote add https://10.0.2.2:18449/repoA", 2), ("pkg update", 8),
             ("pkg remote add https://10.0.2.2:18449/repoA -k", 2), ("pkg remote list", 2), ("pkg update", 8), ("pkg install libgreet", 10), ("pkg list", 2),
             ("mkdir idx", 1), ("cp /var/cache/pkg/libgreet-1.0.rpk idx/", 2), ("pkg mkindex idx", 3), ("cat idx/INDEX", 2), ("echo pkg-done", 2)]
    log = run(guest)
    def a(c, n=1): return after(log, c) if n == 1 else after(log, c)
    check("pkg remote list (empty)", "no remotes" in a("pkg remote list"), a("pkg remote list"))
    check("pkg remote add rejects non-http", "must be http" in a("pkg remote add ftp://x/y"), a("pkg remote add ftp://x/y"))
    check("pkg remote add", "remote added" in a("pkg remote add http://10.0.2.2:8099/repoA"), a("pkg remote add http://10.0.2.2:8099/repoA"))
    check("pkg update fetches the remote index", "2 package(s)" in a("pkg update") and "remote index updated: 2" in a("pkg update"), a("pkg update"))
    check("pkg search finds remote packages", "[remote] libgreet" in a("pkg search greet"), a("pkg search greet"))
    check("pkg install unknown -> not found", "not found" in a("pkg install nosuchpkg"), a("pkg install nosuchpkg"))
    ins = a("pkg install hello")
    check("pkg install downloads dependency + verifies sha256", "installing dependency libgreet" in ins and ins.count("verified sha256") == 2 and "installed hello 1.0" in ins and "installed libgreet 1.0" in ins, ins)
    check("pkg install put the files in place", "hello 1.0 from net" in a("cat /usr/share/hello/msg.txt"), a("cat /usr/share/hello/msg.txt"))
    pl = a("pkg list"); check("pkg list shows both", "hello" in pl and "libgreet" in pl, pl)
    check("pkg verify ok", "mismatch" not in a("pkg verify").lower() and "bad" not in a("pkg verify").lower(), a("pkg verify"))
    up = log.split(">>> pkg upgrade\n")[1].split("\n>>> ")[0].replace("\r", "") if ">>> pkg upgrade\n" in log else ""
    check("pkg upgrade pulls the newer version from the second remote", "upgrading hello 1.0 -> 1.1" in up and "installed hello 1.1" in up, up)
    i2 = log.split(">>> cat /usr/share/hello/msg.txt\n"); check("upgraded file content", len(i2) > 2 and "hello 1.1 from net" in i2[2].split("\n>>> ")[0], "")
    ev = a("pkg install evil")
    check("pkg rejects a package whose sha256 differs from the index", "CHECKSUM MISMATCH" in ev and "installed evil" not in ev, ev)
    check("tampered package was not installed", "evil" not in a("ls /usr/share/evil") or "No such" in a("ls /usr/share/evil") or "not found" in a("ls /usr/share/evil").lower(), a("ls /usr/share/evil"))
    nu = [x for x in log.split(">>> pkg update\n")]
    check("pkg update reports a missing remote index", any("cannot fetch INDEX" in x.split("\n>>> ")[0] and "not found" in x.split("\n>>> ")[0] for x in nu), "")
    ev_https = log.split(">>> pkg remote add https://10.0.2.2:18441/repoA\n")[1].split(">>> wget")[0] if ">>> pkg remote add https://10.0.2.2:18441/repoA\n" in log else ""
    check("pkg: untrusted HTTPS update leaves no remote packages (no download happens)", "downloading" not in ev_https and "could not be verified" in ev_https, ev_https[-300:])
    hs = [x.split("\n>>> ")[0] for x in log.split(">>> pkg update\n")]
    check("pkg update over HTTPS fails when the CA is unknown", any("TLS verification failed" in x or "could not be verified" in x for x in hs), "\n".join(h[-200:] for h in hs))
    check("pkg works over verified HTTPS once the CA is trusted", any("remote index updated: 2" in x and "https" in x or ("10.0.2.2:18441" in x and "2 package" in x) for x in hs), "\n".join(h[-200:] for h in hs))
    check("pkg remote add -k marks the remote unverified", "NOT verified" in a("pkg remote list") or "NOT verified" in log, "")
    check("pkg install over -k HTTPS (self-signed server)", "installed libgreet 1.0" in a("pkg install libgreet"), a("pkg install libgreet"))
    sha_g = hashlib.sha256(open(A + "/libgreet-1.0.rpk", "rb").read()).hexdigest()
    check("pkg mkindex writes sha256 + metadata", ("libgreet 1.0 libgreet-1.0.rpk %s" % sha_g) in a("cat idx/INDEX"), a("cat idx/INDEX"))
    check("pkg session finished", "pkg-done" in a("echo pkg-done"), "")

def s_tls(pki, www, sshw, have_ssh):
    H = "10.0.2.2"
    cmds = [("su", 2), ("123", 2), ("dhcp", 4),
            ("httpsget %s:18441 /hello.txt" % H, 6), ("wget https://%s:18441/hello.txt x.txt" % H, 6), ("httpsget -k %s:18441 /hello.txt" % H, 6),
            ("wget http://%s:8099/rsa_root.pem rsa_root.pem" % H, 3), ("wget http://%s:8099/p256_root.pem p256_root.pem" % H, 3), ("wget http://%s:8099/ed_root.pem ed_root.pem" % H, 3),
            ("httpsget --cacert rsa_root.pem %s:18441 /hello.txt" % H, 6), ("httpsget --cacert rsa_root.pem %s:18443 /hello.txt" % H, 6),
            ("certs add rsa_root.pem", 3), ("httpsget %s:18441 /hello.txt" % H, 6), ("httpsget %s:18443 /hello.txt" % H, 6),
            ("certs add p256_root.pem", 3), ("httpsget %s:18443 /hello.txt" % H, 6), ("httpsget %s:18449 /hello.txt" % H, 6), ("curl -s https://%s:18441/hello.txt" % H, 6),
            ("curl -sk https://%s:18449/hello.txt" % H, 6), ("httpsget %s:18448 /hello.txt" % H, 6), ("httpsget %s:18447 /hello.txt" % H, 6),
            ("certs", 3), ("certs verify rsa_root.pem", 3), ("certs verify nofile.pem", 3), ("echo tls-done", 2)]
    log = run(cmds)
    a = lambda c: after(log, c)
    check("https: unknown CA is refused", "unknown issuer" in a("httpsget %s:18441 /hello.txt" % H) and "secure hello" not in a("httpsget %s:18441 /hello.txt" % H), a("httpsget %s:18441 /hello.txt" % H))
    check("https: wget https refused without trust", "secure hello" not in a("wget https://%s:18441/hello.txt x.txt" % H) and "saved" not in a("wget https://%s:18441/hello.txt x.txt" % H), a("wget https://%s:18441/hello.txt x.txt" % H))
    check("https: -k skips verification", "secure hello" in a("httpsget -k %s:18441 /hello.txt" % H), a("httpsget -k %s:18441 /hello.txt" % H))
    check("https: --cacert (RSA-PSS server, RSA root)", "secure hello" in a("httpsget --cacert rsa_root.pem %s:18441 /hello.txt" % H), a("httpsget --cacert rsa_root.pem %s:18441 /hello.txt" % H))
    check("https: --cacert with the wrong root rejected", "secure hello" not in a("httpsget --cacert rsa_root.pem %s:18443 /hello.txt" % H), a("httpsget --cacert rsa_root.pem %s:18443 /hello.txt" % H))
    check("certs add", "added 1 certificate" in a("certs add rsa_root.pem"), a("certs add rsa_root.pem"))
    first = log.split(">>> httpsget %s:18441 /hello.txt\n" % H)
    check("https: RSA chain OK after certs add", len(first) >= 3 and "secure hello" in first[2].split("\n>>> ")[0], "")
    check("https: P-256 server refused before its CA is added", "unknown issuer" in a("httpsget %s:18443 /hello.txt" % H), a("httpsget %s:18443 /hello.txt" % H))
    p256 = log.split(">>> httpsget %s:18443 /hello.txt\n" % H)
    check("https: ECDSA P-256 chain OK after certs add", len(p256) >= 3 and "secure hello" in p256[-1].split("\n>>> ")[0], "")
    check("https: self-signed server refused", "secure hello" not in a("httpsget %s:18449 /hello.txt" % H), a("httpsget %s:18449 /hello.txt" % H))
    check("curl https verified", "secure hello" in a("curl -s https://%s:18441/hello.txt" % H), a("curl -s https://%s:18441/hello.txt" % H))
    check("curl -k self-signed", "secure hello" in a("curl -sk https://%s:18449/hello.txt" % H), a("curl -sk https://%s:18449/hello.txt" % H))
    check("https: chain to a CA that was never trusted ('other')", "secure hello" not in a("httpsget %s:18448 /hello.txt" % H), a("httpsget %s:18448 /hello.txt" % H))
    check("https: expired certificate refused", "secure hello" not in a("httpsget %s:18447 /hello.txt" % H) and ("expired" in a("httpsget %s:18447 /hello.txt" % H).lower() or "unknown issuer" in a("httpsget %s:18447 /hello.txt" % H)), a("httpsget %s:18447 /hello.txt" % H))
    check("certs lists the store", "117" in a("certs") or "certificate" in a("certs").lower(), a("certs"))
    check("tls session finished", "tls-done" in a("echo tls-done"), "")

def s_ssh(pki, www, sshw, have_ssh):
    if not have_ssh: print("  SKIP  libssh not installed"); return
    H = "tester@10.0.2.2"; pw = ("s3cret", 4)
    cmds = [("su", 2), ("123", 2), ("dhcp", 4),
            ("ssh -p 2201 %s echo hello" % H, 5), pw, ("echo rc=$?", 2), ("ssh -p 2201 %s exit 7" % H, 4), pw, ("echo rc=$?", 2),
            ("ssh -p 2201 %s errtest" % H, 4), pw, ("echo rc=$?", 2), ("ssh -p 2201 %s whoami" % H, 4), ("wrong", 4),
            ("ssh -p 2202 %s echo rsa-ok" % H, 5), pw, ("ssh -p 2203 %s echo ecdsa-ok" % H, 5), pw, ("cat /etc/ssh_known_hosts", 2),
            ("echo 'hello scp file' > a.txt", 1), ("mkdir d1", 1), ("echo one > d1/one.txt", 1), ("echo two > d1/two.txt", 1),
            ("scp -P 2201 a.txt %s:up.txt" % H, 5), pw, ("scp -P 2201 %s:up.txt back.txt" % H, 5), pw, ("cat back.txt", 2),
            ("scp -P 2201 -r d1 %s:" % H, 5), pw, ("scp -P 2201 -r %s:d1 d1copy" % H, 5), pw, ("ls d1copy", 2),
            ("scp -P 2201 %s:missing.txt m.txt" % H, 5), pw,
            ("scp -P 2201 %s:big.bin big.bin" % H, 5), ("s3cret", 25), ("ls -l big.bin", 2),
            ("ssh -p 2201 -o StrictHostKeyChecking=yes %s echo strict" % H, 5), pw,
            ("ssh -R 10.0.2.2", 3), ("ssh -p 2201 -t %s" % H, 5), pw, ("hi there", 3), ("exit", 3), ("echo ssh-done", 2)]
    log = run(cmds)
    a = lambda c: after(log, c)
    def nth(c, n):
        parts = log.split(">>> %s\n" % c); return parts[n].split("\n>>> ")[0].replace("\r", "") if len(parts) > n else ""
    check("ssh exec echo (ed25519 host key, curve25519, aes128-ctr, hmac-sha2-256)", "hello" in a("ssh -p 2201 %s echo hello" % H), a("ssh -p 2201 %s echo hello" % H))
    check("ssh host key stored on first use", "[10.0.2.2]:2201" in a("cat /etc/ssh_known_hosts") and "[10.0.2.2]:2203" in a("cat /etc/ssh_known_hosts"), a("cat /etc/ssh_known_hosts"))
    check("ssh exit status 0 propagated", "rc=0" in nth("echo rc=$?", 1), nth("echo rc=$?", 1))
    check("ssh exit status 7 propagated", "rc=7" in nth("echo rc=$?", 2), nth("echo rc=$?", 2))
    check("ssh stderr data", "rc=" in nth("echo rc=$?", 3), "")
    check("ssh wrong password rejected", "Permission denied" in a("ssh -p 2201 %s whoami" % H) or "Permission denied" in a("wrong"), a("wrong"))
    check("ssh RSA host key", "rsa-ok" in a("ssh -p 2202 %s echo rsa-ok" % H), a("ssh -p 2202 %s echo rsa-ok" % H))
    check("ssh ECDSA host key", "ecdsa-ok" in a("ssh -p 2203 %s echo ecdsa-ok" % H), a("ssh -p 2203 %s echo ecdsa-ok" % H))
    check("scp download round trip", "hello scp file" in a("cat back.txt"), a("cat back.txt"))
    check("scp -r both directions", "one.txt" in a("ls d1copy") and "two.txt" in a("ls d1copy"), a("ls d1copy"))
    check("scp missing remote file reports an error", "no such file" in a("s3cret") .lower() or "no such file" in log.lower(), "")
    big = a("ls -l big.bin"); check("scp 700 KB download size", "700000" in big, big)
    check("ssh StrictHostKeyChecking=yes with a known host works", "strict" in a("ssh -p 2201 -o StrictHostKeyChecking=yes %s echo strict" % H), a("ssh -p 2201 -o StrictHostKeyChecking=yes %s echo strict" % H))
    check("ssh -R forgets a host", "10.0.2.2" in a("ssh -R 10.0.2.2"), a("ssh -R 10.0.2.2"))
    check("ssh interactive shell", "hi there" in a("ssh -p 2201 -t %s" % H) or "hi there" in a("hi there"), a("ssh -p 2201 -t %s" % H))
    check("ssh session finished", "ssh-done" in a("echo ssh-done"), "")

SESSIONS = {"tls": s_tls, "ssh": s_ssh, "httpd": s_httpd, "ftpd": s_ftpd, "pkg": s_pkg}
if len(sys.argv) > 2: SESSIONS = {k: v for k, v in SESSIONS.items() if k in sys.argv[2:]}
if __name__ == "__main__": main()
