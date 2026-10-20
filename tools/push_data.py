#!/usr/bin/env python3
"""Send your game folder to the app over the local network.

On the device, open the app, switch on "局域网接收游戏文件" and note the
address and the pairing code it shows; then on the Mac:

    python3 tools/push_data.py --host 192.168.1.23 --code 123456 /path/to/game

Only the files the port uses are sent: the BURIKO archives (*.arc), the
Chinese patch (BGI-CHS.exe) and its font (font.ttf), and BGI.gdb / BGI.hvl.
Files the device already has with the same size are skipped, so an
interrupted transfer can simply be started again.  Needs only the Python
standard library.
"""
import argparse
import http.client
import json
import os
import sys
import time
import urllib.parse

PORT = 8765
WANTED_EXACT = {"bgi-chs.exe", "font.ttf", "bgi.gdb", "bgi.hvl"}
PATCH_DIR = "AfterstoryCHS"
PATCH_FILES = {"extra", "8_af", "8-1_extra_01-1", "8-1_extra_01-2", "afterstory-font.ttf", "Readme.txt", "patch.json"}


def is_archive(path):
    with open(path, "rb") as f:
        return f.read(12) == b"BURIKO ARC20"


def wanted(folder, patch_only=False):
    files = []
    for name in ([] if patch_only else sorted(os.listdir(folder))):
        path = os.path.join(folder, name)
        if not os.path.isfile(path) or name.startswith("."):
            continue
        lower = name.lower()
        if lower.endswith(".arc") and is_archive(path):
            files.append(name)
        elif lower in WANTED_EXACT:
            files.append(name)
    patch = os.path.join(folder, PATCH_DIR)
    if os.path.isdir(patch):
        # Send the manifest last; the app refuses incomplete payloads.
        files.extend(f"{PATCH_DIR}/{name}" for name in sorted(PATCH_FILES - {"patch.json"})
                     if os.path.isfile(os.path.join(patch, name)))
        if os.path.isfile(os.path.join(patch, "patch.json")):
            files.append(f"{PATCH_DIR}/patch.json")
    return files


def device_files(host, code):
    conn = http.client.HTTPConnection(host, PORT, timeout=30)
    conn.request("GET", "/status", headers={"X-Subahibi-Code": code})
    resp = conn.getresponse()
    body = resp.read()
    if resp.status == 403:
        sys.exit("the pairing code was refused: check the code shown on the device")
    if resp.status != 200:
        sys.exit(f"the device answered {resp.status}: {body[:200]!r}")
    return {f["name"]: f["size"] for f in json.loads(body)["files"]}


def send(host, code, folder, name):
    path = os.path.join(folder, name)
    size = os.path.getsize(path)
    conn = http.client.HTTPConnection(host, PORT, timeout=120)
    conn.putrequest("PUT", "/files/" + urllib.parse.quote(name))
    conn.putheader("Content-Length", str(size))
    conn.putheader("X-Subahibi-Code", code)
    conn.endheaders()
    sent, start = 0, time.time()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            conn.send(chunk)
            sent += len(chunk)
            rate = sent / max(time.time() - start, 0.001) / 1e6
            print(f"\r  {name}: {sent * 100 // max(size, 1):3d}%  {rate:6.1f} MB/s", end="", flush=True)
    resp = conn.getresponse()
    body = resp.read()
    print()
    if resp.status != 200:
        sys.exit(f"{name}: the device answered {resp.status}: {body[:200]!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="the address the app shows")
    ap.add_argument("--code", required=True, help="the pairing code the app shows")
    ap.add_argument("--patch-only", action="store_true", help="send only the AfterstoryCHS folder (folder is its parent)")
    ap.add_argument("--force", action="store_true", help="resend even when sizes match")
    ap.add_argument("folder", help="your game folder (the one holding sysprg.arc)")
    args = ap.parse_args()
    if not args.patch_only and not os.path.exists(os.path.join(args.folder, "sysprg.arc")):
        sys.exit("this folder has no sysprg.arc: point at the game folder itself")
    if args.patch_only and not os.path.isfile(os.path.join(args.folder, PATCH_DIR, "patch.json")):
        sys.exit("this folder has no AfterstoryCHS/patch.json: unzip the patch and point at its parent")
    have = device_files(args.host, args.code)
    todo = [n for n in wanted(args.folder, args.patch_only)
            if args.force or args.patch_only or have.get(n) != os.path.getsize(os.path.join(args.folder, n))]
    total = sum(os.path.getsize(os.path.join(args.folder, n)) for n in todo)
    print(f"{len(todo)} files, {total / 1e9:.2f} GB to send ({len(have)} already on the device)")
    for name in todo:
        send(args.host, args.code, args.folder, name)
    print("done: press 重新检查 in the app")


if __name__ == "__main__":
    main()
