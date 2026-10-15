# Third-party components

| Component | Where | License |
|---|---|---|
| OpenBGI (Cytlan and contributors), upstream commit in `engine/UPSTREAM_COMMIT` | `engine/openbgi/` | GPL-2.0-only |
| pl_mpeg (Dominic Szablewski) | `engine/openbgi/src/os/apple/third_party/pl_mpeg.h` | MIT |
| stb_vorbis (Sean Barrett) | `engine/openbgi/src/snd/stb_vorbis.c` | Public domain / MIT |
| CompressedBG v2 algorithm, after GARbro's `ImageCBG.cs` (morkt) | `engine/openbgi/src/core/cbg2.c` | MIT (original); the C port is GPL-2.0 with this project |

The Enigma Virtual Box reader (`app/Sources/Native/evb.c`) follows the file
layout documented by the evbunpack tool; its code and aPLib decoder are
written for this project.

No part of the game "素晴らしき日々 ～不連続存在～" (KeroQ / ケロQ) or of its
Chinese translation patch is included in this repository.
