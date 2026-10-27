#!/usr/bin/env python3
"""Builds a player package for one platform from the CURRENT checkout (queue P5-4/P5-5): ps1_play + ps1_game, the SDL2 runtime, a launcher script and a README.
   python3 -I tools/package.py windows     cross build with MinGW-w64 in the Docker image (docker/windows), output dist/@NAME@-windows-x64/ and .zip
   python3 -I tools/package.py linux       build in the Docker image (docker/linux), output dist/@NAME@-linux-<arch>/ (needs libsdl2-2.0-0 on the target)
   python3 -I tools/package.py native      build with the host compiler, output dist/@NAME@-<os>-<arch>/
   add `--with-disc PATH` to also include your own disc image in the zip (a single file to move to another machine; never publish it)
The package contains **code recompiled from the game's executables** (generated from the disc image in og-files/ on THIS machine). It is for the owner's own
testing: it is git-ignored (dist/) and must not be published or shared. The disc image itself is never included: the player takes the path of the user's own .bin."""
import os
import platform
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))
import hostenv  # noqa: E402
sys.path.insert(0, ROOT)
from tools.recomp import gamecfg  # noqa: E402
NAME, TITLE, DISCNAME = gamecfg.NAME, gamecfg.TITLE, os.path.basename(gamecfg.DISC)

README = """@NAME@ - native port of @TITLE@ (test build)

You need your own disc image of the game (the .bin file of a single-track .bin/.cue).  Nothing of the game is included here except program code
recompiled from it; do not share this folder.

Run:   play.bat      (uses "@DISC@" from this folder when present; otherwise pass the path: play.bat "C:\\path\\to\\@DISC@", or drag the .bin onto play.bat)
Keys:  arrows = d-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L1/R1, E/R = L2/R2, Enter = start, Right Shift = select,
       F11 = fullscreen, Esc = quit, hold Tab = fast forward, F12 = screenshot (logs/).  Game controllers work too.
Saves: @NAME@.mcr in this folder (a virtual memory card); written when you quit.
Options (ps1_play.exe): --scale N, --fullscreen, --no-sync (run unthrottled), --card FILE
Report: whether it starts, picture, sound, controls, speed, and any text printed in the console window.
"""

PLAY_BAT = """@echo off
cd /d "%~dp0"
if not "%~1"=="" (
  set DISC=%~1
) else if exist "@DISC@" (
  set DISC=@DISC@
) else (
  set /p DISC=Path to the game's .bin file: 
)
ps1_play.exe "%DISC%"
echo.
pause
"""


def windows():
    out = os.path.join(ROOT, "build", "win", "pkg")
    shutil.rmtree(out, ignore_errors=True)
    inner = ("export PS1_TARGET=windows PS1_CC=x86_64-w64-mingw32-gcc PS1_EXTRA_CFLAGS=-static-libgcc; "
             "S=/opt/SDL2/x86_64-w64-mingw32; "
             "[ -d /opt/SDL2 ] || { curl -sL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz | tar xz -C /opt && mv /opt/SDL2-2.30.9 /opt/SDL2; }; "
             "export PS1_SDL_FLAGS=\"-I$S/include/SDL2 -L$S/lib -lmingw32 -lSDL2main -lSDL2\"; "
             "python3 -I tools/recomp/build_game.py -O2 --frontend && mkdir -p /out/pkg && cp build/ps1_play.exe build/ps1_game.exe $S/bin/SDL2.dll /out/pkg/")
    subprocess.run([os.path.join(ROOT, "docker/windows/run.sh"), "bash", "-c", inner], check=True)
    return out, NAME + "-windows-x64"


def linux():
    """Linux package built in the Docker image (docker/linux). PS1_DOCKER_PLATFORM=linux/amd64 gives an x86-64 build (default: the Docker host architecture)."""
    out = os.path.join(ROOT, "build", "linux-out", "pkg")
    shutil.rmtree(out, ignore_errors=True)
    inner = "python3 -I tools/recomp/build_game.py -O2 --frontend && mkdir -p /out/pkg && cp build/ps1_play build/ps1_game /out/pkg/ && uname -m > /out/pkg/ARCH"
    subprocess.run([os.path.join(ROOT, "docker/linux/run.sh"), "bash", "-c", inner], check=True)
    arch = open(os.path.join(out, "ARCH")).read().strip()
    os.remove(os.path.join(out, "ARCH"))
    return out, NAME + "-linux-" + arch


def native():
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools/recomp/build_game.py"), "-O2", "--frontend"], check=True)
    out = os.path.join(ROOT, "build", "pkg")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for n in ("ps1_play", "ps1_game"):
        shutil.copy(os.path.join(ROOT, "build", hostenv.exe(n)), out)
    return out, NAME + "-%s-%s" % (hostenv.target(), platform.machine().lower())


def fill(t):
    """Names of this game in the launcher and README texts."""
    return t.replace("@DISC@", DISCNAME).replace("@NAME@", NAME).replace("@TITLE@", TITLE)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    disc = None
    if "--with-disc" in sys.argv:                              # --with-disc PATH: also put the owner's own disc image in the zip (one file to move to another machine)
        disc = sys.argv[sys.argv.index("--with-disc") + 1]
        args = [a for a in args if a != disc]
    what = args[0] if args else "native"
    src, name = windows() if what == "windows" else linux() if what == "linux" else native()
    dist = os.path.join(ROOT, "dist", name)
    keep = {"logs"}                                            # the player's own files (sessions, saves, the disc link) survive a rebuild
    if os.path.isdir(dist):
        for fn in os.listdir(dist):
            if fn in keep or fn.endswith((".mcr", ".bin")): continue
            fp = os.path.join(dist, fn); shutil.rmtree(fp) if os.path.isdir(fp) and not os.path.islink(fp) else os.remove(fp)
    shutil.copytree(src, dist, dirs_exist_ok=True)
    open(os.path.join(dist, "README.txt"), "w", newline="\r\n" if what == "windows" else "\n").write(fill(README if what == "windows" else README.replace("play.bat      (uses \"@DISC@\" from this folder when present; otherwise pass the path: play.bat \"C:\\path\\to\\@DISC@\", or drag the .bin onto play.bat)", "./ps1_play \"/path/to/@DISC@\"   (Linux needs the SDL2 runtime: libsdl2-2.0-0)")))
    if what == "windows":
        open(os.path.join(dist, "play.bat"), "w", newline="\r\n").write(fill(PLAY_BAT))
    else:
        sh = os.path.join(dist, "play.sh")
        open(sh, "w").write('#!/bin/sh\ncd "$(dirname "$0")"\nDISC="${1:-' + DISCNAME + '}"\nmkdir -p logs\nTS=$(date +%Y%m%d-%H%M%S)\n# the card the session starts with (written by the player only when the window is closed): needed to replay the session later\nfor C in *.mcr; do [ -f "$C" ] && cp "$C" "logs/session-$TS.start-$C"; done\n# every session: logs/session-TS.rec = the pad input (see rec2press.py), logs/session-TS.log = the console output\n./ps1_play "$DISC" --record "logs/session-$TS.rec" 2>&1 | tee "logs/session-$TS.log"\n')
        shutil.copy(os.path.join(ROOT, "tools", "ref_emu", "rec2press.py"), os.path.join(dist, "rec2press.py"))
        os.chmod(sh, 0o755)
    zpath = dist + ("-with-disc" if disc else "") + ".zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for fn in sorted(os.listdir(dist)):
            if fn == "logs" or fn.endswith((".mcr", ".bin")): continue          # the player's own files and the disc never go into the zip (the disc only with --with-disc)
            z.write(os.path.join(dist, fn), os.path.join(name, fn))
        if disc:                                               # the disc is mostly compressed video: store it as is
            z.write(disc, os.path.join(name, DISCNAME), compress_type=zipfile.ZIP_STORED)
    print("package:", dist)
    print("zip:    ", zpath, "(%.1f MB)" % (os.path.getsize(zpath) / 1e6))


if __name__ == "__main__":
    main()
