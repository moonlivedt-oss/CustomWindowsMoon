"""Режет сгенерированные листы из art/ на значки и кладёт их в icons/.

Запуск: python tools/import_art.py
Значки на листе ищутся по прозрачным промежуткам между ними (порядок: по строкам, слева направо),
поэтому неровная сетка не мешает. Если у картинки нет прозрачности, ровный зелёный фон вырезается.
"""

from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "art"
OUT = ROOT / "icons"
SIZE = 256

SHEETS = {
    "sheet-root.png": ["projects", "code", "study", "career", "music", "apps"],
    "sheet-sub.png": [
        "cpp",
        "java",
        "web",
        "game",
        "db",
        "table",
        "files",
        "contests",
        "portfolio",
        "folder",
    ],
    "sheet-extra.png": [
        "downloads",
        "desktop",
        "recycle",
        "search",
        "back",
        "terminal",
        "editor",
        "more",
    ],
    # группы Radial Studio (rail слева); пока листа нет, студия берёт прежние значки
    "sheet-studio.png": [
        "rail-home",
        "rail-rings",
        "rail-desk",
        "rail-tools",
        "rail-behavior",
        "rail-tour",
    ],
    # кольцо «Утилиты»; новых имён (health, projects, procs, mem, bg) нет - кольцо берёт tools-box
    "sheet-tools.png": [
        "tools-disk",
        "tools-health",
        "tools-clean",
        "tools-projects",
        "tools-path",
        "tools-ports",
        "tools-procs",
        "tools-mem",
        "tools-bg",
        "tools-battery",
        "tools-start",
        "tools-env",
        "tools-box",
        "tools-lock",
        "tools-update",
        "tools-dup",
    ],
    # «Защита» в кольце утилит
    "sheet-guard.png": [
        "tools-shield",
        "tools-bug",
        "tools-scan",
        "tools-repair",
        "tools-snapshot",
    ],
    # набор 3: новое в «Защите» (Песочница, проверка, PATH, службы, WMI, целостность, «Чисто», «Подождите»)
    "sheet-guard2.png": [
        "tools-sandbox",
        "tools-sandbox-check",
        "tools-path-open",
        "tools-service",
        "tools-wmi",
        "tools-integrity",
        "tools-ok",
        "tools-wait",
    ],
    # набор 3: сектор «Восстановление» (свой Recovery Kit); починка и точки - нынешние tools-repair, tools-snapshot
    "sheet-recovery.png": [
        "rec-kit",
        "rec-update",
        "rec-unbreak",
        "rec-explorer",
        "rec-boot",
        "rec-offline",
    ],
    # набор 3: новые утилиты (NEXT.md)
    "sheet-tools2.png": [
        "tools-privacy",
        "tools-certs",
        "tools-origin",
        "tools-firewall",
        "tools-sign",
        "tools-reboot",
        "tools-devices",
        "tools-temp",
        "tools-report",
        "tools-git",
        "tools-containers",
        "tools-leftovers",
    ],
    # набор 4: группы кольца «Утилиты» и пункты, которые пока делят общий значок
    "sheet-tools3.png": [
        "grp-space",
        "grp-memory",
        "grp-dev",
        "tools-studio",
        "tools-dustpan",
        "tools-cache",
        "tools-archive",
        "tools-shelved",
        "tools-stale-server",
        "tools-cable",
        "tools-pagefile",
        "tools-alarm",
    ],
    # кольцо «Сеть»
    "sheet-net.png": [
        "net-check",
        "net-fix",
        "net-proxy",
        "net-tunnel",
        "net-dns",
        "net-ping",
        "net-router",
        "net-globe",
        "net-renew",
        "net-procs",
    ],
}
SINGLES = {"app.png": "app", "hub.png": "hub"}
# Листы, нарисованные строгой сеткой (столбцы, строки): кусок значка (отдельный столбик, стрелка, волна)
# принадлежит клетке, где лежит его середина. Без сетки - крупнейшие области, мелочь к ближайшей.
GRIDS = {
    "sheet-root.png": (3, 2),
    "sheet-sub.png": (5, 2),
    "sheet-extra.png": (4, 2),
    "sheet-studio.png": (3, 2),
    "sheet-tools.png": (4, 4),
    "sheet-guard.png": (3, 2),
    "sheet-net.png": (5, 2),
    "sheet-guard2.png": (4, 2),
    "sheet-recovery.png": (3, 2),
    "sheet-tools2.png": (4, 3),
    "sheet-tools3.png": (4, 3),
}


def find(name: str) -> Path | None:
    stem = Path(name).stem
    for ext in (".png", ".webp", ".jpg", ".jpeg"):
        p = ART / (stem + ext)
        if p.exists():
            return p
    return None


def key_green(im: Image.Image) -> Image.Image:
    """Зелёный хромакей с подавлением зелёной каймы (только для картинок без прозрачности)."""
    im = im.convert("RGBA")
    if im.getextrema()[3][0] < 250:  # уже есть прозрачность
        return im
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            dom = g - max(r, b)
            if dom > 90:
                px[x, y] = (0, 0, 0, 0)
                continue
            if dom > 25:
                a = int(a * (1 - (dom - 25) / 65))
            px[x, y] = (r, min(g, max(r, b) + 8), b, a)
    return im


def blobs(
    im: Image.Image, want: int, grid: tuple[int, int] | None = None
) -> list[tuple[int, int, int, int]]:
    """Рамки отдельных значков: связные области непрозрачных пикселей на уменьшенной маске."""
    scale = 4
    small = im.getchannel("A").resize((im.width // scale, im.height // scale))
    w, h = small.size
    mask = [[v > 40 for v in row] for row in _rows(small)]
    seen = [[False] * w for _ in range(h)]
    boxes = []
    for y in range(h):
        for x in range(w):
            if not mask[y][x] or seen[y][x]:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            x0 = x1 = x
            y0 = y1 = y
            n = 0
            while stack:
                cx, cy = stack.pop()
                n += 1
                x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if (
                        0 <= nx < w
                        and 0 <= ny < h
                        and mask[ny][nx]
                        and not seen[ny][nx]
                    ):
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            boxes.append(
                (n, (x0 * scale, y0 * scale, (x1 + 1) * scale, (y1 + 1) * scale))
            )
    if grid:
        cols, rows = grid
        cells: dict[int, list[int]] = {}
        for n, (a0, b0, a1, b1) in boxes:
            if n < 6:  # пылинки
                continue
            cx = min(cols - 1, int((a0 + a1) / 2 * cols / im.width))
            cy = min(rows - 1, int((b0 + b1) / 2 * rows / im.height))
            c = cells.setdefault(cy * cols + cx, [a0, b0, a1, b1])
            c[0], c[1], c[2], c[3] = (
                min(c[0], a0),
                min(c[1], b0),
                max(c[2], a1),
                max(c[3], b1),
            )
        return [tuple(cells[k]) for k in sorted(cells)]
    # крупнейшие области - значки; мелкие (искры, ноты отдельно от предмета) приклеиваем к ближайшему
    boxes.sort(key=lambda b: -b[0])
    main = [list(b[1]) for b in boxes[:want]]
    for _, (a0, b0, a1, b1) in boxes[want:]:
        cx, cy = (a0 + a1) / 2, (b0 + b1) / 2
        best = min(
            main,
            key=lambda m: ((m[0] + m[2]) / 2 - cx) ** 2 + ((m[1] + m[3]) / 2 - cy) ** 2,
        )
        best[0], best[1], best[2], best[3] = (
            min(best[0], a0),
            min(best[1], b0),
            max(best[2], a1),
            max(best[3], b1),
        )
    # порядок чтения: строки по высоте центра, внутри строки слева направо
    main.sort(key=lambda m: (m[1] + m[3]) / 2)
    rows, row = [], []
    for m in main:
        if row and (m[1] + m[3]) / 2 > max((r[3] for r in row)):
            rows.append(row)
            row = []
        row.append(m)
    rows.append(row)
    return [tuple(m) for r in rows for m in sorted(r, key=lambda m: m[0])]


def _rows(img: Image.Image):
    w = img.width
    data = img.tobytes()  # маска в режиме L: один байт на пиксель
    for i in range(0, len(data), w):
        yield data[i : i + w]


def fit(im: Image.Image) -> Image.Image:
    box = im.getchannel("A").point(lambda v: 255 if v > 12 else 0).getbbox()
    if box:
        im = im.crop(box)
    pad = int(SIZE * 0.06)  # место под ореол (polish)
    im.thumbnail((SIZE - 2 * pad, SIZE - 2 * pad), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    canvas.paste(im, ((SIZE - im.width) // 2, (SIZE - im.height) // 2), im)
    return canvas


def polish(im: Image.Image, gamma: float = 0.72, glow: float = 0.6) -> Image.Image:
    """Тёмные значки (графит) на тёмном кольце теряются: тени приподнимаются, вокруг силуэта - холодный ореол."""
    r, g, b, a = im.split()
    lut = [min(255, int(255 * (v / 255) ** gamma * 1.04)) for v in range(256)]
    rgb = Image.merge("RGB", (r, g, b)).point(lut * 3)
    out = Image.merge("RGBA", (*rgb.split(), a))
    halo_a = a.filter(ImageFilter.GaussianBlur(max(2, im.width * 0.03)))
    halo = Image.new("RGBA", im.size, (170, 186, 210, 0))
    halo.putalpha(halo_a.point(lambda v: int(min(255, v * 1.6) * glow)))
    halo.alpha_composite(out)
    return halo


def save(im: Image.Image, name: str) -> None:
    polish(fit(im)).save(OUT / f"{name}.png")
    print("ok", name)


def main() -> None:
    OUT.mkdir(exist_ok=True)
    for sheet, names in SHEETS.items():
        p = find(sheet)
        if not p:
            print("нет", sheet)
            continue
        im = key_green(Image.open(p))
        found = blobs(im, len(names), GRIDS.get(sheet))
        if len(found) != len(names):
            print(
                f"{sheet}: нашлось {len(found)} значков вместо {len(names)}, пропускаю"
            )
            continue
        for name, box in zip(names, found):
            save(im.crop(box), name)
    for single, name in SINGLES.items():
        p = find(single)
        if not p:
            print("нет", single)
            continue
        im = key_green(Image.open(p))
        save(im, name)
        if name == "app":
            fit(im).save(
                ROOT / "src" / "res" / "app.ico",
                sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)],
            )
            print("ok app.ico (пересобери build.cmd)")


if __name__ == "__main__":
    main()
