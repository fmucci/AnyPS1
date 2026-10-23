"""The one place where a repository instance says which game it is. Reads game/game.json (written by tools/init_game.py, edited by hand when a title has several executables).

  {"name": "mygame",                       short id: output names (build/<name>_game is NOT used, see below), package names, default card file
   "title": "My Game",                     window title of the player
   "disc": "og-files/disc1.bin",           the disc image (MODE2/2352 .bin, cue next to it), relative to the repository root, never committed
   "disc_dir": "og-files/disc",            ISO9660 files extracted from it (tools/iso_extract.py), never committed
   "exes": [{"file": "SLUS_000.00", "prefix": "main"}],     executables translated ahead of time (prefix = C symbol prefix)
   "boot_names": ["SLUS_000.00"],          boot file names to try on the disc, in order (multi-disc titles name it differently per disc)
   "overlays": [],                         [{"name": "X", "exr": "X.EXR", "base": "0x80100000", "host_exe": "MAIN.EXE"}] relocatable modules (optional)
   "low_ram": [[lo, hi, "why"]]            extra kernel-area ranges the program is allowed to read (zeros), each with its evidence (optional)}
Hook / root tables of this game: game/hooks.csv, game/roots.csv (same format as the templates in game/)."""
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GAME_DIR = os.path.join(ROOT, "game")
_cache = {}


def _load():
    if not _cache:
        path = os.path.join(GAME_DIR, "game.json")
        if not os.path.exists(path):
            raise SystemExit("game/game.json is missing: run `python3 -I tools/init_game.py DISC.bin` first (see PLAYBOOK.md)")
        cfg = json.load(open(path))
        _cache.update(
            CFG=cfg, NAME=cfg["name"], TITLE=cfg.get("title", cfg["name"]),
            DISC=os.path.join(ROOT, cfg["disc"]), CUE=os.path.splitext(os.path.join(ROOT, cfg["disc"]))[0] + ".cue",
            DISC_DIR=os.path.join(ROOT, cfg.get("disc_dir", "og-files/disc")),
            EXES=[(e["file"], e["prefix"]) for e in cfg["exes"]],
            BOOT_NAMES=cfg.get("boot_names") or [cfg["exes"][0]["file"]],
            OVERLAYS=cfg.get("overlays", []), LOW_RAM=cfg.get("low_ram", []), MAIN_EXE=cfg["exes"][0]["file"])
    return _cache


HOOKS_CSV = os.path.join(GAME_DIR, "hooks.csv")
ROOTS_CSV = os.path.join(GAME_DIR, "roots.csv")


def __getattr__(name):         # the game file is read on first use, so the game-independent tests run without one
    c = _load()
    if name in c:
        return c[name]
    raise AttributeError(name)
