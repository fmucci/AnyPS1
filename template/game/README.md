# game/

Everything that belongs to one title and is NOT game data lives here: `game.json` (names, executables, boot names, extra low-RAM ranges with their evidence), `hooks.csv` (SDK functions found by masked signature, with their addresses in this title's executables) and `roots.csv` (function entries discovery cannot see, with evidence). `tools/init_game.py` creates the files; the agent grows them as the bring-up proceeds. The two CSV files contain addresses of the title, so they are committed only in the title's own repository, never in AnyPS1.
