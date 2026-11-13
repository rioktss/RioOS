#!/usr/bin/env python3
"""SSH test servers built on the real libssh server library (ctypes), so the RioOS client is checked against an
independent implementation.  usage: ssh_test_server.py <workdir>   binds 10.0.2.2: 2201 ed25519, 2202 rsa, 2203 ecdsa.
login tester / s3cret ; exec: echo, whoami, exit N, errtest, big, scp -t/-f (sandbox = <workdir>/root)"""
import ctypes, os, sys, threading
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, ed25519
work = sys.argv[1]; os.makedirs(work + "/root", exist_ok=True)
L = ctypes.CDLL("libssh.so.4")
vp = ctypes.c_void_p
for n, r, a in [("ssh_bind_new", vp, []), ("ssh_bind_options_set", ctypes.c_int, [vp, ctypes.c_int, ctypes.c_char_p]), ("ssh_bind_listen", ctypes.c_int, [vp]),
                ("ssh_bind_accept", ctypes.c_int, [vp, vp]), ("ssh_new", vp, []), ("ssh_handle_key_exchange", ctypes.c_int, [vp]),
                ("ssh_set_auth_methods", None, [vp, ctypes.c_int]), ("ssh_message_get", vp, [vp]), ("ssh_message_type", ctypes.c_int, [vp]),
                ("ssh_message_subtype", ctypes.c_int, [vp]), ("ssh_message_auth_user", ctypes.c_char_p, [vp]), ("ssh_message_auth_password", ctypes.c_char_p, [vp]),
                ("ssh_message_auth_reply_success", ctypes.c_int, [vp, ctypes.c_int]), ("ssh_message_reply_default", ctypes.c_int, [vp]),
                ("ssh_message_channel_request_open_reply_accept", vp, [vp]), ("ssh_message_channel_request_reply_success", ctypes.c_int, [vp]),
                ("ssh_message_channel_request_command", ctypes.c_char_p, [vp]), ("ssh_message_free", None, [vp]),
                ("ssh_channel_read", ctypes.c_int, [vp, ctypes.c_char_p, ctypes.c_uint, ctypes.c_int]), ("ssh_channel_write", ctypes.c_int, [vp, ctypes.c_char_p, ctypes.c_uint]),
                ("ssh_channel_write_stderr", ctypes.c_int, [vp, ctypes.c_char_p, ctypes.c_uint]),
                ("ssh_channel_send_eof", ctypes.c_int, [vp]), ("ssh_channel_request_send_exit_status", ctypes.c_int, [vp, ctypes.c_int]),
                ("ssh_channel_close", ctypes.c_int, [vp]), ("ssh_channel_free", None, [vp]), ("ssh_disconnect", None, [vp]), ("ssh_free", None, [vp]),
                ("ssh_message_auth_pubkey", vp, [vp]), ("ssh_message_auth_publickey_state", ctypes.c_int, [vp]),
                ("ssh_message_auth_reply_pk_ok_simple", ctypes.c_int, [vp]), ("ssh_pki_export_pubkey_base64", ctypes.c_int, [vp, ctypes.POINTER(ctypes.c_char_p)]),
                ("ssh_init", ctypes.c_int, []), ("ssh_get_error", ctypes.c_char_p, [vp])]:
    f = getattr(L, n); f.restype = r; f.argtypes = a
L.ssh_init()

def keyfile(name, key):
    p = "%s/%s_host_key" % (work, name)
    open(p, "wb").write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.OpenSSH, serialization.NoEncryption()))
    os.chmod(p, 0o600); return p
KEYS = {2201: ("ed25519", ed25519.Ed25519PrivateKey.generate()), 2202: ("rsa", rsa.generate_private_key(65537, 2048)), 2203: ("ecdsa", ec.generate_private_key(ec.SECP256R1()))}

def read_exact(ch, n):
    out = b""
    while len(out) < n:
        buf = ctypes.create_string_buffer(n - len(out)); r = L.ssh_channel_read(ch, buf, n - len(out), 0)
        if r <= 0: raise EOFError
        out += buf.raw[:r]
    return out
def read_line(ch):
    s = b""
    while True:
        c = read_exact(ch, 1)
        if c == b"\n": return s.decode()
        s += c
def wr(ch, data):
    off = 0
    while off < len(data):
        r = L.ssh_channel_write(ch, data[off:off + 16384], min(16384, len(data) - off))
        if r <= 0: raise EOFError
        off += r
def safe(path):
    p = os.path.normpath(os.path.join(work, "root", path.lstrip("/")))
    assert p.startswith(work + "/root"), "path escape"
    return p

def scp_sink(ch, recursive, target):
    wr(ch, b"\0"); stack = [safe(target)]; first = True
    while True:
        try: line = read_line(ch)
        except EOFError: return
        t = line[0]
        if t == "C":
            mode, size, name = line[1:].split(" ", 2); size = int(size); wr(ch, b"\0")
            data = read_exact(ch, size); read_exact(ch, 1)
            dest = stack[-1]
            if os.path.isdir(dest): dest = os.path.join(dest, name)
            open(dest, "wb").write(data); os.chmod(dest, int(mode, 8)); wr(ch, b"\0")
        elif t == "D":
            mode, size, name = line[1:].split(" ", 2); d = os.path.join(stack[-1], name) if os.path.isdir(stack[-1]) else stack[-1]
            os.makedirs(d, exist_ok=True); stack.append(d); wr(ch, b"\0")
        elif t == "E":
            stack.pop(); wr(ch, b"\0")
            if not stack: return
        else:
            wr(ch, b"\x01bad scp command\n"); return
def scp_src_one(ch, path, recursive):
    name = os.path.basename(path)
    if os.path.isdir(path):
        if not recursive: wr(ch, b"\x01not a regular file\n"); return False
        wr(ch, ("D0755 0 %s\n" % name).encode()); read_exact(ch, 1)
        for e in sorted(os.listdir(path)): scp_src_one(ch, os.path.join(path, e), recursive)
        wr(ch, b"E\n"); read_exact(ch, 1)
    else:
        data = open(path, "rb").read()
        wr(ch, ("C%04o %d %s\n" % (os.stat(path).st_mode & 0o777, len(data), name)).encode()); read_exact(ch, 1)
        wr(ch, data + b"\0"); read_exact(ch, 1)
    return True
def scp_source(ch, recursive, target):
    read_exact(ch, 1)
    p = safe(target)
    if not os.path.exists(p): wr(ch, b"\x01no such file or directory\n"); return 1
    scp_src_one(ch, p, recursive); return 0

def authorized_b64():
    out = set()
    try:
        for ln in open(work + "/root/.ssh/authorized_keys"):
            f = ln.split()
            if len(f) >= 2 and f[0] == "ssh-ed25519": out.add(f[1])
    except OSError: pass
    return out
os.makedirs(work + "/root/.ssh", exist_ok=True)

def serve(sess, user_holder):
    chan = None; cmd = None; shell = False
    while True:
        msg = L.ssh_message_get(sess)
        if not msg: break
        t = L.ssh_message_type(msg); st = L.ssh_message_subtype(msg)
        if t == 1:
            if st == 2 and L.ssh_message_auth_user(msg) == b"tester" and L.ssh_message_auth_password(msg) == b"s3cret":
                user_holder.append("password"); L.ssh_message_auth_reply_success(msg, 0)
            elif st == 4 and L.ssh_message_auth_user(msg) == b"tester":
                k = L.ssh_message_auth_pubkey(msg); b64 = ctypes.c_char_p()
                ok = bool(k) and L.ssh_pki_export_pubkey_base64(k, ctypes.byref(b64)) == 0 and b64.value.decode() in authorized_b64()
                if ok and L.ssh_message_auth_publickey_state(msg) == 1:
                    user_holder.append("publickey"); L.ssh_message_auth_reply_success(msg, 0)
                elif ok: L.ssh_message_auth_reply_pk_ok_simple(msg)
                else: L.ssh_set_auth_methods(sess, 6); L.ssh_message_reply_default(msg)
            else:
                L.ssh_set_auth_methods(sess, 6); L.ssh_message_reply_default(msg)
        elif t == 2:
            chan = L.ssh_message_channel_request_open_reply_accept(msg)
            if not chan: L.ssh_message_reply_default(msg)
        elif t == 3:
            c = L.ssh_message_channel_request_command(msg)
            if c is not None: cmd = c.decode(); L.ssh_message_channel_request_reply_success(msg); L.ssh_message_free(msg); break
            if st == 1: L.ssh_message_channel_request_reply_success(msg)               # pty
            elif st == 3: shell = True; L.ssh_message_channel_request_reply_success(msg); L.ssh_message_free(msg); break
            else: L.ssh_message_reply_default(msg)
        else: L.ssh_message_reply_default(msg)
        L.ssh_message_free(msg)
    if not chan or (cmd is None and not shell): return
    status = 0
    try:
        if shell:
            wr(chan, b"rio-test$ ")
            line = b""
            while True:
                buf = ctypes.create_string_buffer(1); r = L.ssh_channel_read(chan, buf, 1, 0)
                if r <= 0: break
                if buf.raw == b"\r":
                    cmdl = line.decode().strip(); line = b""
                    if cmdl == "exit": wr(chan, b"\r\nbye\r\n"); break
                    wr(chan, b"\r\n" + (("you said: %s\r\n" % cmdl).encode() if cmdl else b"") + b"rio-test$ ")
                else:
                    line += buf.raw; wr(chan, buf.raw)
        elif cmd.startswith("echo "): wr(chan, (cmd[5:] + "\n").encode())
        elif cmd == "whoami": wr(chan, b"tester\n")
        elif cmd == "authinfo": wr(chan, ("auth=%s\n" % (user_holder[-1] if user_holder else "none")).encode())
        elif cmd.startswith("exit "): status = int(cmd[5:])
        elif cmd == "errtest": L.ssh_channel_write_stderr(chan, b"to stderr\n", 10); wr(chan, b"to stdout\n"); status = 3
        elif cmd == "big": wr(chan, b"".join(b"line %06d abcdefghijklmnopqrstuvwxyz\n" % i for i in range(8000)))
        elif cmd.startswith("scp "):
            parts = cmd.split(); rec = "-r" in parts; tgt = cmd.split("'", 1)[1].rsplit("'", 1)[0].replace("'\\''", "'") if "'" in cmd else parts[-1]
            status = 0 if "-t" in parts else scp_source(chan, rec, tgt)
            if "-t" in parts: scp_sink(chan, rec, tgt)
        else: L.ssh_channel_write_stderr(chan, b"command not found\n", 18); status = 127
    except EOFError: pass
    L.ssh_channel_request_send_exit_status(chan, status); L.ssh_channel_send_eof(chan); L.ssh_channel_close(chan)
    # let the client finish
    buf = ctypes.create_string_buffer(64)
    while L.ssh_channel_read(chan, buf, 64, 0) > 0: pass

def listener(port):
    name, key = KEYS[port]
    b = L.ssh_bind_new()
    L.ssh_bind_options_set(b, 0, b"10.0.2.2"); L.ssh_bind_options_set(b, 2, str(port).encode())
    L.ssh_bind_options_set(b, 3, keyfile(name, key).encode())
    if L.ssh_bind_listen(b) < 0: print("listen failed", port, flush=True); return
    while True:
        s = L.ssh_new()
        if L.ssh_bind_accept(b, s) < 0: L.ssh_free(s); continue
        def run(s=s):
            try:
                if L.ssh_handle_key_exchange(s) == 0:
                    L.ssh_set_auth_methods(s, 6); serve(s, [])
            except Exception as e: print("session error", e, flush=True)
            L.ssh_disconnect(s); L.ssh_free(s)
        threading.Thread(target=run, daemon=True).start()
for p in KEYS: threading.Thread(target=listener, args=(p,), daemon=True).start()
print("ready", flush=True); threading.Event().wait()
