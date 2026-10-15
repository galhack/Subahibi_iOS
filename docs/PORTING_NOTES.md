# Porting notes

Technical notes for anyone working on this port or bringing another BGI /
Ethornell game to Apple platforms with it.

## The game

- 素晴らしき日々 ～不連続存在～ HD: product name `SubarashikihibiHD`, BGI
  **1.647** ("Compatibility 1.72"), 1280 x 960.
- Images: 5697 CompressedBG pictures, 5650 of them **version 2** (the
  JPEG-like coding, `engine/openbgi/src/core/cbg2.c`).
- Sound: BW containers with Ogg Vorbis.  Movies: five MPEG-1 program
  streams (1280 x 960, 25 fps, MP2 audio) stored inside `data021xx.arc` /
  `data029xx.arc`; 1.647 reads them straight from the archives.

## What the engine needed (engine/openbgi)

| Change | Where |
|---|---|
| Generation `GEN_1_647`: the opcode set of the 1.647 executable is the 1.640 set plus `55`, `80 9F`, `81 48`, `91 00`, `91 09`, `92 99`; boot areas of 1.653 (8 MB / 4 MB) | `inc/bgi/version.h`, `src/vm/opset.c`, `src/core/version.c` |
| `90 02` fps 0 = present every pass; `91 9A` accepts up to `0x80000005` | `src/vm/ops_gfx0.c`, `src/sys/present.c`, `src/gfx/text/text.c` |
| `91 0A` (two renderer switches), `56` fixmul, `91 DB` (knob being dragged), `B0 06` (pointer suppressed by touch) | `src/vm/ops_*.c` |
| CompressedBG v2 decoder, reads bounded by the file size | `src/core/cbg2.c` |
| Movies from archive ranges (`OS_MovieOpenRange`) | `src/gfx/movie.c` |
| `Engine_ParseOptions` overwrote the terminator of a command line ending in an option | `src/sys/engine.c` |

The opcode tables were recovered from the original executable with
`tools/re/optables.py` (each family dispatcher indexes a static table; an
empty slot is an undefined instruction).  `tools/re/used_missing.py` lists
the opcodes a game's programs use that the engine does not implement yet.

## The Chinese patch

`BGI-CHS.exe` is the unchanged 1.647 executable with an Enigma Virtual Box
file system appended (the translated scenarios, pictures and three system
programs).  The text is GBK, kept on Shift-JIS character boundaries: common
hanzi whose GBK lead byte falls in 0xA1..0xDF are written as rare GBK
characters with an SJIS-compatible lead byte, which the patch's `font.ttf`
draws as the intended glyphs.  The Apple back end decodes GBK, looks the
glyph up in that font first and falls back to PingFang SC.
`app/Sources/Native/evb.c` unpacks the file system (aPLib chunks) on the
device.

## The Apple back end (engine/openbgi/src/os/apple)

- The engine runs its own message loop on a 16 MB thread; the application
  queues input and uploads the "window image" to a Metal texture when its
  serial changes.
- Paths below `C:\` are redirected (`gPosixPathRedirect`): `UserData\` and
  `BGI.gdb` to a save directory, the patch's loose files over the game's.
- Audio: AudioToolbox output unit pulling the engine's mixer; the mixer is
  not pulled before `AudioOutputUnitStart` returns (on a Mac the start
  waits for a render cycle while the engine holds the sound lock).
- tvOS lets an application write only to Caches and tmp.

## Testing

- `engine/openbgi`: `make PLATFORM=apple CC=clang VORBIS=stb bin/openbgi-hosttest`
  runs the real back end on macOS with a scripted pointer and writes
  pictures; `--movie=ARC:NAME` exercises the movie player alone.
- `tools/sim/` runs the engine headless through the WebAssembly back end.
- `tools/check/cbgcheck.c` decodes every picture of the archives;
  `cbgtrunc.c` fuzzes the v2 decoder under ASan; `evbtest.c` compares the
  patch unpacker with evbunpack.
- On devices: `SUBAHIBI_AUTOSTART=ja|zh` and `SUBAHIBI_SCRIPT="..."` (see
  `app/Sources/App/DebugScript.swift`).  Script input bypasses the gesture
  recognizers: test real touches as well.
- The title menu exists only about 28 s after boot (after the five intro
  clicks of the scripts); a click before that does nothing.
