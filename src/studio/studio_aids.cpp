// «Настройки / Помощники»: то, что работает рядом с кольцами, - палитра команд, история буфера, раскладки окон,
// росчерк, сторожа процессов, резервная копия, безопасный запуск.
#include <windows.h>

#include <algorithm>

#include "app/palette.h"
#include "core/files.h"
#include "desk/cliphist.h"
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
        bool live = !palette::Status().empty();  // в снимке студии палитра не запускалась
        if (live) cols.Row(b, 2, L"Сейчас: " + palette::Status(), 0.95f);
        cols.Note(b, live ? 3.0f : 2.0f,
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
    cols.Card(L"Буфер обмена", 5.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"История: 30 последних записей");
        toggle(rr, ID_AID + 3, cfg.clipHistory);
        rr = cols.Row(b, 1, L"Секреты не запоминать и стирать");
        toggle(rr, ID_AID + 4, cfg.clipSecrets);
        rr = cols.Row(b, 2, L"Стирать через " + std::to_wstring(cfg.clipSecretSec) + L" с", 0.5f);
        int sec = cfg.clipSecretSec;
        if (ui.Slider(ID_AID + 5, RectF(rr.GetRight() - ui.S(170), rr.Y + ui.S(8), ui.S(170), ui.S(28)), sec, 10, 300,
                      10)) {
            Change();
            cfg.clipSecretSec = sec;
            Save();
        }
        rr = cols.Row(b, 3, L"Сейчас: " + cliphist::Status(), 0.62f);
        if (ui.Button(ID_AID + 6, RectF(rr.GetRight() - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)), L"Забыть всё",
                      Kind::Secondary))
            cliphist::Clear();
        cols.Note(b, 4.0f,
                  L"Кольцо «Буфер» - действие угла или пункт кольца action:clip, записи есть и в палитре: выбрали - "
                  L"снова в буфере и вставлены. Хранится только в памяти. Токены (GitHub, AWS, Google, ключи API, "
                  L"JWT), закрытые ключи, пароли строкой и номера карт не запоминаются и стираются из буфера, если "
                  L"его не заменили. Что менеджеры паролей помечают «не для истории» - не трогаю.",
                  c.faint);
    });
    cols.Card(L"Окна: раскладка и Alt + мышь", 3.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Alt + мышь: тащить и размер");
        toggle(rr, ID_AID + 7, cfg.winDrag);
        rr = cols.Row(b, 1, L"Кольцо «Раскладка» на угол", 0.6f);
        bool onCorner = false;
        for (int i = 0; i < 4; ++i) onCorner |= cfg.corners[i] == L"layout" || cfg.corners2[i] == L"layout";
        if (ui.Button(ID_AID + 8, RectF(rr.GetRight() - ui.S(170), rr.Y + ui.S(4), ui.S(170), ui.S(36)),
                      onCorner ? L"Уже на углу" : L"Выбрать угол", Kind::Secondary))
            Defer([this] { GoPage(PAGE_DESK); });
        cols.Note(b, 2.0f,
                  L"Тащить окно можно за любое место, развёрнутое встаёт обычным под курсором; размер тянется "
                  L"ближним краем или углом. Не в играх, окнах без рамки и там, где Alt + мышь - своя камера (Unreal, "
                  L"Blender, Maya, 3ds Max, Houdini, ZBrush, Krita, Photoshop, Unity, Godot). Кольцо «Раскладка» "
                  L"(угол layout или пункт action:layout, строки «Окно:» в палитре): половины, трети, четверти, "
                  L"середина и несколько последних окон монитора рядом, колонками, «главное и два», четвертями.",
                  c.faint);
    });
    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
