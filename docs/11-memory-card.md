# 11 — Memory card and the BIOS file layer

- A virtual card is a 128 KiB file in the standard layout: frame 0 header (`"MC"`, XOR checksum), 15 directory frames (states `51h/52h/53h` first/middle/
  last of a file, `A0h..A3h` free/deleted variants, next-block pointer, name, checksum), broken-sector list and write-test frame, 128-byte frames,
  8 KiB blocks. Formatted on first use.
- BIOS calls implemented on it: card init/start/stop, `_card_info`, sector read/write (128-byte sectors), `FileOpen` (existing / create with N blocks /
  async flag), seek/read/write/close, format, first/next file with `*` and `?` wildcards (direntry layout from psx-spx), delete, last-error.
  `fd 1` stays the console capture.
- Completion is delivered as events on the next frame (see 04): classes `F4000001/4` for file operations, `F0000011/4` for sector operations; error
  specs `0x100` no card, `0x8000` error, `0x2000` unformatted.
- With an empty, never-swapped card the games' card checks, directory scan and "no save" paths run; card-changed flags, broken-sector replacement,
  exact error numbers and hot-swap are not modelled. Programs may open the card file at start-up even when the player never saves.

Origin: f2b#0012, 0023.

## Second title: a real save and load through the BIOS card layer (observed, FF9)
A full save (Moogle menu in the field) and a load (title screen, Continue) now run on the model, and the 128 KiB images were exchanged with an independent emulator's card in both directions (each accepts the other's file; same directory entry, 171 of 8192 data bytes differ = play time and per-run values). Four details were wrong or missing and the title only worked once all four were right; each was found by logging the BIOS calls of both runs and reading the reference BIOS source:
1. **`A(ABh)` _card_info delivers the high-level (SwCARD, `F4000001`) event**, spec 4 ok / `0x100` no card / `0x2000` new card, for the card of port `(a0>>4)&1`. A title that polls only the `F4000001` events (all of its card code did) waits for ever if the answer is the low-level `F0000011` one: the symptom was a black screen after the Moogle's "Let's save" because the save overlay never left its probe state. `B(4Dh)` (low-level info) keeps the HwCARD event. Slot 2 had no card.
2. **`A(ACh)` _card_load(_dir)** ends with the SwCARD event and then the HwCARD one, **3 frames after the call** (one 128-byte sector per frame; async file reads/writes: one frame per sector plus one). The game calls it a second time when the answer has not come after the same 3 frames, so an event that comes at the next frame desynchronises its state machine; model a per-event delay in the pending-event queue (delay frames per entry), not only "next frame".
3. **An async `FileRead` (FileOpen flag `0x8000`) does not advance the file position.** The reference BIOS has this as a special case marked "?? ff9"; the program reads the 128-byte header, seeks `+0x80` with `SEEK_CUR`, reads the next frame. With the position advanced the second read returned the third frame and the load screen said "data corrupted". Writes do advance it.
4. **The program walks the BIOS device control block table itself**: `[0x150]` = table pointer (0x6EE0 in the reference), `[0x154]` = size in bytes, 80-byte entries, entry 0 = name pointer to the string `"bu"`. A libcard-style wrapper copies the device name of `"bu00:..."` up to the first character below `;` (so `"bu"`), looks for it in the table and only then calls `B(42h)` firstfile (and patches a function slot of that entry); with an empty table it returns "no files" without calling the BIOS ("no save data" although the card holds one). A HLE BIOS must seed at least entry 0 (the other fields are never read by an HLE firstfile/read/write).
Method note (see 00): to compare two BIOS implementations on the same program, log every A/B call with arguments from the frame of the user's key press in both and diff the sequences; the first call that exists in one and not the other points at the wrong branch. The reference host needs its memory card enabled (libretro core option `pcsx_rearmed_memcard1=libretro`) or the core runs with no card at all.

Origin: ff9#0015.

## A game's own save may go to RAM first; the card is written somewhere else (observed, first title)
The first title's pause-menu SAVE ends with the text "SAVING TO TEMPORARY MEMORY.. TO TRANSFER TO MEMORY CARD, QUIT TO MAIN MENU OR REFER TO MANUAL.": the card is not touched until the player quits to the main menu and uses RESUME -> SAVE there. So "the card file never changes after saving" is not by itself a card-layer bug. Before blaming the layer: grep the executables' strings for `temporary`, `transfer`, `backup`, `memory card`; follow the route the game describes; run with a card-call log (`PS1_CARDLOG=1`, every BIOS card call and the layer's answer; `PS1_BIOSLOG=1` for all BIOS calls; `PS1_EVLOG=1` shows each event delivery and its callback). A confirm box can be small and dark ("QUIT GAME ? NO"): look at full-size frames, not thumbnails, and remember a second confirm press picks the default.
In that title the card calls of the main-menu SAVE were `_card_info(0)` -> ok, `_card_load(0)` -> ok, callbacks ran and left their result variables at 0, and the game still printed "MEMORY CARD FAILURE" without any further BIOS call: the decision is in the program's own state, not in a missing BIOS answer (cause open, see the first port's finding 0047).

