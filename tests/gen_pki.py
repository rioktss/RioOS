#!/usr/bin/env python3
"""Generate test PKI chains under a directory: usage gen_pki.py <outdir>. Used by x509_host_test and the QEMU regression."""
import datetime, ipaddress, os, sys
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, ed25519
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
now = datetime.datetime.now(datetime.timezone.utc)
def name(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
def pem(o): return o.public_bytes(serialization.Encoding.PEM)
def keypem(k): return k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
def mk(subject, key, issuer_name, issuer_key, ca, san=None, days=365, nb_off=-1, pathlen=None, sighash=hashes.SHA256()):
    b = (x509.CertificateBuilder().subject_name(name(subject)).issuer_name(issuer_name).public_key(key.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(now + datetime.timedelta(days=nb_off)).not_valid_after(now + datetime.timedelta(days=days)))
    b = b.add_extension(x509.BasicConstraints(ca=ca, path_length=pathlen), critical=True)
    if ca: b = b.add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), critical=True)
    if san: b = b.add_extension(x509.SubjectAlternativeName(san), critical=False)
    algo = None if isinstance(issuer_key, ed25519.Ed25519PrivateKey) else sighash
    return b.sign(issuer_key, algo)
def chain(tag, root_key, int_key, leaf_key, sighash=hashes.SHA256(), leaf_san=None, leaf_days=365, leaf_nb=-1):
    root = mk(tag + " Root CA", root_key, name(tag + " Root CA"), root_key, True, days=3650, sighash=sighash)
    inter = mk(tag + " Intermediate CA", int_key, root.subject, root_key, True, days=1800, pathlen=0, sighash=sighash)
    san = leaf_san or [x509.DNSName("test.rio.local"), x509.DNSName("*.wild.rio.local"), x509.IPAddress(ipaddress.IPv4Address("10.0.2.2"))]
    leaf = mk("test.rio.local", leaf_key, inter.subject, int_key, False, san=san, days=leaf_days, nb_off=leaf_nb, sighash=sighash)
    open(f"{out}/{tag}_root.pem", "wb").write(pem(root))
    open(f"{out}/{tag}_chain.pem", "wb").write(pem(leaf) + pem(inter))
    open(f"{out}/{tag}_leaf.pem", "wb").write(pem(leaf))
    open(f"{out}/{tag}_full.pem", "wb").write(pem(leaf) + pem(inter) + pem(root))
    open(f"{out}/{tag}_key.pem", "wb").write(keypem(leaf_key))
    return root, inter, leaf
chain("rsa", rsa.generate_private_key(65537, 2048), rsa.generate_private_key(65537, 2048), rsa.generate_private_key(65537, 2048))
chain("rsa4k", rsa.generate_private_key(65537, 4096), rsa.generate_private_key(65537, 2048), rsa.generate_private_key(65537, 2048), sighash=hashes.SHA384())
chain("p256", ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1()))
chain("p384", ec.generate_private_key(ec.SECP384R1()), ec.generate_private_key(ec.SECP384R1()), ec.generate_private_key(ec.SECP384R1()), sighash=hashes.SHA384())
chain("mixed", rsa.generate_private_key(65537, 2048), ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1()))
chain("ed", ed25519.Ed25519PrivateKey.generate(), ed25519.Ed25519PrivateKey.generate(), ed25519.Ed25519PrivateKey.generate())
chain("expired", rsa.generate_private_key(65537, 2048), rsa.generate_private_key(65537, 2048), rsa.generate_private_key(65537, 2048), leaf_days=-1, leaf_nb=-30)
# unrelated CA (must not validate)
chain("other", rsa.generate_private_key(65537, 2048), rsa.generate_private_key(65537, 2048), rsa.generate_private_key(65537, 2048))
# self-signed leaf
k = rsa.generate_private_key(65537, 2048)
ss = mk("test.rio.local", k, name("test.rio.local"), k, False, san=[x509.DNSName("test.rio.local"), x509.IPAddress(ipaddress.IPv4Address("10.0.2.2"))])
open(f"{out}/self_leaf.pem", "wb").write(pem(ss)); open(f"{out}/self_key.pem", "wb").write(keypem(k))
print("pki ok")
