// Раздел «Кольца»: шапка выбранного сектора и вкладки; дорожки слева - studio_lanes.cpp, вкладки - studio_tabs.cpp.
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "core/files.h"
#include "core/perf.h"
#include "app/autostart.h"
#include "core/text.h"
#include "core/fx.h"
#include "gfx/accent.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/icons.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

void Studio::SectorMain(const RectF& r) {
    const UiColors& c = ui.C();
    Item* it = Sel();
    RectF in = Inset(r, ui.S(24), ui.S(18));
    if (!it) {
        Mascot(RectF(in.X, in.Y + in.Height * 0.2f, in.Width, in.Height * 0.6f), L"mascot-peek.webp",
               L"Кольцо пока пустое",
               L"Добавь сектор кнопкой слева или перетащи папку из Проводника прямо в это окно.");
        if (ui.Button(
                ID_EMPTY_ADD,
                RectF(in.X + (in.Width - ui.S(160)) / 2, in.Y + in.Height * 0.2f + ui.S(230), ui.S(160), ui.S(38)),
                L"+ Сектор", Kind::Primary)) {
            Change();
            Item n;
            n.name = L"Новый сектор";
            Ring(ring_).push_back(n);
            sel_ = (int)Ring(ring_).size() - 1;
            Save();
        }
        return;
    }
    // шапка: имя, где стоит, кнопки
    float btnY = in.Y + ui.S(4);
    RectF more(in.X + in.Width - ui.S(44), btnY, ui.S(44), ui.S(38));
    bool moreClicked = ui.Button(ID_MORE, more, L"", Kind::Secondary);
    ui.Dots(more, c.text);
    if (moreClicked) {
        ov_ = OV_MORE;
        moreAnchor_ = more;
    }
    RectF open(more.X - ui.S(128), btnY, ui.S(120), ui.S(38));
    if (ui.Button(ID_OPEN, open, L"Открыть", Kind::Primary, IconFile(*it).empty() ? L"" : IconFile(*it)) &&
        !it->full.empty()) {
        std::wstring p = it->full;
        Defer([p] { ShellExecuteW(nullptr, nullptr, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    }
    RectF show(open.X - ui.S(168), btnY, ui.S(160), ui.S(38));
    if (ui.Button(ID_SHOW_MENU, show, L"Показать меню", Kind::Secondary)) {
        bool left = ring_ == 0;
        Defer([this, left] {
            Save();
            if (showMenu) showMenu(left, wnd);
        });
    }
    // значок и имя; пока карточка слева летит сюда (MorphGhost) - на их месте пусто, подпись проступает
    float titleW = show.X - in.X - ui.S(12);
    headIcon_ = RectF(in.X, in.Y + ui.S(2), ui.S(46), ui.S(46));
    headTitle_ = RectF(in.X + ui.S(58), in.Y, titleW - ui.S(58), ui.S(32));
    float morph = fx::Game() ? 1.0f : (float)std::clamp((clock_ - morphAt_) / 0.5, 0.0, 1.0);
    if (morph >= 1) {
        ui.Icon(IconFile(*it), headIcon_, it->full);
        ui.Text(it->name.empty() ? L"Без названия" : it->name, headTitle_, c.text, 21, true);
    }
    ui.Text((ring_ == 0 ? L"левый край, сектор " : L"правый край, сектор ") + std::to_wstring(sel_ + 1) + L" из " +
                std::to_wstring(Ring(ring_).size()),
            RectF(headTitle_.X, in.Y + ui.S(32), headTitle_.Width, ui.S(20)),
            WithAlpha(c.muted, (BYTE)(255 * std::clamp(morph * 1.6f - 0.4f, 0.0f, 1.0f))), 12);
    // выбрали другой сектор или вкладку - поля выезжают заново по очереди
    std::wstring shown = std::to_wstring(ring_) + L"|" + std::to_wstring(sel_) + L"|" + std::to_wstring(tab_);
    if (shown != lastShown_) {
        lastShown_ = shown;
        arriveAt_ = clock_;
    }

    int tab = tab_;
    if (ui.Segmented(ID_TABS, RectF(in.X, in.Y + ui.S(64), std::min(in.Width, ui.S(470)), ui.S(38)),
                     {L"1  Основное", L"2  Второе кольцо", L"3  Значок"}, tab)) {
        tab_ = tab;
        ui.ResetScroll(ID_MAIN_SCROLL);
    }

    RectF view(in.X - ui.S(4), in.Y + ui.S(116), in.Width + ui.S(8), in.Y + in.Height - (in.Y + ui.S(116)));
    float off = ui.Scroll(ID_MAIN_SCROLL, view, mainContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    float y0 = view.Y - off, bottom = y0;
    if (tab_ == 0) TabBasic(view, y0, bottom);
    if (tab_ == 1) TabRing(view, y0, bottom);
    if (tab_ == 2) TabIcon(view, y0, bottom);
    mainContentH_ = bottom - y0 + ui.S(16);
    ui.G().Restore(st);
}

}  // namespace studio
