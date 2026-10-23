# ps1_play — native player (SDL2)

Build: `brew install sdl2` (macOS) or your distribution's SDL2 development package, then `python3 -I tools/package.py native` (or `python3 -I tools/recomp/build_game.py -O1 --frontend` → `build/ps1_play`). Windows: `python3 -I tools/package.py windows`.

Run: `build/ps1_play "og-files/<name>-d1.bin" [--card <name>.mcr] [--scale 2] [--fullscreen] [--no-sync]`

| input | key / pad |
|-------|-----------|
| d-pad | arrow keys / d-pad / left stick |
| ✕ ○ □ △ | Z X A S / A B X Y |
| L1 R1 L2 R2 | Q W E R / shoulders, triggers |
| Start / Select | Enter / Right Shift or Backspace; Start / Back |
| fullscreen / quit | F11 / Esc |

Behaviour: the window keeps a 4:3 picture, video is the GPU display area with the vertical display range applied; audio is the SPU model's 44.1 kHz stereo output queued to the device; the loop is paced to 59.94 Hz of wall-clock time (`--no-sync` runs flat out); the memory card is the file given by `--card` (default `<name>.mcr` in the current directory), saved on quit. The game data is read from the disc image you supply; nothing of it is distributed.

Headless check (no display or sound card): `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/ps1_play DISC --ticks 400 --no-sync`.

Pacing: the loop waits for the 59.94 Hz moment of each frame, then presents, then samples the input of the next frame (finding 0044). `PS1_PACELOG=1` prints the wall-clock interval between pictures.

Recording and replay: `ps1_play DISC --card FRESH.mcr --record FILE` writes one line `tick hexword` for every change of the pad word (virtual frame and the BIOS pad word, active low). `run_game DISC --replay FILE` (headless) and the reference-emulator host (`tools/ref_emu/host --replay FILE`) apply it again; with a fresh memory card the replay is deterministic (identical frames on every run), which makes a reported situation reproducible.
