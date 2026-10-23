#!/usr/bin/env python3
"""Build a tiny raw MODE2/2352 disc image (ISO9660 with SYSTEM.CNF) around PS-X EXE files: lets the toolchain be exercised on programs that do not come on a disc (public test programs,
homebrew) and tested without any commercial disc.   usage: python3 -I tools/mkdisc.py OUT.bin BOOT.EXE [OTHER_FILE ...]
OUT.cue is written next to it. Sector EDC/ECC are zero (our readers do not check them)."""
import os
import struct
import sys


def bcd(n):
    return (n // 10) << 4 | n % 10


def sector(lba, data):
    assert len(data) <= 2048
    m, s, f = divmod(lba + 150, 75 * 60)[0], (lba + 150) // 75 % 60, (lba + 150) % 75
    hdr = bytes([bcd(m), bcd(s), bcd(f), 2])
    sub = bytes([0, 0, 8, 0]) * 2
    return b"\x00" + b"\xff" * 10 + b"\x00" + hdr + sub + data.ljust(2048, b"\0") + b"\0" * 280


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def dirrec(name, lba, size, is_dir=False):
    nm = name.encode()
    n = 33 + len(nm)
    n += n % 2
    r = bytes([n, 0]) + both32(lba) + both32(size) + bytes([100, 1, 1, 0, 0, 0, 0]) + bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([len(nm)]) + nm
    return r.ljust(n, b"\0")


def main():
    out, boot = sys.argv[1], sys.argv[2]
    files = [(os.path.basename(p).upper(), open(p, "rb").read()) for p in sys.argv[2:]]
    bootname = files[0][0]
    cnf = ("BOOT = cdrom:\\%s;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n" % bootname).encode()
    entries = [("SYSTEM.CNF", cnf)] + files
    lba = 19
    placed = []
    for name, data in entries:
        placed.append((name, lba, data))
        lba += max(1, (len(data) + 2047) // 2048)
    root = dirrec("\0", 18, 2048, True) + dirrec("\1", 18, 2048, True) + b"".join(dirrec(n + ";1", l, len(d)) for n, l, d in placed)
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[40:72] = b"MKDISC".ljust(32)
    pvd[80:88] = both32(lba)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(2048)
    pvd[156:190] = dirrec("\0", 18, 2048, True)[:34]
    pvd[881] = 1
    with open(out, "wb") as f:
        for i in range(16):
            f.write(sector(i, b""))
        f.write(sector(16, bytes(pvd)))
        f.write(sector(17, bytes([255]) + b"CD001" + bytes([1])))
        f.write(sector(18, root))
        for _n, l, d in placed:
            for k in range(max(1, (len(d) + 2047) // 2048)):
                f.write(sector(l + k, d[k * 2048:(k + 1) * 2048]))
    open(os.path.splitext(out)[0] + ".cue", "w").write('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % os.path.basename(out))
    print("wrote %s: %d sectors, boot %s" % (out, lba, bootname))


if __name__ == "__main__":
    main()
