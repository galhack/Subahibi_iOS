#!/usr/bin/env python3
"""Generate the app's asset catalog icons (app/Resources/Assets.xcassets).

The icon is an original abstract picture - a summer sky gradient, a pale
sun and a few horizon lines - drawn with Pillow; it uses nothing from the
game.  Writes the iOS icon (1024 px), the visionOS layered icon and the tvOS
brand assets (layered app icons and top shelf images), with their
Contents.json files.

usage: make_icons.py [ASSETS_DIR]
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "app", "Resources", "Assets.xcassets")

TOP = (32, 92, 196)
BOTTOM = (178, 222, 250)


def sky(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        c = tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def sun_layer(w, h, cx, cy, r):
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.ellipse((cx - r * 1.6, cy - r * 1.6, cx + r * 1.6, cy + r * 1.6), fill=(255, 250, 220, 70))
    layer = layer.filter(ImageFilter.GaussianBlur(r * 0.35))
    d = ImageDraw.Draw(layer)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 252, 236, 255))
    return layer


def lines_layer(w, h):
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i, frac in enumerate((0.70, 0.78, 0.86)):
        y = int(h * frac)
        width = max(2, int(h * (0.012 - i * 0.002)))
        inset = int(w * (0.18 + i * 0.08))
        d.rounded_rectangle((inset, y, w - inset, y + width), radius=width // 2, fill=(255, 255, 255, 200 - i * 50))
    return layer


def flat_icon(w, h):
    img = sky(w, h).convert("RGBA")
    r = min(w, h) * 0.2
    img = Image.alpha_composite(img, sun_layer(w, h, w * 0.5, h * 0.42, r))
    img = Image.alpha_composite(img, lines_layer(w, h))
    return img.convert("RGB")


def save(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)


def contents(path, data):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "Contents.json"), "w") as f:
        json.dump(data, f, indent=2)


INFO = {"author": "xcode", "version": 1}


def ios_icon():
    d = os.path.join(ASSETS, "AppIcon.appiconset")
    save(flat_icon(1024, 1024), os.path.join(d, "icon-1024.png"))
    contents(d, {"images": [{"filename": "icon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"}],
                 "info": INFO})


def layers(w, h, scale_list, folder, names=("Back", "Middle", "Front")):
    """A layered icon (imagestack / solidimagestack): sky, sun, horizon lines."""
    r = min(w, h) * 0.2
    makers = {
        "Back": lambda W, H: sky(W, H),
        "Middle": lambda W, H: sun_layer(W, H, W * 0.5, H * 0.42, min(W, H) * 0.2),
        "Front": lambda W, H: lines_layer(W, H),
    }
    return makers, r


def vision_icon():
    stack = os.path.join(ASSETS, "AppIconVision.solidimagestack")
    makers, _ = layers(1024, 1024, None, stack)
    order = ["Front", "Middle", "Back"]
    contents(stack, {"info": INFO, "layers": [{"filename": f"{n}.solidimagestacklayer"} for n in order]})
    for n in order:
        layer_dir = os.path.join(stack, f"{n}.solidimagestacklayer")
        contents(layer_dir, {"info": INFO})
        img_dir = os.path.join(layer_dir, "Content.imageset")
        save(makers[n](1024, 1024), os.path.join(img_dir, f"{n.lower()}.png"))
        contents(img_dir, {"images": [{"filename": f"{n.lower()}.png", "idiom": "vision", "scale": "2x"}], "info": INFO})


def tv_stack(folder, w, h, scales, idiom="tv"):
    makers, _ = layers(w, h, scales, folder)
    order = ["Front", "Middle", "Back"]
    contents(folder, {"info": INFO, "layers": [{"filename": f"{n}.imagestacklayer"} for n in order]})
    for n in order:
        layer_dir = os.path.join(folder, f"{n}.imagestacklayer")
        contents(layer_dir, {"info": INFO})
        img_dir = os.path.join(layer_dir, "Content.imageset")
        images = []
        for s in scales:
            name = f"{n.lower()}@{s}x.png"
            save(makers[n](w * s, h * s), os.path.join(img_dir, name))
            images.append({"filename": name, "idiom": idiom, "scale": f"{s}x"})
        contents(img_dir, {"images": images, "info": INFO})


def tv_assets():
    brand = os.path.join(ASSETS, "App Icon & Top Shelf Image.brandassets")
    tv_stack(os.path.join(brand, "App Icon.imagestack"), 400, 240, (1, 2))
    tv_stack(os.path.join(brand, "App Icon - App Store.imagestack"), 1280, 768, (1,))
    for name, w in (("Top Shelf Image", 1920), ("Top Shelf Image Wide", 2320)):
        d = os.path.join(brand, f"{name}.imageset")
        images = []
        for s in (1, 2):
            fn = f"shelf@{s}x.png"
            save(flat_icon(w * s, 720 * s), os.path.join(d, fn))
            images.append({"filename": fn, "idiom": "tv", "scale": f"{s}x"})
        contents(d, {"images": images, "info": INFO})
    contents(brand, {"assets": [
        {"filename": "App Icon - App Store.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "1280x768"},
        {"filename": "App Icon.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "400x240"},
        {"filename": "Top Shelf Image Wide.imageset", "idiom": "tv", "role": "top-shelf-image-wide", "size": "2320x720"},
        {"filename": "Top Shelf Image.imageset", "idiom": "tv", "role": "top-shelf-image", "size": "1920x720"},
    ], "info": INFO})


def launch_color():
    d = os.path.join(ASSETS, "LaunchBackground.colorset")
    contents(d, {"colors": [{"color": {"color-space": "srgb", "components": {
        "red": "0.000", "green": "0.000", "blue": "0.000", "alpha": "1.000"}}, "idiom": "universal"}], "info": INFO})


if __name__ == "__main__":
    contents(ASSETS, {"info": INFO})
    ios_icon()
    vision_icon()
    tv_assets()
    launch_color()
    print("icons written to", ASSETS)
