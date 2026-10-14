"""Готовит PNG-значки для меню из библиотеки image и рисует иконку приложения.

Запуск: python tools/make_icons.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT.parent / "image" / "04 Иконки"
OUT = ROOT / "icons"
SIZE = 128

# имя файла в библиотеке -> имя в icons/
PICK = {
    "rocket.webp": "projects.png",
    "curly-braces-block.webp": "code.png",
    "graduation-cap.webp": "study.png",
    "trophy-cup.webp": "career.png",
    "equalizer-bars.webp": "music.png",
    "monitor-terminal.webp": "apps.png",
    "folder-star.webp": "portfolio.png",
    "medal.webp": "contests.png",
    "folder.webp": "folder.png",
}


def find(name: str) -> Path:
    hits = sorted(LIB.rglob(name))
    if not hits:
        raise SystemExit(f"нет в библиотеке: {name}")
    return hits[0]


def convert() -> None:
    OUT.mkdir(exist_ok=True)
    for src, dst in PICK.items():
        im = Image.open(find(src)).convert("RGBA")
        im.thumbnail((SIZE, SIZE), Image.LANCZOS)
        canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        canvas.paste(im, ((SIZE - im.width) // 2, (SIZE - im.height) // 2), im)
        canvas.save(OUT / dst)
        print("ok", dst)


def app_icon() -> None:
    """Тёмный диск с красным полукольцом из сегментов."""
    s = 256
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse(
        (8, 8, s - 8, s - 8), fill=(18, 20, 27, 255), outline=(60, 64, 76, 255), width=6
    )
    box = (40, 40, s - 40, s - 40)
    for i in range(5):
        a0 = -80 + i * 34
        col = (215, 38, 61, 255) if i == 2 else (150, 156, 170, 255)
        d.arc(box, a0, a0 + 28, fill=col, width=34)
    d.ellipse(
        (s // 2 - 22, s // 2 - 22, s // 2 + 22, s // 2 + 22), fill=(215, 38, 61, 255)
    )
    im.save(OUT / "app.png")
    im.save(
        ROOT / "src" / "app.ico",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)],
    )
    print("ok app.ico")


if __name__ == "__main__":
    convert()
    app_icon()
