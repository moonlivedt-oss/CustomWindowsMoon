# Android — «Ночная обсерватория»

Создано встроенным image_gen. Финальные ресурсы находятся в `android/app/src/main/res/drawable-nodpi/`.

- `app_offline.webp`, `app_online.webp`: 600×400, WebP quality 86, менее 50 КБ каждый.
- `ic_screen.png`, `ic_remote.png`: 192×192, RGBA, прозрачный фон; предмет занимает около 70% рамки, проверен также в 32×32.

Обработка: Pillow — уменьшение, сжатие и выравнивание прозрачных иконок; альфа-канал сохранён. Исходники генерации остаются в generated_images Codex.

Точные промпты:

## app_offline

```text
Use case: stylized-concept. Asset: Android app state illustration for MoonDesk 'Computer Nearby', part of a cohesive Night Observatory collection. Create one refined tactile miniature diorama, landscape 3:2, 1536x1024. Graphite ceramic, subtly textured dark stone, smoked frosted glass, brushed copper edging, warm coral light #e0414f to #ff7b5c, ivory moonlight. Deep near-black plum background #0f0d12, seamless soft falloff, no enclosing frame. Small personal lunar observatory as a sculptural desktop object, premium editorial product illustration, charming and calm rather than generic sci-fi. Clear forms and sparse composition readable at 260px; subject within central 80%, generous breathing room. No text, letters, logos, watermarks, warning symbols, interface screenshots, humans, excessive stars or clutter. Scene: OFFLINE, a laptop sleeping beneath a crescent. A single graphite laptop rests on a low circular basalt plinth, lid half closed at about 25 degrees, unmistakably a laptop, soft black screen with only the faintest residual coral reflection. Above the lid hangs a small ivory crescent from an elegant thin copper orbital wire like a designer mobile; a tiny copper sphere balances the arc. One faint ember-coral orbit encircles the base, deliberately dim, nearly asleep; no energetic beams. Two small dark lunar pebbles and a tiny unlit observatory dome provide scale, sparse. Cool moonlight softly outlines the closed lid while a warm reflected glow catches the front edge. The feeling is temporary quiet and rest, not broken hardware, not error or danger. Main focus laptop silhouette and crescent, understated cinematic ambient occlusion.
```

## app_online

```text
Use case: stylized-concept. Asset: Android app state illustration for MoonDesk 'Computer Nearby', part of a cohesive Night Observatory collection. Create one refined tactile miniature diorama, landscape 3:2, 1536x1024. Graphite ceramic, subtly textured dark stone, smoked frosted glass, brushed copper edging, warm coral light #e0414f to #ff7b5c, ivory moonlight. Deep near-black plum background #0f0d12, seamless soft falloff, no enclosing frame. Small personal lunar observatory as a sculptural desktop object, premium editorial product illustration, charming and calm rather than generic sci-fi. Clear forms and sparse composition readable at 260px; subject within central 80%, generous breathing room. No text, letters, logos, watermarks, warning symbols, interface screenshots, humans, excessive stars or clutter. Scene: ONLINE, the same little observatory has gently awakened. An open graphite laptop on the left and upright smoked-glass smartphone on the right share a low circular basalt island edged with copper. Both screens glow with the same serene mountain lake under a small coral moon, no interface elements. A single luminous coral elliptical orbit wraps around both devices, its front segment low over the platform and its back segment rising behind the screens; two small pearl-like pulses on that orbit suggest a living connection, not random glitter. A small warmly lit observatory dome and two lunar pebbles complete the miniature. Slender copper arc above with tiny moon sphere balances the silhouette. Warm active lighting from screens contrasts deep plum shadows, peaceful sense of reunion and reliable connection. No padlock, no checkmark, no arrows. Main focal story is two devices sharing one continuous living light orbit.
```

## ic_screen

```text
Use case: stylized-concept. Asset: one isolated Android button icon, part of MoonDesk Night Observatory family. Square 1024x1024, genuinely transparent background with alpha. A flat-ish sculptural icon with very shallow depth, almost front-on, crisp simple silhouette, graphite ceramic body, brushed copper bevels, smoked glass inset, soft restrained coral #ff7b5c edge light and warm ivory detail. Subject fills approximately 70 percent of canvas, centered, no cropping, wide clean transparent padding, polished but highly readable when downsampled to 32x32 pixels. No scene, no plinth, no moon, no orbit, no tiny decorations, no cast shadow outside silhouette, no opaque background, no text, no logo, no border around canvas, no watermark. Subject: a smartphone rotated horizontally into landscape orientation, a simple wide rounded rectangular graphite phone with subtle copper rim and one tiny side camera dot. Its inset screen prominently displays ONE bold ivory desktop monitor symbol: a wide rounded monitor rectangle, short center stem, broad foot, coral screen interior. This monitor symbol occupies most of the phone display and must be unmistakable at tiny size. Very sparse geometry, no screen content beyond that monitor pictogram, no real landscape wallpaper. The horizontal phone and monitor silhouette are the whole icon.
```

## ic_remote

```text
Use case: stylized-concept. Asset: one isolated Android button icon, part of MoonDesk Night Observatory family. Square 1024x1024, genuinely transparent background with alpha. A flat-ish sculptural icon with very shallow depth, almost front-on, crisp simple silhouette, graphite ceramic body, brushed copper bevels, smoked glass inset, soft restrained coral #ff7b5c edge light and warm ivory detail. Subject fills approximately 70 percent of canvas, centered, no cropping, wide clean transparent padding, polished but highly readable when downsampled to 32x32 pixels. No scene, no plinth, no moon, no orbit, no tiny decorations, no cast shadow outside silhouette, no opaque background, no text, no logo, no border around canvas, no watermark. Subject: a compact perfectly circular graphite remote control viewed nearly straight on, a copper outer rim and a shallow frosted-glass circular center. ONE large warm ivory play triangle points right in the coral-lit central button. A subtle wide graphite directional ring around that center, only four broad shallow separators at the cardinal positions, no tiny buttons, no plus or minus signs. The object is an elegant solid round media remote, not a music app logo. Match a horizontal graphite phone icon with copper bevels and an ivory monitor pictogram: identical restrained materials and coral highlights. Circular remote diameter about 70 percent of image width, ample transparent padding. Keep visual hierarchy very simple: circle, ring, play triangle.
```

