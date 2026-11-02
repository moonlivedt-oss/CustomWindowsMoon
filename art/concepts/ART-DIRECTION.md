# Изнанка рабочего стола

Концепция дополнительных иллюстраций RadialMenu. Изучены существующие PROMPTS.md, SITE-PROMPTS.md, THEMES-PROMPTS.md и код студии. Это идеи иллюстраций, не обещания новых функций программы. Генеративные картинки не заменяют настоящие скриншоты интерфейса или функциональные значки.

## Визуальный принцип

За краем рабочего стола скрыт компактный мир точных механизмов. Его устройство повторяет действия программы: кольца раскрываются, окна группируются, шум затихает, соединения восстанавливаются. Особенность серии — невозможная физика с понятной функцией: у объёмной вещи тень оказывается плоским интерфейсом, звук становится лентой, а разрозненные окна складываются в один прибор.

Материалы: графитовая керамика, дымчатое стекло, тонкий металл. Багровый и коралловый — функциональные акценты. Свободное пространство вокруг объекта важнее количества деталей. Большие сцены предназначены для обложки, знакомства и оформления; в маленьких состояниях нужна отдельная упрощённая композиция, а не уменьшенная обложка.

## Что стоит сделать

| Приоритет | Иллюстрация | Где пригодится | Необычный образ |
| --- | --- | --- | --- |
| 1 | Карманная обсерватория | Приветствие, обложка сайта, раздел «О программе» | Двухуровневый навигатор выходит из толщины экрана; его тень — плоское меню |
| 2 | Неоткрытое пространство | Ещё не настроенное кольцо | Пустой сектор оказывается маленькой комнатой, в которой свет ждёт первый предмет |
| 3 | Окна-оригами | Знакомство с кольцом окон | Стеклянные окна складываются в радиальную скульптуру, каждое сохраняет свою плоскость |
| 4 | Машина тишины | Фокус и игровой режим | Неровные металлические волны проходят через кольцо и превращаются в одну спокойную световую нить |
| 5 | Сад соединений | Сеть: нормальное состояние, диагностика, восстановление | Оптические волокна привиты к керамическому прибору, разрыв чинится новым световым побегом |
| 6 | Музыка на изнанке | Знакомство с медиапанелью | Из нижнего края вытягивается полупрозрачная лента, на ней рельеф звука; обложка как маленький витраж |
| 7 | Атлас состояний | Нет результатов, ожидание, завершено, недоступно | Один и тот же прибор в разных физических состояниях вместо случайных эмодзи и клипарта |

Первая партия: обсерватория, пустой сектор и машина тишины. После выбора направления лучше генерировать каждый предмет отдельно; для интерфейсных вставок — прозрачный фон и минимум деталей. Для ошибок важнее ясное объяснение и кнопка действия, а иллюстрация остаётся вторичной.

## Первый сгенерированный концепт

`pocket-observatory.png` — самостоятельная концепт-иллюстрация. Встроенный инструмент imagegen, без внешнего API/CLI. Исходник сохранён в проекте; существующая графика и код интерфейса не заменялись. Получившаяся сцена заметно тяготеет к обсерватории и металлу; для компактных интерфейсных вариантов следует уменьшить архитектурные детали и усилить графит/багровый.

### Точный промпт

Use case: conceptual editorial illustration. Create one exceptional art-directed hero concept for RadialMenu, a Windows tool whose radial menu unfolds from the screen edge. Title of concept for context only: 'The pocket observatory / the other side of the desktop'. No actual text anywhere. Wide landscape 16:9 composition, museum-quality surreal physical miniature photographed with macro precision. A thin vertical graphite plane at left is the edge of a desktop, but its edge folds open like a precisely cut book page revealing an impossible circular pocket observatory hidden inside the thickness of the plane. A beautiful off-center mechanical astrolabe with two interrupted concentric rings of smoked glass and satin graphite unfolds out of this seam into the scene; one crimson glass wedge is pulled outward and three smaller orbital compartments fan from it, unmistakably evoking parent and child radial menus. Inside the selected wedge is an exquisite tiny sculptural architectural archive of folded translucent sheets, some resembling window panes, on slender radial rails. The unexpected visual trick: the shadow cast by the physical astrolabe on the plane is a perfectly flat clean semicircular menu, while the actual object is volumetric, a moment between a drawing and a functioning impossible machine. Materials have tactile distinctions: charcoal ceramic, frosted smoky glass, finely brushed dark metal, luminous crimson enamel, subtle warm coral edge caustics. One small pearl cursor-shaped prism suspended near the chosen wedge. Restrained beauty and one strong visual idea, not a pile of gadgets. Deep graphite and wine-black background, crimson #C8323F and coral #FF637A accents, off-white highlights; no blue-purple cyberpunk lighting, no generic galaxy, no planets, no rockets, no robots, no cute mascot, no stock sci-fi portal, no circuit-board textures, no hexagon wallpaper, no excessive bloom, no exploding debris. Light dust and physically plausible soft shadows give scale. Product is composed in left and middle 60 percent, right 40 percent stays quietly dark with carefully controlled light gradient for future heading placement. Cinematic grazing key light, immaculate silhouette, sophisticated negative space, subtle fine film grain. It should feel like a strange collectible precision instrument discovered behind your monitor, intriguing and useful, not a literal app screenshot. No text, no letters, no numbers, no watermark. Finished single image, not a collage or moodboard.
