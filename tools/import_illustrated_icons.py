"""Extract the 4x3 generated icon atlas, preserving its alpha channel.

python tools/import_illustrated_icons.py crimson path/to/sheet.png
python tools/import_illustrated_icons.py --check
This is asset slicing, not a substitute for generative artwork.
"""
from pathlib import Path
import argparse
import json
import shutil
from collections import deque
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'docs' / 'design' / 'illustrated-icons'
OUT = ROOT / 'icons' / 'illustrated'
NAMES = ['projects', 'code', 'study', 'game', 'music', 'apps', 'folder', 'downloads', 'web', 'terminal', 'career', 'tools']
THEMES = ['crimson', 'sunset', 'moon', 'mocha', 'tokyo', 'dracula', 'rosepine', 'nord', 'gruvbox', 'onedark', 'forest', 'wallpaper', 'custom']


def isolate(cell):
    """Discard tiny disconnected atlas bleed; retain floating parts (e.g. the game die)."""
    alpha = cell.getchannel('A')
    w, h = cell.size
    pixels = bytearray(alpha.point(lambda a: 255 if a > 24 else 0).tobytes())
    components = []
    for start in range(w * h):
        if not pixels[start]:
            continue
        queue = deque([start])
        pixels[start] = 0
        comp = []
        while queue:
            pos = queue.popleft()
            comp.append(pos)
            x, y = pos % w, pos // w
            for adjacent in ((pos - 1 if x else -1), (pos + 1 if x + 1 < w else -1),
                             (pos - w if y else -1), (pos + w if y + 1 < h else -1)):
                if adjacent >= 0 and pixels[adjacent]:
                    pixels[adjacent] = 0
                    queue.append(adjacent)
        components.append(comp)
    largest = max(map(len, components), default=0)
    keep = bytearray(w * h)
    for comp in components:
        if len(comp) >= max(48, largest * 0.015):
            for pos in comp:
                keep[pos] = 255
    mask = Image.frombytes('L', (w, h), bytes(keep)).filter(ImageFilter.MaxFilter(5))
    cell.putalpha(ImageChops.multiply(alpha, mask))
    return cell


def preview():
    ready = [t for t in THEMES if (OUT / t / 'projects.png').exists()]
    sheet = Image.new('RGB', (1340, 70 + len(ready) * 112), '#10141d')
    d = ImageDraw.Draw(sheet)
    font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 18)
    d.text((25, 18), 'RADIAL MENU / ILLUSTRATED ICONS', font=font, fill='#e9edf5')
    for row, theme in enumerate(ready):
        y = 65 + row * 112
        d.text((15, y + 35), theme, font=font, fill='#c4cddd')
        for i, name in enumerate(NAMES):
            im = Image.open(OUT / theme / f'{name}.png').convert('RGBA').resize((86, 86), Image.Resampling.LANCZOS)
            sheet.paste(im, (140 + i * 99, y), im)
    sheet.save(SOURCE / 'preview.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('theme', nargs='?', choices=THEMES)
    parser.add_argument('sheet', nargs='?', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    SOURCE.mkdir(parents=True, exist_ok=True)
    if args.check:
        count = 0
        for theme in THEMES:
            for name in NAMES:
                path = OUT / theme / f'{name}.png'
                with Image.open(path) as im:
                    im.load()
                    assert im.size == (256, 256) and im.mode == 'RGBA', path
                    assert im.getpixel((0, 0))[3] == 0, path
                    assert im.getchannel('A').getextrema() == (0, 255), path
                count += 1
        print(f'Checked {count} icons: 13 complete themes, real alpha and PNG decoding')
        return
    if not args.theme or not args.sheet:
        parser.error('theme and sheet are required')
    src = SOURCE / f'{args.theme}-sheet.png'
    if args.sheet.resolve() != src.resolve():
        shutil.copy2(args.sheet, src)
    with Image.open(src) as opened:
        im = opened.convert('RGBA')
    assert im.getchannel('A').getextrema()[0] == 0, 'Atlas has no transparent gutters'
    dest = OUT / args.theme
    dest.mkdir(parents=True, exist_ok=True)
    metadata = []
    for i, name in enumerate(NAMES):
        x, y = i % 4, i // 4
        cell = im.crop((round(x * im.width / 4), round(y * im.height / 3),
                        round((x + 1) * im.width / 4), round((y + 1) * im.height / 3)))
        cell = isolate(cell)
        # Ignore faint alpha specks only for finding the object bounds; retain actual alpha.
        bounds = cell.getchannel('A').point(lambda a: 255 if a > 24 else 0).getbbox()
        assert bounds, (args.theme, name)
        obj = cell.crop(bounds)
        obj.thumbnail((226, 226), Image.Resampling.LANCZOS)
        canvas = Image.new('RGBA', (256, 256))
        canvas.alpha_composite(obj, ((256 - obj.width) // 2, (256 - obj.height) // 2))
        canvas.save(dest / f'{name}.png')
        metadata.append({'name': name, 'cell': i, 'bounds': bounds})
    (SOURCE / f'{args.theme}-slicing.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    preview()
    print(f'{args.theme}: imported {len(NAMES)} generated icons')


if __name__ == '__main__':
    main()
