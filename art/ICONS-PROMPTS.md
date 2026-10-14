# Значки Radial Menu, набор 2: «Багровое поле»

Прошлый набор был наклейками: толстая белая обводка, мультяшный глянец, закатная палитра.
Этот набор - проработанные маленькие предметы из настоящих материалов, как значки инвентаря
в хорошей игре. Цвета - из темы «Багровое поле», под обои с красными цветами:

| цвет | hex | где |
|---|---|---|
| графит | `#12141C` / `#2A2F3A` | корпус предметов, металл |
| сталь | `#8FA3BF` | холодные блики, стекло |
| серебро | `#D9DEE6` | кромки, грани, мелкие детали |
| багровый | `#C8323F` | главный акцент: эмаль, свечение |
| тёмный багровый | `#7E1A23` | глубина акцента, тени на эмали |

Как собрать:

1. Генерируй листы на **прозрачном фоне** (PNG с альфа-каналом). Если генератор прозрачность не умеет,
   проси сплошной фон `#00FF00`: `import_art.py` уберёт зелёный сам.
2. Клади файлы в `RadialMenu\art\` с именами из заголовков (старые файлы с теми же именами заменятся).
3. Запусти `python tools\import_art.py`: листы разрежутся на значки в `icons\`, значок приложения
   пересоберётся. Потом `build.cmd`, если поменялся `app.png`.

Порядок значков в каждом листе важен: скрипт раздаёт имена слева направо, сверху вниз.

Общий хвост стиля повторяется в каждом промпте, чтобы листы вышли одним набором.

---

## 1. `sheet-root.png`: первое кольцо, 6 значков, сетка 3 x 2

Проекты, Код, Учёба, Карьера, Музыка, Программы.

```
A premium icon sheet of exactly six finely crafted miniature objects on a fully transparent background, arranged in a strict 3 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a sleek compact rocket of brushed dark graphite metal with crimson enamel fins and a polished silver nose cone, lifting off from a thin glowing crimson launch ring, a short soft trail of light beneath it, (2) a pair of curly braces forged from polished silver with fine bevelled edges, a small faceted crimson gem glowing between them, (3) a thick closed book bound in dark graphite leather with silver corner guards and a crimson ribbon bookmark, a small graduation cap resting on top with its tassel hanging over the edge, (4) a trophy cup of dark graphite metal with a polished silver rim and handles and a crimson enamel star inlaid on the front, a thin silver laurel around the base, (5) over-ear headphones with a graphite headband, silver hinges and deep crimson cushions, a faint crimson sound-wave arc glowing beside one ear cup, (6) a small graphite tray holding four rounded frosted-glass tiles in a 2 x 2 pattern, one tile glowing crimson from within.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 40 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes, photorealistic clutter
```

## 2. `sheet-sub.png`: второе кольцо, 10 значков, сетка 5 x 2

C++, Java, Веб, Игры, База данных, Таблица, Файлы, Конкурсы, Портфолио, Папка.

```
A premium icon sheet of exactly ten finely crafted miniature objects on a fully transparent background, arranged in a strict 5 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a hexagonal badge of dark graphite metal with a polished silver rim and two small raised crimson plus signs side by side, (2) a small ceramic coffee cup in graphite glaze with a silver rim, three thin curls of steam rising and glowing faint crimson at the tips, (3) a globe of frosted glass with fine silver meridian lines on a slim graphite stand, a crimson glow at its core, (4) a compact game controller in graphite with silver accents and crimson buttons, (5) a database cylinder made of three stacked graphite discs separated by thin glowing crimson seams, silver top cap, (6) a small slate board showing a grid of cells etched in silver, one cell filled with glowing crimson, (7) a neat stack of three thin sheets of frosted glass with folded corners, the top sheet edged in silver, (8) a round medal of dark graphite with a silver rim and a crimson enamel star, hanging from a short crimson ribbon, (9) a closed folder of dark graphite leather with a crimson back panel and a small silver star clasp on the front, (10) a plain closed folder of dark graphite with a crimson back panel and a thin silver edge, the generic folder of the set.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere; the plus signs on the first badge are simple raised shapes, not letters.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, readable code, labels, programming language logos, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes
```

## 3. `sheet-extra.png`: правое кольцо и служебные значки, 8 штук, сетка 4 x 2

Загрузки, Рабочий стол, Корзина, Поиск, Назад, Терминал, Редактор, Ещё.

```
A premium icon sheet of exactly eight finely crafted miniature objects on a fully transparent background, arranged in a strict 4 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a chunky downward arrow of polished silver sinking into an open graphite tray lined with glowing crimson, (2) a slim desktop monitor of dark graphite on a silver stand whose screen shows a tiny field of crimson flowers under a stormy steel blue sky, (3) a rounded trash bin of brushed graphite metal with a silver lid and fine vertical ribs, a thin crimson line glowing along the lid seam, (4) a magnifying glass with a graphite handle and a polished silver ring, the lens of frosted glass with a small crimson glint, (5) a curved arrow turning back to the left, forged from polished silver with a crimson enamel inlay along its body, (6) a small terminal window of dark graphite glass with a silver frame, a crimson prompt chevron and a short glowing crimson cursor bar, (7) a code editor window of dark graphite glass with a silver frame and a few horizontal lines of different lengths etched in steel blue, one line glowing crimson, (8) three round polished silver beads in a row inside a thin graphite ring, the middle bead crimson.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, readable code, labels, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes
```

## 4. `sheet-studio.png`: разделы Radial Studio, 7 значков, сетка 4 x 2 (последняя клетка пустая)

Кольца, Темы, Поведение, Стол, Система, Клавиши, Тур. Раньше разделы брали чужие значки
(«Система» - стопку файлов, «Стол» - наушники), теперь у каждого свой.

```
A premium icon sheet of exactly seven finely crafted miniature objects on a fully transparent background, arranged in a strict 4 x 2 grid of equal square cells where the last cell of the bottom row stays completely empty, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a half-ring menu of five curved graphite segments fanning out from a small silver hub, the middle segment glowing crimson, (2) a faceted glass prism splitting a beam of white light into a crimson ray and a steel blue ray, (3) a small graphite control panel with two vertical silver sliders and one round toggle glowing crimson, (4) a slim monitor of dark graphite whose bottom right corner glows with a soft crimson light, a tiny silver music note floating beside it, (5) a precise mechanical gear of brushed graphite with a polished silver rim and a glowing crimson core, (6) two blank keyboard keycaps side by side, one graphite with a silver edge and one in crimson enamel, slightly tilted, (7) a small compass of dark graphite with a silver bezel and a crimson needle.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 30 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere; the keycaps are blank.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, symbols on the keycaps, labels, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, an object in the eighth cell, glow spilling far outside the object, objects touching each other, uneven object sizes
```

## 5. `app.png`: значок приложения (трей, exe, шапка студии)

```
A single premium app icon on a fully transparent background, square, centered, filling about 85 percent of the canvas: a round disk of dark brushed graphite metal with a polished silver bevelled rim; on its face a half-ring menu of five curved segments fans out from the left edge like a menu opening from the side of a screen, the segments in frosted dark glass, the middle one glowing deep crimson (#C8323F) from within; at the hub on the left sits a small polished silver circle with a tiny crimson gem; soft cinematic rim light from the upper left with cool steel blue reflections. Large simple shapes and strong contrast between the crimson segment and the dark disk so it reads clearly at 16 pixels. No outline, no sticker border, fully transparent background, no text.
Avoid: sticker style, white outline, cartoon look, pastel colors, pink, orange, gold, green, letters, numbers, labels, watermark, signature, background color, square frame, thin lines that vanish when small, faces, glow outside the disk
```

## 6. `hub.png`: эмблема в центре кольца

```
A single small premium emblem on a fully transparent background, square, centered, filling about 80 percent of the canvas: a round medallion of dark graphite metal with a polished silver rim; on it, a single stylized crimson flower (a spider lily with thin curved petals) in glowing crimson enamel, a fine silver stem, a few tiny silver petals drifting away to the upper right; soft cinematic rim light from the upper left with cool steel blue reflections, crisp and readable at 50 pixels. No outline, no sticker border, fully transparent background, no text.
Avoid: sticker style, white outline, cartoon look, pastel colors, pink, orange, gold, green, letters, numbers, labels, watermark, signature, background color, square frame, faces, realistic photo, glow outside the medallion
```
