// «Настройки / Помощники»: то, что работает рядом с кольцами, - палитра команд, история буфера, раскладки окон,
// росчерк, сторожа процессов, резервная копия, безопасный запуск.
#include <windows.h>

#include <algorithm>

#include "app/palette.h"
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
    const UiColors& c = ui.C();
    auto toggle = [&](const RectF& rr, int id, bool& v) {
        bool val = v;
        if (ui.Toggle(id, RectF(rr.X + rr.Width - ui.S(42), rr.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };

    cols.Card(L"Командная палитра", 3.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Открывать по сочетанию");
        toggle(rr, ID_AID + 0, cfg.palette);
        rr = cols.Row(b, 1, L"Сочетание", 0.4f);
        FieldOrText(
            ID_AID + 1, RectF(rr.X + rr.Width * 0.42f, rr.Y + ui.S(4), rr.Width * 0.58f, ui.S(36)), view,
            cfg.paletteKey,
            [this](const std::wstring& s) {
                FieldEdit(ID_AID + 1);
                cfg.paletteKey = s;
            },
            L"alt+space");
        if (!palette::Status().empty()) cols.Row(b, 2, L"Сейчас: " + palette::Status(), 0.95f);
        cols.Note(b, 3.0f,
                  L"Строка поиска по всем пунктам колец, действиям утилит и сети, разделам студии и открытым окнам; "
                  L"Enter - выполнить. Буквы можно пропускать, раскладка не важна: «gc» найдёт «Проверить сеть».",
                  c.faint);
    });
    cols.Card(L"Росчерк", 2.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Махнуть с Alt - выбрать без ожидания");
        toggle(rr, ID_AID + 2, cfg.stroke);
        cols.Note(b, 1.0f,
                  L"Зажали Alt у края и сразу махнули мышью в сторону сектора: отпустили Alt над пунктом - он "
                  L"открывается, кольцо можно не ждать. Над группой или папкой кольцо остаётся, дальше - ломаной во "
                  L"второе кольцо. Медленный подход к кольцу работает как раньше.",
                  c.faint);
    });
    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
