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

## Community afterstory translation

致敬并感谢后日谈社区汉化原作者 **笑笑竺斋**。
原汉化包出处：[素晴日后日谈笑笑汉化补丁](https://www.bilibili.com/video/BV19m4y1m7Us)。

`app/Resources/AfterstoryCHS/` contains the community Chinese delta for
Knockin' on heaven's door: four afterstory resources and a compatibility font,
plus its manifest and attribution. The original translation and in-game
credits are retained. This project supplies the Apple packaging, resource
loader and font compatibility adaptation; it does not claim authorship of
the translation. These resources retain their original rights and are not
relicensed by the engine's GPL-2.0-only notice.

The full game, original executables and archives, base-story Chinese patch,
music, movies and player saves are not included. Game rights remain with
KeroQ / ケロQ; players supply their own purchased game and base patch.
