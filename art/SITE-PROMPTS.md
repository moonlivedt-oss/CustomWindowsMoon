# Картинки для сайта (docs/index.html)

Готовые файлы класть в `docs/img/` с точно такими именами. Нет файла - сайт просто показывает карточку без
картинки, ничего не ломается.

Форматы: карточки «Что умеет» - 1200x900 (4:3), WebP до ~150 КБ; фон первого экрана - 2400x1350, WebP до ~300 КБ;
превью ссылки - 1200x630, PNG.

Общий стиль - вставлять в начало каждого промпта:

> Dark moody digital illustration, deep wine-black background (#140b0f), crimson (#c8323f) and warm coral (#ff7b5c)
> light accents, frosted dark glass surfaces with soft blur, subtle hexagonal glass texture, cinematic soft glow,
> clean minimal composition, no text, no letters, no logos, no watermark.

Это иллюстрации, а не снимки экрана: интерфейс на них условный. Если хочется показать настоящую программу -
вместо картинки можно положить свой скриншот с тем же именем.

| файл | что на картинке | промпт (после общего стиля) |
|---|---|---|
| `hero.webp` | фон первого экрана | Abstract wide backdrop: the left edge of a dark computer screen glowing crimson, a half-ring of frosted glass segments emerging from that edge, soft bokeh of warm light on the right, lots of empty dark space on the right half for a headline. 16:9. |
| `rings.webp` | кольцо у края | Close-up of a semicircular radial menu made of frosted glass segments emerging from the left edge of a dark desktop, one segment highlighted crimson and pushed outward, a second smaller ring of glass tiles fanning out from it, simple folder and app glyphs as abstract shapes. |
| `corners.webp` | горячие углы | The bottom-left corner of a dark screen, a fan of small glowing hexagons spreading from the corner, an arrow-shaped mouse cursor resting in the corner, a thin crimson arc showing progress. |
| `music.webp` | что играет | A floating glass media card sliding up from the bottom edge of a dark screen: square album cover with warm abstract art, play and skip buttons as simple circles, a thin coral progress line; the card's glow takes the colour of the cover. |
| `windows.webp` | окна | A circle of small floating window thumbnails arranged around a mouse cursor on a dark desktop, one window in front with a thin crimson border, another window semi-transparent like tinted glass. |
| `phone.webp` | телефон рядом | A smartphone and a laptop on a dark desk, photos flying from the phone to the laptop as small glowing cards along an arc, a QR code shape glowing on the laptop screen, no cables. |
| `net.webp` | интернет | Stylised home network: a laptop, a Wi-Fi router and a globe connected by glowing lines, one broken link being repaired with a crimson spark, calm and reassuring mood. |
| `tools.webp` | порядок в компьютере | A hard drive drawn as a honeycomb pie chart of glowing hexagons, some cells dissolving into fine sand-like particles that drift into a small bin, tidy and satisfying feeling. |
| `guard.webp` | защита и приватность | A shield assembled from many small glowing particles in front of a dark desktop, a check mark inside the shield, a few suspicious grey shapes held back outside it. |
| `game.webp` | игровой режим | A gaming laptop with a game scene glowing on screen, a soft shockwave ring of hexagons expanding from the screen, background apps shown as small dimmed cards drifting to sleep at the side. |
| `beauty.webp` | оформление | A row of six small desktop previews, each in a different colour theme (crimson, forest green, nord blue, sunset orange, mocha, violet), the middle one enlarged with a glowing crimson window border. |
| `og.png` | превью ссылки в мессенджерах | Wide banner 1200x630: the crimson glass half-ring at the left edge, dark empty space on the right where the site title will be added later; keep the right two thirds calm and dark. |

## Следующие (по желанию)

Тот же общий стиль в начале промпта. Готовое - в `00 Входящие` библиотеки картинок.

| файл | где | что на картинке |
|---|---|---|
| `divider-arc.webp` (2400x240, прозрачный фон) | между разделами сайта | A very wide thin horizontal strip: a single long arc of frosted crimson glass segments, like a slice of a giant radial ring seen edge-on, glowing softly in the middle and fading to nothing at both ends. |
| `divider-hex.webp` (2400x240, прозрачный фон) | между разделами сайта | A very wide thin horizontal strip of small glowing crimson hexagons, dense in the centre and scattering into sparse sparks towards both ends, like a wave travelling sideways. |
| `start-bg.webp` (1920x1080) | фон раздела «Как начать» | Calm dark desk scene from above, a laptop with a softly glowing crimson half-ring at the left edge of its screen, a coffee cup, lots of empty dark space; very low contrast so text on top stays readable. |
| `app-icon.png` (1024x1024, прозрачный фон) | значок Android-приложения «Компьютер рядом» | App icon: a rounded smartphone outline and a small laptop outline joined by a short glowing crimson arc, centred, simple bold shapes that read at 48 px, no text. |
| `app-empty.webp` (1024x1024, прозрачный фон) | приложение, пока ПК не подключён | A small sleeping laptop with a dim crimson half-ring at its edge and a phone beside it waiting, a dotted arc between them not yet connected, friendly calm mood, no text. |
