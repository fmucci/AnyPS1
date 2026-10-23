import sys, os, struct  # usage: iso_extract.py <MODE2/2352 .bin> <out dir>
bin_path, out = sys.argv[1], sys.argv[2]
f = open(bin_path, 'rb')
def sector(n, form2=False):
    f.seek(n*2352)
    raw = f.read(2352)
    return raw[24:24+2048]
pvd = sector(16)
assert pvd[1:6] == b'CD001', pvd[:8]
root = pvd[156:190]
def walk(lba, size, path):
    data = b''.join(sector(lba+i) for i in range((size+2047)//2048))[:size]
    off = 0
    while off < len(data):
        l = data[off]
        if l == 0:
            off = (off//2048+1)*2048
            continue
        rec = data[off:off+l]; off += l
        elba = struct.unpack('<I', rec[2:6])[0]
        esize = struct.unpack('<I', rec[10:14])[0]
        flags = rec[25]
        name = rec[33:33+rec[32]]
        if name in (b'\x00', b'\x01'): continue
        name = name.decode('latin1')
        p = os.path.join(path, name)
        if flags & 2:
            os.makedirs(os.path.join(out, p), exist_ok=True)
            walk(elba, esize, p)
        else:
            fn = name.split(';')[0]
            dst = os.path.join(out, path, fn)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            # files may be Mode2 Form2 (XA); copy raw 2048 user bytes
            with open(dst, 'wb') as o:
                rem = esize; s = elba
                while rem > 0:
                    o.write(sector(s)[:min(2048, rem)]); rem -= 2048; s += 1
            print(f"{esize:>12} {os.path.join(path, fn)}")
walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '')
