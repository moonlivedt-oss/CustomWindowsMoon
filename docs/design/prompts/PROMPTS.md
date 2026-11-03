# Картинки для Radial Menu

Стиль как у маскота и обложек MoonLight / Test Menu Moon: глянцевые милые предметы с толстой белой
обводкой-наклейкой. Палитра заката, без зелёного:

| цвет | hex | где |
|---|---|---|
| слива (контур, тени) | `#3B1F4A` | контур, тёмные места |
| сумеречный фиолетовый | `#7A4B9A` | глубина, тени предметов |
| малиново-розовый | `#E8608C` | основной цвет |
| коралл | `#FF7A6B` | основной цвет, переходы |
| персик | `#FFB38A` | светлые грани |
| золото солнца | `#FFD36E` | акценты, свечение |

Генерируй **на прозрачном фоне** (PNG с альфа-каналом). Готовые файлы клади сюда, в `RadialMenu\art\`,
с именами из заголовков, и запусти `python tools\import_art.py`: скрипт разрежет листы на значки,
сложит их в `icons\` и пересоберёт иконку приложения.

Значки рисуются листами: один стиль на весь лист выходит ровнее, чем по одной картинке.

---

## 1. `sheet-root.png`: первое кольцо, 6 значков, сетка 3 x 2

```
A clean sticker sheet of exactly six cute glossy icons on a fully transparent background, arranged in a strict 3 x 2 grid of equal square cells, every icon centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a chubby rocket lifting off with a little puff of cloud and a golden flame, (2) a pair of rounded curly braces with a tiny glowing star between them, (3) a graduation cap resting on a thick closed book with a golden bookmark ribbon, (4) a trophy cup with a star on it and a few tiny sparkle marks, (5) round over-ear headphones with two small music notes floating above, (6) a square of four rounded app tiles in a 2 x 2 pattern, one tile lit golden. All six icons share one consistent style: cute glossy cartoon objects painted in a sunset palette, smooth gradients from raspberry pink (#E8608C) through coral (#FF7A6B) to peach (#FFB38A), shadows in dusky violet (#7A4B9A), small accents and warm glow in sunset gold (#FFD36E), bold rounded deep plum outline (#3B1F4A), big soft white glossy highlights like jelly or vinyl toys, as if lit by a warm evening sun from the upper left, a thick clean white die-cut sticker border around each icon, chunky friendly silhouette that still reads clearly when shrunk to 40 pixels, slight three-quarter front view. Same visual family as a cute glossy blob mascot sticker set. Transparent background, no backdrop, no text anywhere.
Avoid: green or teal colors, letters, numbers, labels, captions, watermark, signature, background color or scenery, grid lines drawn on the image, frames, cast shadows on the background, glow spilling outside the sticker border, faces or eyes on the objects, realistic metal, harsh black outlines, icons touching each other, uneven icon sizes
```

## 2. `sheet-sub.png`: второе кольцо, 10 значков, сетка 5 x 2

```
A clean sticker sheet of exactly ten cute glossy icons on a fully transparent background, arranged in a strict 5 x 2 grid of equal square cells, every icon centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a rounded hexagon badge with two small plus signs on it, (2) a coffee cup with three curls of steam, (3) a round globe with a soft grid of meridians, (4) a chubby game controller, (5) a database cylinder made of three stacked rounded discs, (6) a small spreadsheet board with a grid of cells and one cell lit golden, (7) a stack of three paper sheets with folded corners, (8) a round medal on a ribbon, (9) a closed folder with a golden star on its front, (10) a plain closed folder, simple and cute, the generic folder of the set. All ten icons share one consistent style: cute glossy cartoon objects painted in a sunset palette, smooth gradients from raspberry pink (#E8608C) through coral (#FF7A6B) to peach (#FFB38A), shadows in dusky violet (#7A4B9A), small accents and warm glow in sunset gold (#FFD36E), bold rounded deep plum outline (#3B1F4A), big soft white glossy highlights like jelly or vinyl toys, as if lit by a warm evening sun from the upper left, a thick clean white die-cut sticker border around each icon, chunky friendly silhouette that still reads clearly when shrunk to 34 pixels, slight three-quarter front view. Same visual family as a cute glossy blob mascot sticker set. Transparent background, no backdrop, no text anywhere; the plus signs on the first badge are simple shapes, not letters.
Avoid: green or teal colors, letters, numbers, labels, captions, programming language logos, brand logos, watermark, signature, background color or scenery, grid lines drawn on the image, frames, cast shadows on the background, glow spilling outside the sticker border, faces or eyes on the objects, realistic metal, harsh black outlines, icons touching each other, uneven icon sizes
```

## 3. `sheet-extra.png`: правое кольцо и служебные значки, 8 штук, сетка 4 x 2

```
A clean sticker sheet of exactly eight cute glossy icons on a fully transparent background, arranged in a strict 4 x 2 grid of equal square cells, every icon centered in its own cell with generous empty space around it, nothing crossing the cell borders. Reading order left to right, top to bottom: (1) a chunky arrow pointing down into an open tray, (2) a small desktop monitor on a stand whose screen shows a tiny sunset over the sea, (3) a rounded trash bin with a lid and soft vertical ribs, (4) a magnifying glass with a golden glint on the lens, (5) a curved arrow turning back to the left, (6) a terminal window with a prompt chevron and a short cursor bar in gold, (7) a code editor window with a few lines of different lengths, one line golden, (8) three round dots inside a circle. All eight icons share one consistent style: cute glossy cartoon objects painted in a sunset palette, smooth gradients from raspberry pink (#E8608C) through coral (#FF7A6B) to peach (#FFB38A), shadows in dusky violet (#7A4B9A), small accents and warm glow in sunset gold (#FFD36E), bold rounded deep plum outline (#3B1F4A), big soft white glossy highlights like jelly or vinyl toys, as if lit by a warm evening sun from the upper left, a thick clean white die-cut sticker border around each icon, chunky friendly silhouette that still reads clearly when shrunk to 34 pixels, slight three-quarter front view. Same visual family as a cute glossy blob mascot sticker set. Transparent background, no backdrop, no text anywhere.
Avoid: green or teal colors, letters, numbers, readable code, labels, captions, brand logos, watermark, signature, background color or scenery, grid lines drawn on the image, frames, cast shadows on the background, glow spilling outside the sticker border, faces or eyes on the objects, realistic metal, harsh black outlines, icons touching each other, uneven icon sizes
```

## 4. `app.png`: значок приложения (трей и exe)

```
A single cute glossy app icon on a fully transparent background, square, centered, filling about 85 percent of the canvas. A round disk painted like a small sunset sky, deep plum at the top fading to raspberry and coral at the bottom; on it, a half-ring menu of five chunky rounded segments fans out from the left side like a menu opening from the edge of a screen; the middle segment glows sunset gold (#FFD36E), the other four are glossy coral and peach; at the hub on the left sits a small glowing half-set sun. Bold rounded deep plum outline (#3B1F4A), big soft glossy highlights, a thick clean white die-cut sticker border around the whole disk. Simple enough to read at 16 pixels: few large shapes, strong contrast between the gold segment and the rest. Transparent background, no text.
Avoid: green or teal colors, letters, numbers, labels, watermark, signature, background color, square frame, glow outside the sticker border, thin lines that vanish when small, faces, realistic metal
```

## 5. `hub.png`: эмблема в центре меню, у края экрана

```
A single small cute emblem on a fully transparent background, square, centered, filling about 80 percent of the canvas: a glowing sunset-gold sun (#FFD36E) half sunk behind two soft wavy coral and raspberry stripes of sea, with a tiny four-pointed sparkle above it, inside a thin glossy peach ring, soft glossy highlights, bold rounded deep plum outline (#3B1F4A), a thick clean white die-cut sticker border around the whole emblem, very clean and readable at 50 pixels. Transparent background, no text.
Avoid: green or teal colors, letters, numbers, labels, watermark, signature, background color, square frame, glow outside the sticker border, face on the sun, realistic style
```

## 6. `cover-radial-menu.png`: обложка в коллекцию `image\06 Иллюстрации\Обложки`, 2:1

Обложка с фоном, прозрачность тут не нужна.

```
A wide README banner illustration, 2:1. Left part: the title «Radial Menu» in large elegant glowing lettering in warm sunset gold with a soft glow and a small four-pointed sparkle above one letter, and under it a smaller subtitle in Russian «Папки по Alt и краю экрана» in warm cream; generous margins, on a calm uncluttered area of deep plum evening sky. Right part: a sunset over a calm sea, the sky fading from deep plum (#3B1F4A) and dusky violet through raspberry pink and coral to a golden glow at the horizon, the sun half sunk into the water with a shimmering path of light; in front of it the rounded right edge of a big computer screen floats among soft peach clouds; from the screen's edge a half-ring menu unfolds like a fan of chunky glossy segments in coral and peach with golden rims, one segment glowing gold, and a second outer ring opens beyond it; little cute sticker-style icons drift out of the segments like petals in the warm wind: folders, a chubby rocket, headphones with music notes, a trophy, a game controller; at the hub of the menu sits a small cute round lavender blob mascot with big sparkly eyes and rosy cheeks, lit warmly by the sunset, happily hugging a large glossy keyboard keycap «Alt»; a few early stars appear in the plum part of the sky. Cozy magical evening illustration in a polished cute digital painting style: soft glow, rounded shapes, gentle rim light, rich warm atmosphere, full of small details worth discovering on a second look. Color palette: deep plum (#3B1F4A), dusky violet (#7A4B9A), raspberry pink (#E8608C), coral (#FF7A6B), peach (#FFB38A), sunset gold (#FFD36E). The text is spelled exactly as in the quotes, large and easy to read even when the image is shown small; besides the title, the subtitle and the keycap «Alt», nothing else is written anywhere.
Avoid: green or teal colors, misspelled or garbled text, Latin letters in the Russian text, extra words or labels on the icons, numbers, real game logos or characters, people, slogans, watermark, signature, ui screenshots, frames, borders, clutter behind the title, gloomy mood, harsh neon, violence
```
