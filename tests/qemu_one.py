#!/usr/bin/env python3
"""usage: qemu_one.py <qemu> <wait-seconds> [-smp N] -- cmd [cmd...]   (runs commands as 'su 123' session, prints log)"""
import sys, os, shutil
sys.argv_saved = sys.argv
q = sys.argv[1]; wait = int(sys.argv[2]); rest = sys.argv[3:]
extra = []
if rest and rest[0] == "-smp": extra = ["-smp", rest[1]]; rest = rest[2:]
if rest and rest[0] == "-net": extra += ["-netdev", "dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=7011,remote.type=inet,remote.host=127.0.0.1,remote.port=7012", "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:56"]; rest = rest[1:]
assert rest[0] == "--"; cmds = rest[1:]
sys.argv = [sys.argv[0], q]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "qemu_regression.py")).read()
head = src.split('shutil.copy(os.path.join(ROOT, "storage", "disk.img")')[0]
exec(compile(head, "reg", "exec"))
shutil.copy(os.path.join(ROOT, "storage", "disk.img"), os.path.join(TMP, "disk.img"))
log = run([("su", None), ("123", None)] + [(c, wait) for c in cmds], extra=extra)
print(log)
