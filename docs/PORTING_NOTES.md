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

### 0.1.1 image-load crash

Two iPhone crash reports on 2026-10-20 had the same stack:
`memmove → LoadFile → BmpOp_LoadFile → Opcode_Gfx0_BmpLoad`.
The decoded copy was 46,687,504 bytes, matching `sp0011a`
(11,766 × 992 × 4 + 16); the destination was only 32 MiB.
Ten images in the HD archives exceed that old allocation.

`GetFileLoadCapacity` probes the stored length and compression header.
Bitmap and wait loaders allocate the larger of stored and decoded sizes;
cache lookup accepts a null destination to query its size before copying.
The synthetic `test_imageload` covers uncached and cached synchronous/wait
loads. With `BGI_GAME_DIR`, it also loads all ten large compressed images.
Those ten images passed AddressSanitizer and UndefinedBehaviorSanitizer.
The test fixtures and crash reports remain local; no game data is published.
The owner confirmed on iPhone 16 Pro Max that the subway animation shortly
after save slot 2 now advances without the crash. All 12 engine test
executables passed, and iOS, tvOS, visionOS and Mac Catalyst packages built.
The other physical devices were unavailable for this revision's testing.

### Other checks

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
- Apple builds enable `--skip-maker-logo` from 0.1.3. The optional prelude
  lookup returns absent, so the original title script enters its normal menu
  without introductory pictures. Startup tests must wait for menu readiness.

### 0.1.3 global progress and startup

`GdbPath` returned an unqualified `BGI.gdb` when no alternate directory was
set. Reads therefore missed Apple's drive-rooted save redirection, while
`WriteTextFile` qualified writes with `gBaseDir`. Both operations now use the
same qualified path. `test_database` puts conflicting original and player
records in separate locations and checks chapter flags, system state, names
and read flags after reload. `test_startup` checks that only the exact optional
MakerLogo lookup is hidden and that direct asset access remains available.

The original Kimika ending records completion flag 214; the main ending sets
chapter flag 232. These conditions are unchanged. Chapter selection uses
transparent normal images and visible hover images; private fixture testing
confirmed highlighting and entry into a chapter. Lost flags from an already
overwritten database cannot be reconstructed from the chapter-local manual
saves. No synthetic unlock flags are installed on user devices.
