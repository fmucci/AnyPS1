#!/usr/bin/env python3
"""Extract the files of a MODE2/2352 image that contain XA/Form 2/video sectors as RAW sectors (queue P0-5).
usage: python3 -I tools/raw_extract.py "disc1.bin" OUT_DIR   > manifest.sha256

For every ISO9660 file at least one of whose sectors is not plain Form-1 data (Form 2 bit, or audio/video submode bits),
writes OUT_DIR/<path> as consecutive 2336-byte records = raw sector bytes 16..2351 (8-byte subheader + 2328 bytes: Form 2 data is
2324 bytes + 4 EDC; Form 1 is 2048 data + 4 EDC + 276 ECC). Sector count = ceil(directory size / 2048), the same extent
tools/disc_probe.py uses. Prints "sha256  path" lines (path relative to the cwd) for each written file.
Plain-data files are skipped: tools/iso_extract.py already covers them (2048 user bytes per sector)."""
import hashlib
import os
import struct
import sys

path, out = sys.argv[1], sys.argv[2]
f = open(path, "rb")


def raw(n):
    f.seek(n * 2352)
    return f.read(2352)


pvd = raw(16)[24:24 + 2048]
assert pvd[1:6] == b"CD001"
files = []


def walk(lba, size, p):
    data = b"".join(raw(lba + i)[24:24 + 2048] for i in range((size + 2047) // 2048))[:size]
    off = 0
    while off < len(data):
        l = data[off]
        if l == 0:
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + l]
        off += l
        elba, esize, fl = struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0], rec[25]
        nm = rec[33:33 + rec[32]]
        if nm in (b"\x00", b"\x01"):
            continue
        nm = nm.decode("latin1").split(";")[0]
        if fl & 2:
            walk(elba, esize, p + nm + "/")
        else:
            files.append((p + nm, elba, esize))


walk(*struct.unpack("<I", pvd[158:162]) + struct.unpack("<I", pvd[166:170]), "")
for name, lba, size in sorted(files):
    ns = (size + 2047) // 2048
    special = False
    for i in range(ns):
        sm = raw(lba + i)[18]
        if sm & 0x20 or sm & 0x06:
            special = True
            break
    if not special:
        continue
    dst = os.path.join(out, name)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    h = hashlib.sha256()
    with open(dst, "wb") as o:
        for i in range(ns):
            b = raw(lba + i)[16:2352]
            o.write(b)
            h.update(b)
    print("%s  %s" % (h.hexdigest(), dst))
