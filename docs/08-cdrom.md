# 08 — CD-ROM controller

## Register-level model (observed to run an unmodified SDK CD library and streaming code)
- Index/status register, parameter FIFO, response FIFO, interrupt flag/enable (INT1..INT5), request register, 8-bit data FIFO drained by DMA 3.
- Commands modelled: Getstat, Setloc, ReadN/ReadS, Pause, Stop, Init, Mute/Demute, Setfilter, Setmode, Getparam, GetlocL/P, GetTN, GetTD, SeekL/SeekP,
  Test(20h), GetID, ReadTOC. Any other command answers INT5 (`stat|1`, `40h`) and does **not** consume the parameters.
- Interrupts are queued with virtual due times and fired one at a time; a new one waits for the acknowledge (the controller does the same).
- **Data rate**: 75 sectors/s single speed, 150 double speed = 1.25 / 2.5 sectors per 60 Hz frame; sector delivery pauses while an interrupt is
  unacknowledged. Seek/rotation delays are constants (2 ticks); real values depend on distance (not modelled).
- Modes: Setmode bit 0x80 double speed, 0x40 XA-ADPCM on, 0x20 whole 924h-byte sectors, 0x08 XA filter. With bit 0x20 the data FIFO starts after the
  12-byte sync (924h bytes), otherwise after the 24-byte header+subheader (800h bytes).
- Sector layout of a raw 2352-byte MODE2 image: 12 sync, 4 header (BCD mm:ss:ff + mode), 8 subheader (file, channel, submode, coding ×2), data.
  Submode bits: 0x80 EOF, 0x40 real-time, 0x20 Form 2, 0x08 data, 0x04 audio, 0x02 video.
- XA audio sectors (audio + real-time) that pass the file/channel filter are consumed by the XA decoder and **not** delivered as data; filtered-out
  audio sectors are not delivered either. Video and data sectors are delivered.

## Streaming pattern seen in SDK movie code
Read the header sector (ReadN, 1 sector, Pause) → Setfilter (file, channel) → Setloc/SeekL to the stream start → Setmode `0x68` → ReadS. Movie
sectors interleave video and audio (e.g. 3 video : 1 audio). The SDK's stream ring is an array of slots, each a 32-byte header (`0x8001` type, chunk
index/count, frame number, demuxed size, width, height...) + 2016 bytes of payload; consumer polls "next complete frame". The model must deliver
sectors at the real rate or the ring overflows/deadlocks.

## Unknown
Exact response timings per command, GetlocP details (approximated), multi-session/audio-track discs (only one data track modelled).

Origin: f2b#0004, 0011, 0025, 0029.
