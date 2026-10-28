#!/usr/bin/env python3
"""Turn a disc into a configured instance of this template: finds the image, extracts the ISO9660 files, reads the boot executable from SYSTEM.CNF and writes game/game.json
(+ empty game/hooks.csv, game/roots.csv). Idempotent. Nothing of the disc is committed (og-files/ and work/ are git-ignored).

usage: python3 -I tools/init_game.py PATH [--name NAME] [--title TITLE]
PATH: a .cue or .bin (single data track, MODE2/2352), or an archive (.7z .zip .rar) holding one. Extra discs of a multi-disc title can be added later (docs 08).
Exit code 0 = configured; the last line says what to run next."""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def die(msg):
    sys.exit("init_game: " + msg)


def unpack(path, tmp):
    z = next((c for c in ("7zz", "7z", "7za") if shutil.which(c)), None)
    if not z:
        die("%s is an archive but no 7z tool was found (macOS: brew install sevenzip; Debian/Ubuntu: apt install p7zip-full)" % path)
    subprocess.run([z, "x", "-y", "-o" + tmp, path], check=True, stdout=subprocess.DEVNULL)
    cands = [os.path.join(d, f) for d, _s, fs in os.walk(tmp) for f in fs if f.lower().endswith((".cue", ".bin", ".img", ".iso"))]
    return sorted(cands, key=lambda p: (not p.lower().endswith(".cue"), p))


def find_bin(path):
    if path.lower().endswith(".cue"):
        m = re.search(r'FILE\s+"([^"]+)"', open(path, errors="replace").read())
        if not m:
            die("no FILE entry in " + path)
        return os.path.join(os.path.dirname(path), m.group(1))
    return path


def check_raw(binp):
    with open(binp, "rb") as f:
        head = f.read(12)
        f.seek(16 * 2352 + 24)
        pvd = f.read(8)
    if head != SYNC:
        die("%s is not a raw MODE2/2352 image (no CD sync at sector 0). Cooked 2048-byte .iso files lose the XA audio and video sectors the console games stream; "
            "re-dump or convert to a raw .bin/.cue" % binp)
    if pvd[1:6] != b"CD001":
        die("%s: no ISO9660 volume descriptor at sector 16" % binp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--name")
    ap.add_argument("--title")
    a = ap.parse_args()
    os.makedirs(os.path.join(ROOT, "og-files"), exist_ok=True)
    src = os.path.abspath(a.path)
    if not os.path.exists(src):
        die("not found: " + src)
    tmp = None
    if src.lower().endswith((".7z", ".zip", ".rar")):
        tmp = tempfile.mkdtemp()
        c = unpack(src, tmp)
        if not c:
            die("no .cue/.bin inside " + src)
        src = c[0]
    binp = find_bin(src)
    check_raw(binp)
    base = a.name or re.sub(r"[^a-z0-9]+", "", os.path.splitext(os.path.basename(binp))[0].lower())[:12] or "game"
    dst = os.path.join(ROOT, "og-files", base + "-d1.bin")
    if not os.path.exists(dst):
        shutil.move(binp, dst) if tmp else (os.symlink(os.path.abspath(binp), dst) if not os.path.exists(dst) else None)
    open(os.path.join(ROOT, "og-files", base + "-d1.cue"), "w").write('FILE "%s-d1.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % base)
    disc_dir = os.path.join(ROOT, "og-files", "disc")
    if not os.path.exists(os.path.join(disc_dir, "SYSTEM.CNF")):
        subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "iso_extract.py"), dst, disc_dir], check=True, stdout=subprocess.DEVNULL)
    unp = os.path.join(ROOT, "og-files", "unpacked")              # the disc as loose files + disc.map: every program takes this folder instead of the .bin (docs/findings, tools/disc_unpack.py)
    if not os.path.exists(os.path.join(unp, "d1", "disc.map")):
        subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "disc_unpack.py"), unp, dst], check=True, stdout=subprocess.DEVNULL)
    cnf = open(os.path.join(disc_dir, "SYSTEM.CNF"), errors="replace").read()
    m = re.search(r"BOOT\s*=\s*cdrom:\\?([^;\s]+)", cnf, re.I)
    if not m:
        die("no BOOT= line in SYSTEM.CNF:\n" + cnf)
    boot = m.group(1).replace("\\", "/").split("/")[-1]
    exe = os.path.join(disc_dir, boot)
    if not os.path.exists(exe) or open(exe, "rb").read(8) != b"PS-X EXE":
        die("boot file %s is missing or is not a PS-X EXE" % boot)
    pc, gp, base_addr, size = struct.unpack("<IIII", open(exe, "rb").read(0x20)[0x10:0x20])
    os.makedirs(os.path.join(ROOT, "game"), exist_ok=True)
    cfg = {"name": base, "title": a.title or base, "disc": "og-files/%s-d1.bin" % base, "disc_dir": "og-files/disc",
           "exes": [{"file": boot, "prefix": "main"}], "boot_names": [boot], "overlays": [], "low_ram": []}
    p = os.path.join(ROOT, "game", "game.json")
    if os.path.exists(p):
        old = json.load(open(p))
        if boot not in old.get("boot_names", []) and boot != old.get("exes", [{}])[0].get("file"):
            old = {}                                                          # another game: start from scratch
        cfg.update({k: old[k] for k in ("exes", "boot_names", "overlays", "low_ram") if k in old})        # keep what was learned about this game
    json.dump(cfg, open(p, "w"), indent=2)
    for fn, head in (("hooks.csv", "exe,address,name,param,param2,masked_signature_sha256_prefix\n"), ("roots.csv", "exe,address,evidence\n")):
        fp = os.path.join(ROOT, "game", fn)
        if not os.path.exists(fp):
            open(fp, "w").write(head)
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    print("game:       %s (%s)" % (cfg["name"], cfg["title"]))
    print("boot:       %s  entry %08x  text %08x  size %#x  gp %08x" % (boot, pc, base_addr, size, gp))
    print("configured: game/game.json; disc %s; files in og-files/disc" % cfg["disc"])
    print("next:       python3 -I tools/recomp/build_game.py -O1   then   build/ps1_game %s --ticks 3000 --card /tmp/c.mcr --ppm /tmp/f.ppm" % cfg["disc"])


if __name__ == "__main__":
    main()
