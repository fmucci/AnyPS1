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
