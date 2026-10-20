#!/usr/bin/env python3
"""Build an Apple-only delta from the player's original Chinese patch assets.

Run with a Python environment containing fontTools. Both source folders are
read-only inputs. Output contains copyrighted resources: keep it local.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from compatible_font import build_font

RESOURCES = {"extra", "8_af", "8-1_extra_01-1", "8-1_extra_01-2"}
FONT = "afterstory-font.ttf"
DIRECTORY = "AfterstoryCHS"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def resource_map(folder):
    return {p.name.lower(): p for p in Path(folder).iterdir()
            if p.is_file() and p.suffix.lower() != ".exe"}


def check_delta(base, updated):
    old, new = resource_map(base), resource_map(updated)
    removed = set(old) - set(new)
    changed = {name for name in set(old) & set(new) if sha(old[name]) != sha(new[name])}
    added = set(new) - set(old)
    if removed or changed != {"8_af"} or added != RESOURCES - {"8_af"}:
        raise ValueError(f"unsupported patch difference: removed={sorted(removed)}, "
                         f"changed={sorted(changed)}, added={sorted(added)}")
    return old, new


def replace_extra(source, records):
    data = bytearray(Path(source).read_bytes())
    matches = [record for record in records if Path(record["file"]).resolve() == Path(source).resolve()]
    if len(matches) != 1:
        raise ValueError("missing extra string replacement record")
    record = matches[0]
    if sha(source) != record["source_sha256"]:
        raise ValueError("extra changed after font analysis")
    return data, record


README = """Knockin' on heaven's door 后日谈增量中文字幕补丁（Apple 版）

需要 Subahibi Apple 移植版 0.1.6 或更新版本，以及匹配的原汉化游戏文件。
将整个 AfterstoryCHS 文件夹放进设备游戏目录，与 sysprg.arc、原 BGI-CHS.exe
和原 font.ttf 同级。保留文件夹名称和内部文件名称。不要覆盖原汉化 EXE 或字体。
完全退出应用后重新打开，选择简体中文并开始游戏。启动器会校验全部资源。
游戏按原作进度开放标题界面的 Knockin' on heaven's door 入口，本补丁不解锁章节。

iPhone/iPad/Vision Pro：通过 Finder 文件共享或“文件”App，放进实际游戏目录。
Mac：放进启动器选择的游戏目录。
Apple TV 或局域网：开启应用中的局域网接收，使用项目 tools/push_data.py：
python3 tools/push_data.py --host 设备IP --code 配对码 --patch-only 解压后的父目录
--patch-only 会重传小包全部文件，防止同大小旧文件被误当成已传完。

卸载补丁：退出应用，只移除 AfterstoryCHS 文件夹，然后重新启动。
保留本篇封包、汉化 EXE、原 font.ttf 和存档；后日谈存档应在补丁启用时读取。
本包仅包含后日谈的四个差异资源与一份保留旧字形的派生字体，不含游戏本体。
汉化内容及字体来自用户提供的后日谈补丁，署名保留于原剧情，未重新翻译。
仅供已合法取得原游戏和相应补丁的用户本地使用，请勿随开源应用上传或再分发。
"""


def build(game, epilogue, output):
    game, epilogue, output = map(lambda p: Path(p).resolve(), (game, epilogue, output))
    if output.exists():
        raise ValueError("output already exists; choose a new output folder")
    sources = [game / "BGI-CHS.exe", game / "font.ttf", epilogue / "BGI-CHS.exe"]
    fonts = list(epilogue.rglob("font.ttf"))
    if len(fonts) != 1:
        raise ValueError("epilogue folder must contain exactly one font.ttf")
    sources.append(fonts[0])
    before = {str(p): sha(p) for p in sources}
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="subahibi-afterstory-") as temp:
        temp = Path(temp)
        reader = temp / "evb-extract"
        subprocess.run(["cc", "-O2", "-I", str(root / "app/Sources/Native"),
                        str(root / "tools/check/evbtest.c"), str(root / "app/Sources/Native/evb.c"),
                        "-o", str(reader)], check=True)
        base, new = temp / "base", temp / "new"
        for exe, target in [(sources[0], base), (sources[2], new)]:
            subprocess.run([str(reader), str(exe), str(target)], check=True, capture_output=True)
        old, updated = check_delta(base, new)
        staging = temp / DIRECTORY
        staging.mkdir()
        report = build_font(sources[1], sources[3], [base, new], staging / FONT)
        extra, record = replace_extra(updated["extra"], report["extra_replacements"])
        replacement = bytes.fromhex(report["shadow_code"]["gbk_hex"])
        for offset in record["offsets"]:
            if extra[offset:offset + 2] != b"\x81\x4c":
                raise ValueError("extra replacement is not at the expected string bytes")
            extra[offset:offset + 2] = replacement
        if len(record["offsets"]) != 3:
            raise ValueError("expected exactly three conflicting glyph codes in extra")
        (staging / "extra").write_bytes(extra)
        for name in sorted(RESOURCES - {"extra"}):
            shutil.copyfile(updated[name], staging / name)
        manifest = {"format": "subahibi-afterstory-chs", "version": 1, "minimumAppVersion": "0.1.6",
                    "baseFontSHA256": sha(sources[1]), "baseScriptSHA256": sha(old["8_af"]),
                    "files": {name: sha(staging / name) for name in sorted(RESOURCES | {FONT})},
                    "sourceExecutableSHA256": sha(sources[2]),
                    "sourceFontSHA256": sha(sources[3]),
                    "fontCompatibility": {k: report[k] for k in ["shadow_code", "validation"]}}
        (staging / "patch.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (staging / "Readme.txt").write_text(README, encoding="utf-8")
        if any(sha(p) != digest for p, digest in before.items()):
            raise ValueError("an input file changed while building")
        output.mkdir(parents=True)
        shutil.copytree(staging, output / DIRECTORY)
        archive = output / "Subahibi-AfterstoryCHS-Apple-v1.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in sorted((output / DIRECTORY).iterdir()):
                z.write(p, f"{DIRECTORY}/{p.name}")
        print(json.dumps({"zip": str(archive), "bytes": archive.stat().st_size, "sha256": sha(archive),
                          "resources": len(RESOURCES), "inputFilesUnchanged": True,
                          "fontValidation": report["validation"]}, ensure_ascii=False, indent=2))
        return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, help="original Chinese game folder")
    parser.add_argument("--epilogue", required=True, help="supplied 后日谈补丁 folder")
    parser.add_argument("--output", required=True, help="new local output folder; never publish its contents")
    args = parser.parse_args()
    build(args.game, args.epilogue, args.output)


if __name__ == "__main__":
    main()
