# 19 - Playing a game without a human: route search with web guides

Method used to reach a save point of a 4-disc RPG (ff9 findings 0012-0015). Everything here is **observed** unless tagged.

## Why
Some bugs only show in game states you cannot reach with a title-screen smoke test (a save screen, a battle, a second field, a movie that starts after a script event). A person would play to get there. An agent can do it with: a **reference emulator that can save states**, a **public walkthrough read as text**, and **picture reading**.

## Pieces (all in the template, `tools/ref_emu/` and `tools/route/`)
| piece | what it gives |
|---|---|
| `setup_ref.sh` | builds the reference core + our headless host from public source on the user's machine (never redistributed) |
| host `--savestate/--loadstate` | resume the reference at any frame: a segment costs seconds, not minutes (a resumed run is pixel-identical to a straight one) |
| `refstep.py` | run a press script from a state, dump pictures, save the end state |
| `refbatch.py` / `refwalk.py` | K random walkers in parallel from one state + one contact sheet; continue from the seed that got somewhere |
| `sheet.py` | PPM -> contact sheet PNG (pure Python); the agent reads the PNG with its image tool |
| `run_game --branch TICK:FILE` | fork OUR build at a tick: many explorer branches from one in-memory state (several `--branch` allowed; children can fork again) |
| `tools/route/ours_session.py`, `owalk.py` | our-build explorer session and walker generator |
| the virtual memory card | the only checkpoint of OUR build that survives a session: once a save point is reached, "title -> Continue -> Load" (about 4,500 ticks) replaces a 100,000-tick route |

## Procedure
1. **Get the text of a guide.** `WebSearch` for "<title> walkthrough disc 1 <first area>"; read it with `WebFetch` and a prompt that asks for *ordered steps with directions, objects to talk to, and what triggers each scene* (the first result may refuse automated fetches: try another site; guides that list "the screen shows X, go up" are best). Ask for one section at a time (a page per area), and for the **name of the next section**.
2. **Get to the first controllable screen in the reference** with a short press script (title -> New Game -> Cross every few hundred frames through the intro; see ff9 0012). Save the state.
3. **Advance one guide step at a time from a state.** For each step: press, dump a picture every 100-200 frames, read the contact sheet, decide the next press, save the new state. Keep a log of the exact presses that worked (this becomes the route file).
4. **When the guide is not specific enough** (it never says how to enter a door), run `refbatch.py` with a walker biased towards the direction the guide suggests (`WALK_SET=up,up,left`); 8 seeds x 5,000 frames find most doors in one or two rounds. Continue from the winning seed's `.st`.
5. **Dialogs:** pressing Cross every 90-120 frames advances them. Menus that need a choice (Yes/No, a Moogle menu) need `down`/`up` taps of 6 frames and a Cross; read the picture before confirming.
6. **Find the save point.** Guides name the first one (a Moogle, a crystal, a save tile). Open the save menu in the reference with the card enabled (`REF_CARD1=1`, host `--savecard`), save, and keep the card image.
7. **Replay on our build.** The reference and our build drift in time (loading time, movie pacing, dialog timing): a press list by frame number can miss by hundreds of frames. Replay the **whole route from the title** on our build with the same list shifted by the measured offset of the boot (ff9: -900 after the opening movie), then look at where it diverges (`ppmbest` of our pictures against the reference's) and fix **only the late part**: fork with `--branch` at the last good tick and try variants (hold lengths, extra taps) in parallel. Dialogs in our build may need Cross where the reference advances by itself, and the reverse.
8. **Make the checkpoint durable:** save on our build, keep the card; later sessions start with a 6-press load script.

## Pitfalls that cost hours (observed)
- The reference ran **without a memory card** until the host enabled it (the libretro core disables cards when the host answers no core options). Always enable it before comparing anything about saving.
- A character can be invisible in the 320x240 half-size sheet (small sprite behind foreground grass); look at one full-size picture before concluding it vanished.
- Facing a character to talk needs a **short direction tap** (3-12 frames) then Cross; a long hold walks past.
- Facing/hold lengths that work in the reference may fail on ours by a few frames: try 6-8 variants at once with `--branch`.
- `rt_pad_script_add`-style parsers that use `strtok` break a caller's own tokenising: tokenise first.
- Every guide step that changes screen is a *transition* (fade, load): do not read the picture during it; read 100-200 frames later.
- Random walking in the reference finds areas, but its press list is only valid from that state: keep the end states, not just the lists.
- **Forked branches must not share the disc file offset**: children of `run_game --branch` inherit the open disc file description; stdio reads in sibling processes then interleave on one offset and corrupt each other (symptom: some branches stay on a black screen at the title overlay, different ones on each run). The runtime reopens the disc files in every child (`rt_hle_reopen_files`); any new open file a model keeps must do the same.
- **Branch spec is `PREFIX:FIRST:STEP`** (first picture tick and the picture interval), not a length: the run length is the main `--ticks`. Writing `p:6000:50000` gives one picture and looks like a dead run.
- A blind random walker stalls at **text-entry screens** (a naming screen): it types into it. Run it in short seeded segments instead, and keep the seeds in the route file; a route is then `base presses + manual segments + walk(start, frames, seed, set)` and is exactly reproducible (the first game's `route_play.py`).
- A route found in the reference starting from a savestate can usually be **replayed on our build from the memory-card checkpoint** by shifting the press ticks so that its first press lands where control starts (about 100 ticks after the load): no re-tuning was needed for 50,000 ticks of rooftops, a name entry, cutscenes and a battle.
- On macOS `sed -i` needs a suffix argument (`sed -i ''`); edit route files with python.
- Treat a guide as **documented**, not observed: the game here is a specific release; names/steps can differ.

## What this replaces and what it does not
It replaces a person for *reaching* states and for *judging that two builds show the same picture*. It does not judge feel, balance or difficulty, and it cannot finish a game that needs puzzle knowledge the guide does not give. Coverage is reported (docs 18), not claimed.

## Replaying a person's session
`play.sh` (from `tools/package.py`) records `logs/session-TS.rec` (pad word per change), `logs/session-TS.log` and a copy of the starting card. Replay: `python3 -I tools/ref_emu/rec2press.py logs/session-TS.rec --ours > session.press`, then `build/ps1_game DISC --card START.mcr --ticks N $(cat session.press)`. Check it against the person's own log before trusting it: the player prints `audio rms` every 600 ticks; the replay's values (headless `ps1_play --no-sync` with `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, or the same line from the runner) must equal them to the digit. A mismatch means the input is off by a tick or the starting state differs; a one-tick press (a Start that skips a movie) is the usual cause. Compare pictures with `PS1_SHOT=TICK:file.ppm`. A person who left the game idle (a session of 795,000 ticks had three pauses of 150,000-310,000) is replayed in a fraction of the time with `rec2press.py ... --ours --squeeze 600`, which caps every stretch with no button held to 600 ticks: it reaches the same places but game time differs from theirs, so the audio check only holds up to the first squeezed pause.

## Testing memory-card flows without a person
The game's own save and load screens can be driven headless with `--press` lists and `--ppm-every PREFIX:FIRST:STEP`, reading the pictures at each step: new block, overwrite (the "Overwrite?" box defaults to No), slot 2 with no card, blank and zeroed images. Check the card image after each step (every 128-byte frame XORs to its last byte; No must leave the image byte-identical) and keep the press lists as routes: they are exact only for the same list, because the screens appear earlier or later with different input. A zeroed (unformatted) image is the case the BIOS layer can get wrong silently: the real game asks to format.
