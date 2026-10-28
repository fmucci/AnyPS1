#!/usr/bin/env python3
"""Split MODE2/2352 disc images into loose files + a sector map the runtime can read instead of the .bin.

usage: disc_unpack.py [--standalone] OUT_DIR DISC1.bin [DISC2.bin ...]      (a name ending in dN.bin is written to OUT_DIR/dN/, otherwise by position)

Layout per disc (observed format, see docs/findings/0023-unpacked-disc.md):
  dN/disc.map     one line per extent:  <kind> <first_lba> <sectors> <file> [<subheader hex> <first sector within the file>]
                  kind c = cooked  (file holds 2048 user bytes per sector; the runtime rebuilds sync, MSF header and the
                                    8-byte subheader; the 280 EDC/ECC bytes are dropped and read back as zero: the CD controller never exposes them)
                  kind r = raw     (file holds 2352 bytes per sector: movies, XA audio, anything not provably cookable)
  dN/<iso path>   every ISO 9660 file, byte-exact size when cooked ('.raw' suffix when raw)
  dN/_sys/*       sectors outside any file (system area, volume descriptors, path tables, directories, gaps)
Nothing is stored twice: a file identical (SHA-1) to one already stored is not written again, and inside cooked files every
stretch of >= MIN_REF sectors whose data already exists in an earlier file (e.g. FF9.IMG repeats 58-81 % of itself from disc 1 on)
is mapped to that file; only the new sectors go to '<name>.new'. --standalone keeps every disc self-contained (no sharing across discs).
Every cooked sector is checked against the image (sync, header, subheader; data is copied); a sector that does not round-trip makes the
whole file raw. Nothing here is committed: the output is game data (ignored by git)."""
import hashlib, os, re, struct, sys

MIN_REF = 8        # a repeated stretch shorter than this is stored again (not worth a map line)

FRAME = 2352
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'

def bcd(v): return ((v // 10) << 4) | (v % 10)

def head(lba):
    a = lba + 150
    return bytes([bcd(a // 4500), bcd((a // 75) % 60), bcd(a % 75), 2])

def cooked_ok(raw, lba):
    """subheader (8 bytes) if the sector round-trips through 'cook', else None"""
    if raw[:12] != SYNC or raw[12:16] != head(lba): return None
    sh = raw[16:24]
    if sh[:4] != sh[4:] or sh[2] & 0x20: return None            # copies must agree; form 2 is not cookable
    return sh

class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.n = os.path.getsize(path) // FRAME
    def raw(self, lba):
        self.f.seek(lba * FRAME)
        return self.f.read(FRAME)
    def user(self, lba): return self.raw(lba)[24:24 + 2048]

def walk(d, lba, size, path, out):
    data = b''.join(d.user(lba + i) for i in range((size + 2047) // 2048))[:size]
    off = 0
    while off < len(data):
        ln = data[off]
        if ln == 0: off = (off // 2048 + 1) * 2048; continue
        rec = data[off:off + ln]; off += ln
        elba, esize = struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]
        name = rec[33:33 + rec[32]]
        if name in (b'\x00', b'\x01'): continue
        name = name.decode('latin1')
        if rec[25] & 2:
            out.append(('dir', elba, esize, path + '/' + name))
            walk(d, elba, esize, path + '/' + name, out)
        else:
            out.append(('file', elba, esize, (path + '/' + name.split(';')[0]).lstrip('/')))

def unpack(d, root, name, seen, index):
    pvd = d.user(16)
    assert pvd[1:6] == b'CD001', 'no ISO 9660 descriptor'
    items = []
    walk(d, struct.unpack('<I', pvd[158:162])[0], struct.unpack('<I', pvd[166:170])[0], '', items)
    base = os.path.join(root, name)
    os.makedirs(base, exist_ok=True)
    covered = bytearray(d.n)                                   # sectors that belong to a stored file
    maplines = []
    def store(rel, payload, kind, lba, nsec, shs=None):
        h = hashlib.sha1(payload).hexdigest() + kind
        if h in seen: target = seen[h]
        else:
            target = os.path.join(name, rel)
            dst = os.path.join(root, target); os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as o: o.write(payload)
            seen[h] = target
        rp = os.path.relpath(os.path.join(root, target), base)
        if not shs: maplines.append((lba, f"{kind} {lba} {nsec} {rp}"))
        i = 0
        while shs and i < nsec:                                # one extent per run of equal subheaders: c lba n file subheader file_sector
            j = i
            while j < nsec and shs[j] == shs[i]: j += 1
            maplines.append((lba + i, f"c {lba + i} {j - i} {rp} {shs[i].hex()} {i}")); i = j
    def emit(lba, k, rp, sh, fsec): maplines.append((lba, f"c {lba} {k} {rp} {sh.hex()} {fsec}"))
    def store_cooked(path, data, shs, size, lba):
        nsec = len(data)
        hs = [hashlib.md5(x).digest() for x in data]
        ext, i = [], 0                                         # (start, length, target file or None, target sector)
        while i < nsec:
            src = index.get(hs[i])
            j = i + 1
            while src and j < nsec and index.get(hs[j]) == (src[0], src[1] + j - i): j += 1
            if src and j - i >= MIN_REF: ext.append((i, j - i, src[0], src[1])); i = j
            else:
                if ext and ext[-1][2] is None and ext[-1][0] + ext[-1][1] == i: ext[-1] = (ext[-1][0], ext[-1][1] + 1, None, 0)
                else: ext.append((i, 1, None, 0))
                i += 1
        if all(e[2] is None for e in ext):                     # nothing shared: an ordinary exact-size file
            store(path, b''.join(data)[:size], 'c', lba, nsec, shs)
            tgt = seen[hashlib.sha1(b''.join(data)[:size]).hexdigest() + 'c']
            for k in range(nsec): index.setdefault(hs[k], (tgt, k))
            return
        new = b''.join(b''.join(data[e[0]:e[0] + e[1]]) for e in ext if e[2] is None)
        if ext[-1][2] is None: new = new[:len(new) - (nsec * 2048 - size)]      # the file's own short last sector
        rel = os.path.join(name, path + '.new'); dst = os.path.join(root, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as o: o.write(new)
        nstat['shared'] += sum(e[1] for e in ext if e[2] is not None); nstat['new'] += sum(e[1] for e in ext if e[2] is None)
        pos = 0
        for st, ln, tgt, tsec in ext:
            if tgt is None:
                trel = rel
                for k in range(ln): index.setdefault(hs[st + k], (rel, pos + k))
                ftsec, pos = pos, pos + ln
            else: trel, ftsec = tgt, tsec
            rp = os.path.relpath(os.path.join(root, trel), base)
            a = st
            while a < st + ln:                                 # one map line per run of equal subheaders
                b = a
                while b < st + ln and shs[b] == shs[a]: b += 1
                emit(lba + a, b - a, rp, shs[a], ftsec + a - st); a = b
    nraw = ncook = 0
    nstat = {'shared': 0, 'new': 0}
    for kind, lba, size, path in items:
        if kind != 'file' or size == 0: continue
        nsec = (size + 2047) // 2048
        if lba + nsec > d.n: print('skip (past end of image):', path); continue
        raws = [d.raw(lba + i) for i in range(nsec)]
        shs = [cooked_ok(r, lba + i) for i, r in enumerate(raws)]
        pad_ok = raws[-1][24 + size - (nsec - 1) * 2048:24 + 2048] == b'\0' * (2048 - (size - (nsec - 1) * 2048))
        if all(shs) and pad_ok:
            store_cooked(path, [r[24:24 + 2048] for r in raws], shs, size, lba); ncook += 1
        else:
            store(path + '.raw', b''.join(raws), 'r', lba, nsec); nraw += 1
        for i in range(nsec): covered[lba + i] = 1
    # everything else: runs of uncovered sectors, raw
    i = 0; nsys = 0
    while i < d.n:
        if covered[i]: i += 1; continue
        j = i
        while j < d.n and not covered[j]: j += 1
        store(f'_sys/{i:06d}.raw', b''.join(d.raw(k) for k in range(i, j)), 'r', i, j - i); nsys += 1
        i = j
    with open(os.path.join(base, 'disc.map'), 'w') as o:
        o.write(f'# disc_unpack.py map for {name}: kind first_lba sectors file [subheader file_sector]\n')
        for _, l in sorted(maplines): o.write(l + '\n')
    print(f'{name}: sectors in shared stretches {nstat["shared"]}, newly stored {nstat["new"]}'); print(f'{name}: {ncook} cooked files, {nraw} raw files, {nsys} system/gap extents, {d.n} sectors')

def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    args = [a for a in sys.argv[1:] if a != '--standalone']
    standalone = len(args) != len(sys.argv) - 1
    root, seen, index = args[0], {}, {}
    for i, p in enumerate(args[1:], 1):
        if standalone: seen, index = {}, {}
        m = re.search(r'd(\d+)\.\w+$', os.path.basename(p))        # ff9-d3.bin -> d3, so a missing disc does not renumber the others
        unpack(Disc(p), root, 'd' + m.group(1) if m else f'd{i}', seen, index)

if __name__ == '__main__': main()
