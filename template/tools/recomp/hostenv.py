"""Host-platform details shared by the build scripts and tests (macOS, Linux, Windows).
  CC           C compiler: env PS1_CC, then CC, then `cc` (on Windows `gcc` from MinGW-w64 or `clang` when `cc` is absent)
  target()     darwin, linux or windows (PS1_TARGET overrides the host, for cross builds)
  exe(name)    executable file name for the platform ("ps1_game" -> "ps1_game.exe" on Windows)
  shlib_ext()  shared library extension (.dylib, .so, .dll)
  extra_flags() extra compiler/linker flags from env PS1_EXTRA_CFLAGS (e.g. -static-libgcc for redistributable Windows builds)
  sdl_flags()  compiler/linker flags of SDL2: sdl2-config, else pkg-config, else env PS1_SDL_FLAGS; None when SDL2 is not found"""
import os
import shutil
import subprocess
import sys


def _compiler():
    for name in (os.environ.get("PS1_CC"), os.environ.get("CC"), "cc", "gcc", "clang"):
        if name and shutil.which(name.split()[0]):
            return name
    return "cc"


CC = _compiler()


def target():
    """Platform the binaries are built FOR: darwin, linux or windows. PS1_TARGET=windows selects a MinGW-w64 cross build (the compiler is then PS1_CC)."""
    t = os.environ.get("PS1_TARGET")
    if t:
        return t
    return {"darwin": "darwin", "win32": "windows", "cygwin": "windows", "msys": "windows"}.get(sys.platform, "linux")


def exe(name):
    return name + (".exe" if target() == "windows" else "")


def shlib_ext():
    return {"darwin": ".dylib", "windows": ".dll"}.get(target(), ".so")


def sdl_flags():
    if os.environ.get("PS1_SDL_FLAGS"):
        return os.environ["PS1_SDL_FLAGS"].split()
    for cmd in (["sdl2-config", "--cflags", "--libs"], ["pkg-config", "--cflags", "--libs", "sdl2"]):
        if shutil.which(cmd[0]):
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode == 0:
                return r.stdout.split()
    return None


def extra_flags():
    return os.environ.get("PS1_EXTRA_CFLAGS", "").split()
