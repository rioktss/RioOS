#!/usr/bin/env python3
"""Generates tests/crypto_vectors.txt (signatures made by the `cryptography` package) for crypto_host_test."""
import hashlib, os, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, ed25519, padding, utils
H = {1: hashes.SHA256, 2: hashes.SHA384, 3: hashes.SHA512}
HL = {1: hashlib.sha256, 2: hashlib.sha384, 3: hashlib.sha512}
out = []
def hx(b): return b.hex() if b else "-"
for i in range(3):
    m = os.urandom(10 + i * 100)
    out.append("SHA %d %s %s" % (3, hx(m), HL[3](m).hexdigest()))
    out.append("SHA %d %s %s" % (2, hx(m), HL[2](m).hexdigest()))
for bits in (2048, 3072, 4096):
    k = rsa.generate_private_key(65537, bits)
    pn = k.public_key().public_numbers()
    n = pn.n.to_bytes(bits // 8, "big"); e = pn.e.to_bytes(3, "big")
    for h in (1, 2, 3):
        m = os.urandom(50)
        d = HL[h](m).digest()
        s1 = k.sign(m, padding.PKCS1v15(), H[h]())
        out.append("RSA1 %d %s %s %s %s 1" % (h, n.hex(), e.hex(), d.hex(), s1.hex()))
        bad = bytearray(s1); bad[-1] ^= 1
        out.append("RSA1 %d %s %s %s %s 0" % (h, n.hex(), e.hex(), d.hex(), bytes(bad).hex()))
        s2 = k.sign(m, padding.PSS(padding.MGF1(H[h]()), H[h]().digest_size), H[h]())
        out.append("RSAPSS %d %s %s %s %s 1" % (h, n.hex(), e.hex(), d.hex(), s2.hex()))
        bad = bytearray(s2); bad[5] ^= 1
        out.append("RSAPSS %d %s %s %s %s 0" % (h, n.hex(), e.hex(), d.hex(), bytes(bad).hex()))
for curve, cid, h in ((ec.SECP256R1(), 1, 1), (ec.SECP384R1(), 2, 2), (ec.SECP256R1(), 1, 2), (ec.SECP384R1(), 2, 1)):
    for _ in range(4):
        k = ec.generate_private_key(curve)
        pub = k.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        m = os.urandom(40)
        d = HL[h](m).digest()
        sig = k.sign(m, ec.ECDSA(H[h]()))
        out.append("ECDSA %d %s %s %s 1" % (cid, pub.hex(), d.hex(), sig.hex()))
        bad = bytearray(d); bad[0] ^= 1
        out.append("ECDSA %d %s %s %s 0" % (cid, pub.hex(), bytes(bad).hex(), sig.hex()))
for _ in range(6):
    k = ed25519.Ed25519PrivateKey.generate()
    pub = k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    m = os.urandom(_ * 37)
    sig = k.sign(m)
    out.append("ED25519 %s %s %s 1" % (pub.hex(), hx(m), sig.hex()))
    bad = bytearray(sig); bad[40] ^= 4
    out.append("ED25519 %s %s %s 0" % (pub.hex(), hx(m), bytes(bad).hex()))
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "crypto_vectors.txt"), "w").write("\n".join(out) + "\n")
print(len(out), "vectors")
