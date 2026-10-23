#!/usr/bin/env python3
"""Per-file CD-XA sector inventory of a MODE2/2352 image.  usage: python3 -I tools/disc_probe.py "disc1.bin" [--all]

For every ISO9660 file: LBA, directory-record size, sectors spanned by the extent (record size / 2048, rounded up),
and the XA subheader tuples found in its sectors (file#, channel, submode, coding) with counts.
Subheader = raw sector bytes 16..23 (psx-spx "CDROM XA Subheader, File, Channel, Interleave"); submode bit 5 = Form 2,
bit 2 = audio, bit 1 = video, bit 3 = data, bit 0 = EOF, bit 7 = EOR. Without --all only files that contain any sector
that is not plain Form 1 data are listed. Reads the image only; nothing is written.
"""
import struct
import sys
from collections import Counter

path = sys.argv[1]
show_all = "--all" in sys.argv
f = open(path, "rb")


def raw(n):
    f.seek(n * 2352)
    return f.read(2352)


def user(n):
    return raw(n)[24:24 + 2048]


pvd = user(16)
assert pvd[1:6] == b"CD001"
files = []


def walk(lba, size, p):
    data = b"".join(user(lba + i) for i in range((size + 2047) // 2048))[:size]
    off = 0
    while off < len(data):
        l = data[off]
        if l == 0:
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + l]
        off += l
        elba, esize, flags = struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0], rec[25]
        name = rec[33:33 + rec[32]]
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("latin1")
        if flags & 2:
            walk(elba, esize, p + name + "/")
        else:
            files.append((p + name, elba, esize))


walk(*struct.unpack("<I", pvd[158:162]) + struct.unpack("<I", pvd[166:170]), "")
print("| file | LBA | record size | sectors | subheader tuples (file,chan,submode,coding)=count |")
print("|---|---:|---:|---:|---|")
tot = Counter()
for name, lba, size in sorted(files, key=lambda x: x[1]):
    ns = (size + 2047) // 2048
    c = Counter()
    for i in range(ns):
        r = raw(lba + i)
        sh = r[16:20]
        c[(sh[0], sh[1], sh[2], sh[3])] += 1
    plain = all(k[2] & 0x20 == 0 and k[2] & 0x0E in (0x08, 0x00) for k in c)
    for k, v in c.items():
        tot[(k[2] & 0x20 != 0, k[2] & 0x0E)] += v
    if show_all or not plain:
        s = "; ".join("(%d,%d,0x%02x,0x%02x)=%d" % (k + (v,)) for k, v in sorted(c.items())[:6])
        if len(c) > 6:
            s += "; ... %d tuples" % len(c)
        print("| %s | %d | %d | %d | %s |" % (name, lba, size, ns, s))
print("\nsector classes over all files (form2?, submode&0x0e) = count: " + ", ".join("%s=%d" % (k, v) for k, v in sorted(tot.items())))
