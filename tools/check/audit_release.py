#!/usr/bin/env python3
"""Verify built Apple packages contain exactly the approved community delta."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import zipfile

ROOT = Path(__file__).resolve().parents[2]
BLOCKED = {".arc", ".exe", ".ttf", ".sud", ".gdb", ".hvl", ".pem", ".p12", ".p8", ".key", ".mobileprovision"}
PRIVATE = {"agents.md", "implementation_plan.md", ".team", ".env"}


def audit(path, version, build):
    source = ROOT / "app/Resources/AfterstoryCHS"
    expected = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir() if p.is_file()}
    with zipfile.ZipFile(path) as archive:
        entries = {name.split("/AfterstoryCHS/", 1)[1]: name for name in archive.namelist()
                   if "/AfterstoryCHS/" in name and not name.endswith("/")}
        if set(entries) != set(expected):
            raise ValueError(f"{path}: incomplete or unexpected bundled delta: {sorted(entries)}")
        for name, digest in expected.items():
            if hashlib.sha256(archive.read(entries[name])).hexdigest() != digest:
                raise ValueError(f"{path}: bundled {name} differs from approved source")
        for name in archive.namelist():
            if name in entries.values():
                continue
            item = Path(name)
            if item.suffix.lower() in BLOCKED or item.name.lower() in PRIVATE:
                raise ValueError(f"{path}: unexpected private or game resource {name}")
        primary = [name for name in archive.namelist()
                   if name.endswith(("Subahibi.app/Info.plist", "Subahibi.app/Contents/Info.plist"))]
        if len(primary) != 1:
            raise ValueError(f"{path}: missing primary app Info.plist")
        info = plistlib.loads(archive.read(primary[0]))
        if info["CFBundleShortVersionString"] != version or str(info["CFBundleVersion"]) != build:
            raise ValueError(f"{path}: unexpected app version")
    return {"file": path.name, "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "version": version, "build": build, "bundledCommunityFiles": len(expected)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("packages", type=Path, nargs="+")
    args = parser.parse_args()
    print(json.dumps([audit(p, args.version, args.build) for p in args.packages], indent=2))


if __name__ == "__main__":
    main()
