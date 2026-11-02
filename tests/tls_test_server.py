"""Local TLS 1.3 HTTP server for the HTTPS regression (port 18443); usage: tls_test_server.py <cert.pem> <key.pem> <docroot>"""
import ssl, http.server, os, sys
cert, key, root = sys.argv[1:4]
os.chdir(root)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(cert, key)
h = http.server.SimpleHTTPRequestHandler
h.protocol_version = "HTTP/1.0"
h.log_message = lambda *a, **k: None
s = http.server.HTTPServer(("127.0.0.1", 18443), h)
s.socket = ctx.wrap_socket(s.socket, server_side=True)
s.serve_forever()
