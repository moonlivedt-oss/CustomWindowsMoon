"""Apply a generated icon palette without changing radial.cfg.

python tools/apply_icon_theme.py aurora
Restart RadialMenu afterwards to clear its image cache.
"""
import argparse
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('theme', choices=['crimson', 'aurora', 'dusk', 'sand'])
    args = parser.parse_args()
    source = ROOT / 'icons' / 'themes' / args.theme
    files = sorted(source.glob('*.png'))
    if not files:
        parser.error('Run tools/build_identity.py first')
    backup = ROOT / 'backups' / 'before-icon-theme'
    backup.mkdir(parents=True, exist_ok=True)
    for file in files:
        target = ROOT / 'icons' / file.name
        if target.exists() and not (backup / file.name).exists():
            shutil.copy2(target, backup / file.name)
        shutil.copy2(file, target)
    print(f'Applied {args.theme}: {len(files)} icons. Restart RadialMenu.')


if __name__ == '__main__':
    main()
