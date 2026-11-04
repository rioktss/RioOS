#!/usr/bin/env python3
"""Independent FAT32 reader/writer used by the QEMU regression to cross-check the kernel's `fat` command."""
import struct

def mkimage(path, files, size_mb=8):
    """files: {name: bytes}; names longer than 8.3 get LFN entries. One root directory, 1 sector per cluster."""
    total = size_mb * 2048; spc = 1; reserved = 32; nfats = 2
    fatsz = 1
    for _ in range(8):
        clusters = (total - reserved - nfats * fatsz) // spc
        fatsz = ((clusters + 2) * 4 + 511) // 512
    img = bytearray(total * 512)
    bpb = bytearray(512)
    bpb[0:3] = b'\xeb\x58\x90'; bpb[3:11] = b'HOSTFAT '
    struct.pack_into('<HBHBHHBHHHII', bpb, 11, 512, spc, reserved, nfats, 0, 0, 0xF8, 0, 32, 64, 0, total)
    struct.pack_into('<IHHI', bpb, 36, fatsz, 0, 0, 2)
    struct.pack_into('<HH', bpb, 48, 1, 6)
    bpb[64] = 0x80; bpb[66] = 0x29; bpb[71:82] = b'HOSTIMG    '; bpb[82:90] = b'FAT32   '
    bpb[510:512] = b'\x55\xaa'
    img[0:512] = bpb
    fat = [0] * (fatsz * 128)
    fat[0] = 0x0FFFFFF8; fat[1] = 0x0FFFFFFF; fat[2] = 0x0FFFFFFF
    data_start = reserved + nfats * fatsz
    nxt = 3
    ents = bytearray()
    def csec(c): return data_start + (c - 2) * spc
    for name, content in files.items():
        first = 0; n = (len(content) + 511) // 512
        for i in range(n):
            c = nxt; nxt += 1
            if i == 0: first = c
            fat[c] = 0x0FFFFFFF
            if i: fat[c - 1] = c
            img[csec(c) * 512: csec(c) * 512 + 512] = content[i * 512:(i + 1) * 512].ljust(512, b'\0')
        base, _, ext = name.rpartition('.') if '.' in name else (name, '', '')
        short = (base.upper()[:6] + '~1').ljust(8) if (len(base) > 8 or len(ext) > 3 or name != name.upper()) else base.ljust(8)
        short = short + ext.upper()[:3].ljust(3)
        sb = short.encode()
        cks = 0
        for b in sb: cks = ((((cks & 1) << 7) | (cks >> 1)) + b) & 0xFF
        need_lfn = (len(base) > 8 or len(ext) > 3 or name != name.upper())
        if need_lfn:
            chars = [ord(c) for c in name] + [0]
            while len(chars) % 13: chars.append(0xFFFF)
            parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
            for idx in range(len(parts) - 1, -1, -1):
                e = bytearray(32); e[0] = (idx + 1) | (0x40 if idx == len(parts) - 1 else 0); e[11] = 0x0F; e[13] = cks
                p = parts[idx]
                struct.pack_into('<5H', e, 1, *p[0:5]); struct.pack_into('<6H', e, 14, *p[5:11]); struct.pack_into('<2H', e, 28, *p[11:13])
                ents += e
        e = bytearray(32); e[0:11] = sb; e[11] = 0x20
        struct.pack_into('<H', e, 20, first >> 16); struct.pack_into('<H', e, 26, first & 0xFFFF); struct.pack_into('<I', e, 28, len(content))
        struct.pack_into('<H', e, 24, 0x5A21)
        ents += e
    assert len(ents) <= 512, "root dir limited to one sector in this helper"
    img[csec(2) * 512: csec(2) * 512 + len(ents)] = ents
    fb = b''.join(struct.pack('<I', v) for v in fat)
    for f in range(nfats):
        o = (reserved + f * fatsz) * 512
        img[o:o + len(fb)] = fb
    open(path, 'wb').write(img)

def read_image(path):
    """Returns {path: bytes} for every file in the image (recursive), parsing long names."""
    d = open(path, 'rb').read()
    bps, spc, reserved, nfats = struct.unpack_from('<HBHB', d, 11)
    fatsz, = struct.unpack_from('<I', d, 36); root, = struct.unpack_from('<I', d, 44)
    data_start = reserved + nfats * fatsz
    def fat(c): return struct.unpack_from('<I', d, reserved * 512 + c * 4)[0] & 0x0FFFFFFF
    def chain(c):
        out = []
        while 2 <= c < 0x0FFFFFF8:
            out.append(c); c = fat(c)
            if len(out) > 100000: raise Exception("loop")
        return out
    def cdata(c): return b''.join(d[(data_start + (x - 2) * spc) * 512:(data_start + (x - 2) * spc + spc) * 512] for x in chain(c))
    res = {}
    def walk(cl, prefix):
        raw = cdata(cl); lname = {}
        i = 0
        while i < len(raw):
            e = raw[i:i + 32]; i += 32
            if e[0] == 0: break
            if e[0] == 0xE5: lname = {}; continue
            if e[11] == 0x0F:
                seq = e[0] & 0x1F
                chars = struct.unpack_from('<5H', e, 1) + struct.unpack_from('<6H', e, 14) + struct.unpack_from('<2H', e, 28)
                lname[seq] = ''.join(chr(c) for c in chars if c not in (0, 0xFFFF)); continue
            if e[11] & 0x08: lname = {}; continue
            nm = e[0:8].decode().rstrip(); ex = e[8:11].decode().rstrip()
            short = nm + ('.' + ex if ex else '')
            name = ''.join(lname[k] for k in sorted(lname)) if lname else short
            lname = {}
            if name in ('.', '..'): continue
            c = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            size, = struct.unpack_from('<I', e, 28)
            if e[11] & 0x10: walk(c, prefix + name + '/')
            else: res[prefix + name] = cdata(c)[:size] if c else b''
    walk(root, '')
    return res

if __name__ == '__main__':
    import sys
    for k, v in read_image(sys.argv[1]).items(): print(k, len(v))
