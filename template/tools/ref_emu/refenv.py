"""Locations of the local reference emulator pieces (built by tools/ref_emu/setup_ref.sh under work/emu/, git-ignored) and the disc of this instance."""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT)
from tools.recomp import gamecfg, hostenv  # noqa: E402

HOST_DIR = os.environ.get("REF_HOST_DIR", os.path.join(ROOT, "work", "emu", "host"))      # holds the compiled host (host2)
CORE = os.environ.get("REF_CORE", os.path.join(ROOT, "work", "emu", "pcsx_rearmed", "pcsx_rearmed_libretro" + hostenv.shlib_ext()))
HOST = os.path.join(HOST_DIR, "host2")
CUE = gamecfg.CUE
