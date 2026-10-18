// «Настройки / Помощники»: то, что работает рядом с кольцами, - палитра команд, история буфера, раскладки окон,
// росчерк, сторожа процессов, резервная копия, безопасный запуск.
#include <windows.h>

#include <algorithm>

#include "core/files.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

void Studio::AidsPage(const RectF& r) {
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Помощники", L"Что работает рядом с кольцами. Всё выключается.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));
    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
