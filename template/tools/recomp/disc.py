"""Minimal ISO 9660 reader over a raw MODE2/2352 .bin image (used by the oracle's CD hooks; the C runtime has its own reader in runtime/disc.c).
User data of a sector = 2048 bytes at offset 24 of the 2352-byte frame (pitfall 3). Only what the hooks need: path lookup and sector reads."""
import struct

SECTOR = 2352
HDR = 24


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read(16, 1)
        if pvd[1:6] != b"CD001":
            raise ValueError("no ISO 9660 primary volume descriptor")
        self.root = (struct.unpack("<I", pvd[156 + 2:156 + 6])[0], struct.unpack("<I", pvd[156 + 10:156 + 14])[0])

    def read(self, lba, n):
        out = bytearray()
        for k in range(n):
            self.f.seek((lba + k) * SECTOR)
            raw = self.f.read(SECTOR)
            if len(raw) != SECTOR:
                raise ValueError("read past the end of the image at sector %d" % (lba + k))
            out += raw[HDR:HDR + 2048]
        return bytes(out)

    def _dir(self, lba, size):
        data = self.read(lba, (size + 2047) // 2048)[:size]
        off = 0
        while off < len(data):
            ln = data[off]
            if ln == 0:
                off = (off // 2048 + 1) * 2048
                continue
            rec = data[off:off + ln]
            off += ln
            name = rec[33:33 + rec[32]]
            if name in (b"\x00", b"\x01"):
                continue
            yield name.decode("latin1"), struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0], rec[25] & 2

    def find(self, path):
        """`\\DIR\\NAME;1` (case-insensitive, `;version` optional) -> (lba, size, name_as_stored) or None."""
        cur = self.root
        parts = [p for p in path.split("\\") if p]
        found = None
        for k, part in enumerate(parts):
            want = part.upper()
            found = None
            for name, lba, size, isdir in self._dir(*cur):
                if name.upper() == want or name.upper().split(";")[0] == want.split(";")[0] and ";" not in want:
                    found = (lba, size, name, isdir)
                    break
            if found is None:
                return None
            if k < len(parts) - 1:
                if not found[3]:
                    return None
                cur = (found[0], found[1])
        return None if found is None or found[3] else found[:3]


def to_bcd(v):
    return ((v // 10) << 4) | (v % 10)


def from_bcd(b):
    return (b >> 4) * 10 + (b & 15)


def lba_to_loc(lba):
    """Absolute MSF of a logical sector: minute/second/frame of lba + 150 (psx-spx CD-ROM addressing), BCD."""
    n = lba + 150
    return to_bcd(n // 4500), to_bcd((n // 75) % 60), to_bcd(n % 75)


def loc_to_lba(m, s, f):
    return (from_bcd(m) * 60 + from_bcd(s)) * 75 + from_bcd(f) - 150
