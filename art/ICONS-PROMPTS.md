# Значки Radial Menu, набор 2: «Багровое поле»

Сейчас в `icons\` лежат значки первого набора (глянцевые наклейки в закатной палитре) и подобранные
из библиотеки картинки для колец «Сеть» и «Утилиты». Под тему «Багровое поле» они не подходят, а часть
секторов делит один значок (Проекты, Процессы и Автозагрузка - все с ящиком, у «Защиты» и «Проверки сети»
один и тот же щит). Этот набор заменяет всё: проработанные маленькие предметы из настоящих материалов,
как значки инвентаря в хорошей игре.

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

| файл | значков | что |
|---|---|---|
| `sheet-root.png` | 6 | первое кольцо |
| `sheet-sub.png` | 10 | второе кольцо, значки проектов |
| `sheet-extra.png` | 8 | правое кольцо, служебные |
| `sheet-studio.png` | 6 | группы студии слева |
| `sheet-tools.png` | 16 | кольцо «Утилиты» |
| `sheet-guard.png` | 5 | «Защита» |
| `sheet-net.png` | 10 | кольцо «Сеть» |
| `app.png`, `hub.png` | 1 + 1 | значок программы, эмблема в центре кольца |
| `sheet-mascot.png` | 5 | по желанию: маскот тура в новой гамме |

---

## 1. `sheet-root.png`: первое кольцо, 6 значков, сетка 3 x 2

Проекты, Код, Учёба, Карьера, Музыка, Программы.

```
A premium icon sheet of exactly six finely crafted miniature objects on a fully transparent background, arranged in a strict 3 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a sleek compact rocket of brushed dark graphite metal with crimson enamel fins and a polished silver nose cone, lifting off from a thin glowing crimson launch ring, a short soft trail of light beneath it, (2) a pair of curly braces forged from polished silver with fine bevelled edges, a small faceted crimson gem glowing between them, (3) a thick closed book bound in dark graphite leather with silver corner guards and a crimson ribbon bookmark, a small graduation cap resting on top with its tassel hanging over the edge, (4) a trophy cup of dark graphite metal with a polished silver rim and handles and a crimson enamel star inlaid on the front, a thin silver laurel around the base, (5) over-ear headphones with a graphite headband, silver hinges and deep crimson cushions, a faint crimson sound-wave arc glowing beside one ear cup, (6) a small graphite tray holding four rounded frosted-glass tiles in a 2 x 2 pattern, one tile glowing crimson from within.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 40 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes, photorealistic clutter
```

## 2. `sheet-sub.png`: второе кольцо, 10 значков, сетка 5 x 2

C++, Java, Веб, Игры, База данных, Таблица, Файлы, Конкурсы, Портфолио, Папка. Они же - бейджи
проектов в кольце (cpp, web, game, db, files...), поэтому силуэты должны читаться и в 20 пикселях.

```
A premium icon sheet of exactly ten finely crafted miniature objects on a fully transparent background, arranged in a strict 5 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a hexagonal badge of dark graphite metal with a polished silver rim and two small raised crimson plus signs side by side, (2) a small ceramic coffee cup in graphite glaze with a silver rim, three thin curls of steam rising and glowing faint crimson at the tips, (3) a globe of frosted glass with fine silver meridian lines on a slim graphite stand, a crimson glow at its core, (4) a compact game controller in graphite with silver accents and crimson buttons, (5) a database cylinder made of three stacked graphite discs separated by thin glowing crimson seams, silver top cap, (6) a small slate board showing a grid of cells etched in silver, one cell filled with glowing crimson, (7) a neat stack of three thin sheets of frosted glass with folded corners, the top sheet edged in silver, (8) a round medal of dark graphite with a silver rim and a crimson enamel star, hanging from a short crimson ribbon, (9) a closed folder of dark graphite leather with a crimson back panel and a small silver star clasp on the front, (10) a plain closed folder of dark graphite with a crimson back panel and a thin silver edge, the generic folder of the set.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 20 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere; the plus signs on the first badge are simple raised shapes, not letters.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, readable code, labels, programming language logos, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes
```

## 3. `sheet-extra.png`: правое кольцо и служебные значки, 8 штук, сетка 4 x 2

Загрузки, Рабочий стол, Корзина, Поиск, Назад, Терминал, Редактор, Ещё.

```
A premium icon sheet of exactly eight finely crafted miniature objects on a fully transparent background, arranged in a strict 4 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a chunky downward arrow of polished silver sinking into an open graphite tray lined with glowing crimson, (2) a slim desktop monitor of dark graphite on a silver stand whose screen shows a tiny field of crimson flowers under a stormy steel blue sky, (3) a rounded trash bin of brushed graphite metal with a silver lid and fine vertical ribs, a thin crimson line glowing along the lid seam, (4) a magnifying glass with a graphite handle and a polished silver ring, the lens of frosted glass with a small crimson glint, (5) a curved arrow turning back to the left, forged from polished silver with a crimson enamel inlay along its body, (6) a small terminal window of dark graphite glass with a silver frame, a crimson prompt chevron and a short glowing crimson cursor bar, (7) a code editor window of dark graphite glass with a silver frame and a few horizontal lines of different lengths etched in steel blue, one line glowing crimson, (8) three round polished silver beads in a row inside a thin graphite ring, the middle bead crimson.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, readable code, labels, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes
```

## 4. `sheet-studio.png`: группы Radial Studio, 6 значков, сетка 3 x 2

Обзор, Кольца, Стол, Сервис, Настройки, Тур (rail слева). Сейчас группы берут чужие значки
(Обзор - эмблему, Сервис - ящик утилит, Настройки - редактор).

```
A premium icon sheet of exactly six finely crafted miniature objects on a fully transparent background, arranged in a strict 3 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a round dashboard gauge of dark graphite with a polished silver bezel, fine engraved tick marks and a glowing crimson needle pointing up and to the right, (2) a half-ring menu of five curved graphite segments fanning out from a small silver hub, the middle segment glowing crimson, (3) a slim monitor of dark graphite on a silver stand whose bottom right corner glows with a cluster of tiny crimson hexagon cells, (4) a graphite open-end wrench lying diagonally across a small frosted glass globe with silver meridian lines, a crimson glow at the globe core, (5) a precise mechanical gear of brushed graphite with a polished silver rim and a glowing crimson core, (6) a small compass of dark graphite with a silver bezel and a crimson needle.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 30 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes
```

## 5. `sheet-tools.png`: кольцо «Утилиты», 16 значков, сетка 4 x 4

Диски, Здоровье, Очистка, Проекты (мусор сборки), PATH, Порты, Процессы, Подкачка, Фон, Батарея,
Автозагрузка, Терминал с окружением, Все утилиты, Кто держит, Версии, Дубли. «Очистка» перекликается
с вихрем в карточке, «Фон» - с тем, что работает без окна.

```
A premium icon sheet of exactly sixteen finely crafted miniature objects on a fully transparent background, arranged in a strict 4 x 4 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a slim M.2 solid state drive stick with a dark graphite board, polished silver chips and a tiny glowing crimson status light, (2) a small graphite drive block with a glowing crimson heartbeat pulse line running across its face, (3) a swirling vortex of tiny crimson and silver square particles spiralling into a dark graphite ring, (4) an open graphite crate full of small silver building blocks, one block glowing crimson, (5) a silver signpost with three graphite arrow-shaped boards pointing in different directions, the top board crimson, (6) a graphite network socket panel with three ports, a polished silver plug inserted into one of them, a crimson glow around it, (7) a square processor chip of dark graphite with fine silver pins on all sides and a glowing crimson core, (8) a memory module stick with a graphite board, a row of chips and polished silver contacts, one chip glowing crimson, (9) a graphite crescent moon with a small silver gear peeking out from behind it, a faint crimson glow in the gear teeth, (10) an upright graphite battery with a silver cap, its charge level glowing crimson at about two thirds, a small engraved silver lightning bolt, (11) a round power button of graphite with a polished silver rim and a glowing crimson power symbol, (12) a terminal window of dark graphite glass with two thin translucent layers stacked behind it, a crimson prompt chevron, (13) a compact toolbox of brushed graphite with silver latches and a crimson handle, (14) a graphite padlock clamped onto the corner of a frosted glass sheet, a crimson glow in the keyhole, (15) a small graphite tag with a silver eyelet and a raised crimson upward chevron, (16) two identical frosted glass sheets overlapping with a slight offset, the front one edged in crimson, a thin silver link joining their corners.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes, two objects that look alike
```

## 6. `sheet-guard.png`: «Защита», 5 значков, сетка 3 x 2 (последняя клетка пустая)

Защита (всё в порядке), Защита (есть опасное), Проверить заново, Починить Windows, Точка восстановления.
Щит той же формы, что в живой сцене проверки: прямые бока, пологая арка сверху, острие снизу.

```
A premium icon sheet of exactly five finely crafted miniature objects on a fully transparent background, arranged in a strict 3 x 2 grid of equal square cells where the last cell of the bottom row stays completely empty, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a heraldic shield with straight sides, a gently arched top and a pointed bottom, made of dark graphite with a polished silver rim and a crimson enamel check mark inlaid in the center, (2) the same shield shape with a fine crack across it and a small crimson enamel beetle sitting on its face, the rim glowing faint crimson, (3) the same shield shape built from a grid of tiny square graphite tiles, a thin horizontal crimson scan line of light passing across it, (4) a polished silver wrench crossed over a small window frame of four frosted glass panes, a crimson glow behind the panes, (5) a round graphite clock face with a crimson hand, wrapped by a curved polished silver arrow circling counterclockwise around it.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, brand logos, operating system logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, an object in the sixth cell, glow spilling far outside the object, objects touching each other, uneven object sizes
```

## 7. `sheet-net.png`: кольцо «Сеть», 10 значков, сетка 5 x 2

Проверить сеть, Починить, Сбросить прокси, Остатки VPN, DNS, Пинг, Роутер, Выход в мир, Обновить адрес,
Кто в сети.

```
A premium icon sheet of exactly ten finely crafted miniature objects on a fully transparent background, arranged in a strict 5 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a small frosted glass globe with silver meridians and a crimson enamel check badge floating in front of it, (2) a polished silver wrench crossing a short graphite network cable whose plug throws a tiny crimson spark, (3) a compact graphite relay box with two silver arrows passing through it, one entering on the left and one leaving on the right, a crimson light on top, (4) a graphite tunnel arch receding into depth with a soft crimson light glowing at its far end, (5) a slim graphite card index with a row of silver tabs, one tab crimson, (6) four rising signal bars of graphite with polished silver tops, the tallest bar glowing crimson, (7) a compact router of dark graphite with two silver antennas and a row of tiny crimson status lights, (8) a frosted glass globe wrapped by a thin silver orbit ring with a small crimson bead travelling on it, (9) two curved polished silver arrows chasing each other in a circle around a small faceted crimson gem, (10) a small graphite processor chip with a silver antenna wave arc rising above it and a glowing crimson core.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, brand logos, watermark, signature, background color or scenery, floor, pedestals, plates, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes, two objects that look alike
```

## 8. `app.png`: значок приложения (трей, exe, шапка студии)

```
A single premium app icon on a fully transparent background, square, centered, filling about 85 percent of the canvas: a round disk of dark brushed graphite metal with a polished silver bevelled rim; on its face a half-ring menu of five curved segments fans out from the left edge like a menu opening from the side of a screen, the segments in frosted dark glass, the middle one glowing deep crimson (#C8323F) from within; at the hub on the left sits a small polished silver circle with a tiny crimson gem; soft cinematic rim light from the upper left with cool steel blue reflections. Large simple shapes and strong contrast between the crimson segment and the dark disk so it reads clearly at 16 pixels. No outline, no sticker border, fully transparent background, no text.
Avoid: sticker style, white outline, cartoon look, pastel colors, pink, orange, gold, green, letters, numbers, labels, watermark, signature, background color, square frame, thin lines that vanish when small, faces, glow outside the disk
```

## 9. `hub.png`: эмблема в центре кольца и плитка «Тема»

```
A single small premium emblem on a fully transparent background, square, centered, filling about 80 percent of the canvas: a round medallion of dark graphite metal with a polished silver rim; on it, a single stylized crimson flower (a spider lily with thin curved petals) in glowing crimson enamel, a fine silver stem, a few tiny silver petals drifting away to the upper right; soft cinematic rim light from the upper left with cool steel blue reflections, crisp and readable at 50 pixels. No outline, no sticker border, fully transparent background, no text.
Avoid: sticker style, white outline, cartoon look, pastel colors, pink, orange, gold, green, letters, numbers, labels, watermark, signature, background color, square frame, faces, realistic photo, glow outside the medallion
```

## 10. `sheet-mascot.png`: по желанию, маскот тура, 5 поз, сетка 5 x 1

Тот же пушистик, что в туре студии и в пустых разделах, только в гамме темы. Если нравится нынешний
лавандовый - лист можно не делать. Порядок: машет, показывает, удивлён, читает, выглядывает
(`mascot-wave`, `-point`, `-surprised`, `-read`, `-peek`). Этот лист я подключу сам (переведу в WebP).

```
A character sheet of exactly five poses of the same small round fluffy blob mascot on a fully transparent background, arranged in a strict 5 x 1 row of equal square cells, each pose centered in its own cell with generous empty space around it, nothing crossing the cell borders. The mascot: a soft round creature the size of a palm with fluffy silver-white fur (#D9DEE6) shaded with cool steel blue (#8FA3BF), big glossy dark eyes with tiny crimson sparkles, small crimson blush on the cheeks, a tiny crimson spider lily petal tucked into its fur like a hair clip, stubby little arms. Poses left to right: (1) happily waving one arm, (2) pointing to the right with one arm, smiling, (3) surprised with wide eyes and a small open mouth, both arms raised, (4) sitting and reading a tiny graphite book with a crimson bookmark, (5) peeking in from the left edge of its cell, only half of the body visible, curious. Polished cute digital painting, soft rim light from the upper left, gentle ambient occlusion, the same character proportions and colors in every pose, no outline, no sticker border. Fully transparent background, no text anywhere.
Avoid: different characters, sticker style, white die-cut border, purple or lavender fur, pink, orange, green, gold, letters, numbers, labels, watermark, signature, background color or scenery, floor, cast shadows on the background, grid lines drawn on the image, frames, poses touching each other, uneven sizes
```

---

# Набор 3: только новые значки, нынешние не меняются

Нынешние значки остаются как есть. Здесь только предметы, которых в наборе ещё нет: каждый сверен со всеми
68 значками в `icons\`, чтобы силуэт не повторял уже нарисованный (указатель PATH, шестерёнку «Поведения»,
кнопку питания «Автозагрузки», лоток «Загрузок», чипы «Процессов», ящики, щиты «Защиты»). Где подходящий
значок уже есть, берётся он:

| пункт | значок |
|---|---|
| «Защита» в студии (Б1) | `tools-shield` / `tools-bug` |
| Починить Windows по шагам (А1) | `tools-repair` |
| Точки восстановления (А2) | `tools-snapshot` |
| Износ батареи (В5) | `tools-battery` |
| Камера и микрофон: открыть Параметры | `tools-privacy` (новый) |

Стиль и палитра - те же, что в наборе 2 (общий хвост повторяется в каждом промпте). Листы класть в `art\` и
запускать `python tools\import_art.py`: имена уже прописаны.

| файл | значков | что |
|---|---|---|
| `sheet-guard2.png` | 8 | Песочница, проверка поведения, PATH открыт, служба, WMI, целостность, «Чисто», «Подождите» |
| `sheet-recovery.png` | 6 | «Восстановление»: сам сектор, Центр обновления, вернуть сломанное, Проводник, особая загрузка, офлайн-проверка |
| `sheet-tools2.png` | 12 | новые утилиты из `NEXT.md` |

## 11. `sheet-guard2.png`: новое в «Защите», 8 значков, сетка 4 x 2

```
A premium icon sheet of exactly eight finely crafted miniature objects on a fully transparent background, arranged in a strict 4 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a sealed cube of thick frosted glass with polished silver edges, a tiny dark graphite gear floating safely inside it, a faint crimson glow at the cube's core, (2) the same frosted glass cube with a thin crimson laser line sweeping across it and small silver measuring ticks along its edges, (3) a heavy graphite door standing slightly ajar in a silver frame, crimson light spilling out through the gap, (4) a compact graphite engine block with two polished silver pistons on top and a crimson glow deep inside its core, (5) a slim silver fishing hook caught on a small graphite lightning bolt, a crimson spark where they touch, (6) a small graphite plate with a finely engraved silver fingerprint, faint crimson light glowing in the ridges, (7) a smooth polished graphite pebble with a calm soft crimson glow inside and a single silver leaf resting on it, (8) a slender hourglass with a graphite frame and silver caps, fine crimson sand trickling from the top bulb to the bottom.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: shields, padlocks, signposts, cogwheels, magnifying glasses, toolboxes, crates, power buttons, microchips, sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, brand logos, operating system logos, watermark, signature, background color or scenery, floor, pedestals, plates under objects, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes, two objects that look alike
```

## 12. `sheet-recovery.png`: «Восстановление», 6 значков, сетка 3 x 2

Сам сектор (спасательный круг), Центр обновления, Вернуть сломанное вирусом, Сбросить Проводник, Особая
загрузка, Офлайн-проверка. Починка Windows и точки восстановления берут нынешние `tools-repair` и
`tools-snapshot`.

```
A premium icon sheet of exactly six finely crafted miniature objects on a fully transparent background, arranged in a strict 3 x 2 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a small lifebuoy ring of dark graphite with four deep crimson enamel bands and a silver rope looped around it, (2) a compact graphite parcel box tied with silver straps, a round crimson enamel seal with a curved arrow pressed on its lid, (3) a broken graphite chain link being rejoined by a crimson glowing clamp with tiny silver bolts, (4) a short graphite broom with silver bristle bands sweeping a small cloud of glowing crimson dust, (5) a round graphite emergency hatch with a polished silver wheel handle in the middle and a thin crimson light leaking around its rim, (6) a sealed graphite capsule pod with silver bands, a loose unplugged cable end hanging beside it and a crimson scanning ring circling the capsule.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: shields, wrenches, clocks, power buttons, toolboxes, crates, folders, circular arrows around a gem, crosses, sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, brand logos, operating system logos, watermark, signature, background color or scenery, floor, pedestals, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes, two objects that look alike
```

## 13. `sheet-tools2.png`: новые утилиты, 12 значков, сетка 4 x 3

Камера и микрофон (Б2), Корневые сертификаты (Б3), Откуда скачан (Б4), Брандмауэр (Б5), Подпись своих exe
(Б6), Ждёт перезагрузки (В1), Устройства с ошибками (В3), Температура (В4), Отчёт о системе (В6), Git (Г1),
Docker и WSL (Г2), Мусор удалённых программ (Д2). «Обновления Windows» (В2) берёт посылку из листа
«Восстановление».

```
A premium icon sheet of exactly twelve finely crafted miniature objects on a fully transparent background, arranged in a strict 4 x 3 grid of equal square cells, each object centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a small graphite webcam with a polished silver lens ring and a tiny glowing crimson recording dot, a slim silver microphone standing beside it, (2) a rolled certificate scroll of dark graphite paper tied with a silver cord, a round crimson enamel seal hanging from it, (3) a graphite map pin with a polished silver rim and a glowing crimson core, (4) a short segment of graphite brick wall with a narrow silver gate in the middle, crimson light glowing through the gate bars, (5) a polished silver fountain pen nib with a drop of glowing crimson ink at its tip, (6) a graphite toggle switch caught mid-flip in a silver housing, a thin crimson arc of motion around the lever, (7) a graphite USB plug with silver contacts and a small crimson enamel warning triangle tag hanging from its cable, (8) a slim thermometer of frosted glass in a graphite and silver housing, crimson liquid rising high inside it, (9) a neat graphite clipboard holding a sheet with silver bar chart columns, the tallest column crimson, (10) a small branching tree of polished silver lines with round graphite nodes, one node glowing crimson, (11) a stack of three small graphite shipping containers with silver ribbing, the top one glowing crimson at its seams, (12) a delicate silver spider web stretched in a corner with a small graphite spider glowing crimson at its center.
Shared style for every object: premium dark fantasy UI icon set in the spirit of a high-end game inventory, NieR Automata inspired minimalism, real materials (brushed graphite metal, frosted glass, polished silver edges, deep crimson enamel, soft crimson inner glow), cinematic soft rim light from the upper left with cool steel blue reflections (#8FA3BF), gentle ambient occlusion, crisp clean silhouette, subtle fine detail (engraved lines, tiny screws, soft wear) that stays readable when shrunk to 34 pixels, the same three-quarter front camera and the same lighting for all objects, colors limited to graphite (#12141C, #2A2F3A), steel (#8FA3BF), silver (#D9DEE6), crimson (#C8323F) and deep crimson (#7E1A23), no outline, no sticker border, objects float on nothing. Fully transparent background, no text anywhere.
Avoid: power buttons, microchips, download trays, down arrows, shields, padlocks, monitors, globes, sticker style, white outline or die-cut border, cartoon look, cute faces or eyes, pastel or candy colors, pink, orange, green, teal, purple, gold, letters, numbers, labels, Git logo, Docker whale, Windows logo, NVIDIA logo, brand logos, watermark, signature, background color or scenery, floor, pedestals, cast shadows on the background, grid lines drawn on the image, frames, glow spilling far outside the object, objects touching each other, uneven object sizes, two objects that look alike
```

## Цвет под тему - без новых картинок

Значки нарисованы в багровом под тему «Багровое поле». Перерисовывать их под каждую из 12 тем не нужно:
багровые детали можно перекрашивать на лету в акцент выбранной темы (графит и серебро остаются как есть),
так же, как кольцо уже сдвигает оттенок групп. Это делается в коде, без генерации.
