// Раздел «Обзор»: всё главное плитками с живым состоянием; плитка ведёт в свой раздел, проблемы подсвечены.
#include <windows.h>

#include <algorithm>

#include "app/autostart.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/corners.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;

namespace studio {

std::function<std::wstring()> g_netStatus;

void Studio::HomePage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    RectF col = Column(in, ui.S(980));
    Header(col, L"Обзор",
           L"Всё главное на одном экране. Плитка ведёт в свой раздел; то, что требует внимания, подсвечено.");

    // данные: дешёвое - каждый кадр, PATH и диски - не чаще раза в 30 с
    ULONGLONG now = GetTickCount64();
    if (!homeAt_ || now - homeAt_ > 30000) {
        homeAt_ = now;
        homeVols_ = tools::Volumes();
        homePathBad_ = 0;
        for (const tools::PathEntry& e : tools::ReadPath()) homePathBad_ += e.dead || e.dup;
        homeAuto_ = AutostartOn();
    }

    struct Tile {
        const wchar_t* icon;
        std::wstring title, value, hint;
        bool warn;
        int page, tab;
    };
    std::vector<Tile> tiles;
    tiles.push_back({L"apps.png", L"Кольца",
                     L"слева " + Plural((int)cfg.left.size(), L"сектор", L"сектора", L"секторов") + L", справа " +
                         std::to_wstring(cfg.right.size()),
                     problems_.empty() ? L"Alt + курсор к краю экрана"
                                       : L"есть пропавшие пути: " + std::to_wstring(problems_.size()),
                     !problems_.empty(), PAGE_RINGS, 0});
    ThemeInfo t = CurrentTheme(cfg);
    tiles.push_back(
        {L"hub.png", L"Тема", t.name, L"кольца, студия, рамки окон и экран блокировки", false, PAGE_THEMES, 0});
    {
        static const wchar_t* kCorner[] = {L"слева вверху", L"справа вверху", L"слева внизу", L"справа внизу"};
        std::wstring v;
        int set = 0;
        for (int i = 0; i < 4; ++i)
            if (!cfg.corners[i].empty()) {
                v += (v.empty() ? L"" : L"\n") + std::wstring(kCorner[i]) + L": " + corners::ActionName(cfg.corners[i]);
                ++set;
            }
        tiles.push_back({L"desktop.png", L"Углы экрана", set ? v : L"ничего не назначено", L"", false, PAGE_DESK, 0});
    }
    std::wstring net = g_netStatus ? g_netStatus() : L"";
    tiles.push_back({L"net-globe.png", L"Сеть", net.empty() ? L"сторож выключен" : net, L"", net.rfind(L"Нет", 0) == 0,
                     PAGE_NET, 0});
    {
        std::wstring v;
        bool warn = false;
        for (const tools::Volume& vol : homeVols_) {
            v += (v.empty() ? L"" : L"   ") + vol.root.substr(0, 2) + L" " + tools::Bytes(vol.free);
            warn = warn || tools::DiskWarn(vol);
        }
        tiles.push_back({L"tools-disk.png", L"Диски", v,
                         warn ? L"места мало - посмотри, что его занимает" : L"свободно", warn, PAGE_TOOLS, 1});
    }
    tiles.push_back(
        {L"tools-path.png", L"PATH",
         homePathBad_ ? Plural(homePathBad_, L"лишняя запись", L"лишние записи", L"лишних записей") : L"всё чисто",
         homePathBad_ ? L"мёртвые папки и дубли - «Почистить»" : L"мёртвых папок и дублей нет", homePathBad_ > 0,
         PAGE_TOOLS, 0});
    std::wstring game = GameStatus();
    tiles.push_back({L"game.png", L"Игровой режим", game.empty() ? L"выключен" : game, L"в игре без анимаций и стекла",
                     false, PAGE_STYLE, 0});
    tiles.push_back({L"app.png", L"Автозапуск",
                     homeAuto_ ? (AutostartEarly() ? L"сразу после входа" : L"включён") : L"выключен",
                     homeAuto_ ? L"запускается вместе с Windows" : L"включить - Настройки, Поведение", !homeAuto_,
                     PAGE_BEHAVIOR, 0});
    tiles.push_back(
        {L"terminal.png", L"Клавиши", L"Alt + край - кольца\nAlt + ` - кольцо окон", L"", false, PAGE_KEYS, 0});

    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    int cols = col.Width > ui.S(860) ? 3 : 2;
    float gap = ui.S(14), tw = (col.Width - gap * (cols - 1)) / cols, th = ui.S(138);
    float y0 = view.Y - off;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const Tile& tl = tiles[i];
        RectF tr(col.X + (i % cols) * (tw + gap), y0 + (i / cols) * (th + gap), tw, th);
        int id = ID_HOME_TILE + (int)i;
        Reveal(tr);
        bool click = ui.Click(id, tr);
        float hov = ui.Anim(id + 50, ui.Hot(id), 16);
        ui.Fill(tr, MixColor(c.card, c.cardHi, hov), ui.S(14));
        ui.Stroke(tr, tl.warn ? c.warn : MixColor(c.border, c.accent, hov * 0.6), ui.S(14), tl.warn ? ui.S(1.6f) : 1);
        ui.Icon(ExeDir() + L"\\icons\\" + tl.icon, RectF(tr.X + ui.S(16), tr.Y + ui.S(16), ui.S(34), ui.S(34)));
        ui.Text(tl.title, RectF(tr.X + ui.S(60), tr.Y + ui.S(14), tr.Width - ui.S(70), ui.S(22)), c.muted, 11.5f);
        if (tl.warn) ui.Fill(RectF(tr.X + tr.Width - ui.S(22), tr.Y + ui.S(18), ui.S(8), ui.S(8)), c.warn, ui.S(4));
        int lines = 1 + (int)std::count(tl.value.begin(), tl.value.end(), L'\n');
        bool multi = lines > 1 || ui.Measure(tl.value, 14, true) > tr.Width - ui.S(76);
        float vh = multi ? ui.S(18) * std::max(lines, 2) + ui.S(6) : ui.S(26);
        ui.Text(tl.value, RectF(tr.X + ui.S(60), tr.Y + ui.S(36), tr.Width - ui.S(72), vh), c.text, multi ? 12 : 14,
                true, ui::Left, multi);
        if (!tl.hint.empty())
            ui.Text(tl.hint, RectF(tr.X + ui.S(60), tr.Y + th - ui.S(38), tr.Width - ui.S(72), ui.S(22)),
                    tl.warn ? c.warn : c.faint, 11);
        if (click) {
            if (tl.page == PAGE_TOOLS)
                Defer([this, tab = tl.tab] { GoTools(tab, L""); });
            else
                Defer([this, p = tl.page] { GoPage((Page)p); });
        }
    }
    size_t rows = (tiles.size() + cols - 1) / cols;
    pageContentH_ = rows * (th + gap);
    ui.G().Restore(st);
}

}  // namespace studio

void SetEditorNetStatus(std::function<std::wstring()> f) { studio::g_netStatus = std::move(f); }
