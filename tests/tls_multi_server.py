#!/usr/bin/env python3
"""HTTPS servers for the certificate-verification tests. usage: tls_multi_server.py <pkidir> <docroot>   (binds 10.0.2.2)
ports: rsa 18441, rsa4k 18442, p256 18443, p384 18444, mixed 18445, ed 18446, expired 18447, other 18448, self 18449"""
import http.server, os, ssl, sys, threading
pki, root = sys.argv[1:3]
os.chdir(root)
PORTS = {"rsa": 18441, "rsa4k": 18442, "p256": 18443, "p384": 18444, "mixed": 18445, "ed": 18446, "expired": 18447, "other": 18448, "self": 18449}
class H(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a, **k): pass
for tag, port in PORTS.items():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    chain = "%s/%s_chain.pem" % (pki, tag) if tag != "self" else "%s/self_leaf.pem" % pki
    ctx.load_cert_chain(chain, "%s/%s_key.pem" % (pki, tag))
    srv = http.server.ThreadingHTTPServer(("10.0.2.2", port), H)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
print("ready", flush=True)
threading.Event().wait()
