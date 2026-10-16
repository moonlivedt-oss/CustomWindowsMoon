// «Состояние» - главная страница студии: живые шкалы дисков и памяти, что можно освободить одной кнопкой,
// мусор проектов и Загрузки с выбором строк, рост за неделю, драйвер, здоровье дисков, автозапуск.
// «Автоматика» - что программа делает сама: переключатели, когда было в последний раз, и журнал действий.
#include <windows.h>

#include <algorithm>
#include <cmath>

#include "app/autos.h"
#include "app/autostart.h"
#include "app/toolsui.h"
#include "core/files.h"
#include "core/fx.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

// Идёт чистка: по полосе бегут косые штрихи, а с неё вверх улетают и тают крупинки мусора.
void CleanDust(ui::Ui& ui, const RectF& bar, Color a, Color b) {
    Graphics& g = ui.G();
    ui.Tick();
    double t = GetTickCount64() / std::max(1.0, fx::Dur(1000));
    GraphicsState st = g.Save();
    g.SetClip(ui.Round(bar, bar.Height / 2), CombineModeIntersect);
    SolidBrush stripe(Color(40, 255, 255, 255));
    float step = ui.S(16), shift = (float)std::fmod(t * ui.S(40), step);
    for (float x = bar.X - bar.Height - step + shift; x < bar.GetRight(); x += step) {
        PointF q[4] = {{x, bar.GetBottom()},
                       {x + ui.S(7), bar.GetBottom()},
                       {x + ui.S(7) + bar.Height, bar.Y},
                       {x + bar.Height, bar.Y}};
        g.FillPolygon(&stripe, q, 4);
    }
    g.Restore(st);
    for (int k = 0; k < 18; ++k) {
        double ph = t * 0.8 + k * 0.618;
        ph -= std::floor(ph);
        double lane = k * 0.381966;
        lane -= std::floor(lane);
        float px = bar.X + bar.Width * (float)lane + (float)std::sin(ph * 6 + k) * ui.S(4);
        float py = bar.Y - (float)ph * ui.S(46);
        float d = ui.S(2.2f) + (k % 3) * ui.S(0.8f);
        SolidBrush p(WithAlpha(k % 2 ? a : b, (BYTE)(220 * (1 - ph))));
        g.FillRectangle(&p, px, py, d, d);
    }
}

std::wstring Leaf(const std::wstring& p) { return p.substr(p.find_last_of(L'\\') + 1); }

constexpr int kSceneId = 0x61000001;  // область вихря: щелчок по нему после итога - искры ещё раз

std::wstring Days(int d) {
    if (d <= 0) return L"сегодня";
    if (d == 1) return L"вчера";
    return Plural(d, L"день", L"дня", L"дней") + L" назад";
}

std::wstring Ago(ULONGLONG at) {
    if (!at) return L"ещё не считалось";
    ULONGLONG s = (GetTickCount64() - at) / 1000;
    if (s < 60) return L"только что";
    if (s < 3600) return Plural((int)(s / 60), L"минуту", L"минуты", L"минут") + L" назад";
    return Plural((int)(s / 3600), L"час", L"часа", L"часов") + L" назад";
}

}  // namespace

// Вихрь очистки (scene::CleanScene) в рамке r: та же сцена, что в карточке, по общей ленте.
void Studio::CleanSceneIn(const RectF& r) {
    const UiColors& c = ui.C();
    scene::Palette p{c.text, c.muted, c.faint, c.accent, c.accent2, c.gold, c.card};
    bool click = ui.Click(kSceneId, r);
    if (cleanScene_.Draw(ui.G(), r, (float)ui.K(), p, ui.Mouse(), click)) ui.Tick();
}

void Studio::GoState(const std::wstring& focus) {
    stateFocus_ = focus;
    stateFocusY_ = -1;
    stateAt_ = 0;
    if (focus == L"downloads") autos::ScanDownloads();
    GoPage(PAGE_STATE);
}

// ---------------- «Состояние» ----------------

void Studio::StatePage(const RectF& r) {
    const UiColors& c = ui.C();
    Graphics& g = ui.G();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    RectF col = Column(in, ui.S(1060));

    // живое: диски, память, тяжёлые программы - не чаще раза в 3 с; здоровье дисков (WMI) - раз за показ
    ULONGLONG now = GetTickCount64();
    if (!stateAt_ || now - stateAt_ > 3000) {
        stateAt_ = now;
        stateVols_ = tools::Volumes();
        stateMem_ = tools::Memory();
        stateApps_ = tools::HeavyApps(4);
        homeAuto_ = AutostartOn();
    }
    if (!statePhysAt_) {
        statePhysAt_ = now;
        LoadPhysAsync();
    }
    if (physJob_ && physJob_->done) {
        AcquireSRWLockShared(&physJob_->lock);
        statePhys_ = physJob_->disks;
        if (toolsPhys_.empty()) toolsPhys_ = physJob_->disks;
        ReleaseSRWLockShared(&physJob_->lock);
    }
    toolsui::State s = toolsui::Snapshot();
    std::vector<tools::DownloadJunk> dls = autos::Downloads();
    bool working = s.scanning || autos::Cleaning() || toolsui::Busy();
    if (working) RepaintIn(250);  // пока считается - изредка перерисовываемся, итог появится сам

    Header(col, L"Состояние",
           L"Всё важное сейчас и что можно сделать в один клик. Подсчёт размеров: " +
               (s.scanning ? std::wstring(L"идёт...") : Ago(s.at)));
    if (ui.Button(ID_STATE_REFRESH, RectF(col.GetRight() - ui.S(132), col.Y + ui.S(4), ui.S(132), ui.S(36)),
                  s.scanning ? L"Считаю" : L"Обновить", Kind::Secondary, L"", s.scanning)) {
        toolsui::Refresh();
        autos::ScanDownloads();
        stateAt_ = statePhysAt_ = 0;
    }

    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState gs = g.Save();
    g.SetClip(view);
    float x = col.X, w = col.Width, gap = ui.S(16), y = view.Y - off;
    int anim = 0;  // номер анимации появления
    auto appear = [&](bool ready) { return ui.Anim(ID_STATE_ANIM + anim++, ready, 5); };

    // ---- шкалы: диски, память, питание ----
    {
        float h = ui.S(132);
        RectF cr(x, y, w, h);
        Card(cr);
        struct G {
            std::wstring big, title, sub;
            float frac;
            bool warn;
            int page;  // куда ведёт клик: 1 - диски в утилитах, -1 - никуда
            std::wstring scan;
        };
        std::vector<G> gauges;
        for (const tools::Volume& v : stateVols_) {
            if (v.type != DRIVE_FIXED || !v.total) continue;
            float used = 1 - (float)v.free / v.total;
            gauges.push_back({std::to_wstring((int)std::lround(used * 100)) + L"%", L"Диск " + v.root.substr(0, 2),
                              L"свободно " + tools::Bytes(v.free) + L"\nиз " + tools::Bytes(v.total), used,
                              tools::DiskWarn(v), 1, v.root});
        }
        if (stateMem_.total)
            gauges.push_back({std::to_wstring(stateMem_.loadPercent) + L"%", L"Память",
                              L"свободно " + tools::Bytes(stateMem_.avail) + L"\nиз " + tools::Bytes(stateMem_.total),
                              stateMem_.loadPercent / 100.0f, stateMem_.loadPercent >= 85, -1, L""});
        float slotW = w / (gauges.size() + 1);  // последняя ячейка - питание
        for (size_t i = 0; i < gauges.size(); ++i) {
            const G& gg = gauges[i];
            RectF slot(x + slotW * i, y, slotW, h);
            float a = appear(true);
            int id = ID_STATE_ANIM + 50 + (int)i;
            bool click = gg.page >= 0 && ui.Click(id, slot);
            float hov = ui.Anim(id + 20, ui.Hot(id), 16);
            float d = ui.S(78), cx = slot.X + ui.S(22) + d / 2, cy = y + h / 2;
            Color arc = gg.warn ? c.warn : MixColor(c.accent, c.gold, gg.frac > 0.8f ? (gg.frac - 0.8f) * 4 : 0);
            Pen track(WithAlpha(c.text, 22), ui.S(9));
            g.DrawEllipse(&track, cx - d / 2, cy - d / 2, d, d);
            Pen p(MixColor(arc, Color(255, 255, 255), hov * 0.2), ui.S(9));
            p.SetStartCap(LineCapRound);
            p.SetEndCap(LineCapRound);
            float sweep = 360 * std::clamp(gg.frac, 0.01f, 1.0f) * (float)EaseOut(a);
            g.DrawArc(&p, cx - d / 2, cy - d / 2, d, d, -90, sweep);
            ui.Text(gg.big, RectF(cx - d / 2, cy - ui.S(12), d, ui.S(24)), c.text, 15, true, ui::Center);
            float tx = cx + d / 2 + ui.S(14), tw = slot.GetRight() - tx - ui.S(8);
            ui.Text(gg.title, RectF(tx, cy - ui.S(30), tw, ui.S(22)), c.text, 13, true);
            ui.Text(gg.sub, RectF(tx, cy - ui.S(6), tw, ui.S(38)), gg.warn ? c.warn : c.muted, 11.5f, false, ui::Left,
                    true);
            if (click) Defer([this, sc = gg.scan] { GoTools(1, sc); });
        }
        // питание: что сейчас и почему
        RectF ps(x + slotW * gauges.size() + ui.S(8), y + ui.S(22), slotW - ui.S(24), h - ui.S(44));
        ui.Fill(ps, c.field, ui.S(12));
        ui.Text(L"Питание", RectF(ps.X + ui.S(14), ps.Y + ui.S(10), ps.Width - ui.S(20), ui.S(20)), c.muted, 11.5f);
        std::wstring pw =
            cfg.autoPower ? autos::PowerStatus() : std::wstring(tools::PowerModeName(tools::GetPowerMode()));
        size_t br = pw.find(L" (");
        ui.Text(br == std::wstring::npos ? pw : pw.substr(0, br),
                RectF(ps.X + ui.S(14), ps.Y + ui.S(30), ps.Width - ui.S(20), ui.S(24)), c.text, 14, true);
        ui.Text(br != std::wstring::npos ? pw.substr(br + 2, pw.size() - br - 3)
                                         : std::wstring(cfg.autoPower ? L"переключаю сам" : L"сам не переключаю"),
                RectF(ps.X + ui.S(14), ps.Y + ui.S(54), ps.Width - ui.S(20), ui.S(20)), c.faint, 11);
        y += h + gap;
    }

    // ---- что можно освободить ----
    {
        ULONGLONG quick = 0, slow = 0, idle = 0, dlBytes = 0, bin = tools::RecycleBinBytes();
        for (const tools::Junk& j : s.junk) (j.slow ? slow : quick) += j.bytes;
        for (const tools::ProjectJunk& p : s.projects)
            if (p.idleDays >= 60) idle += p.bytes;
        for (const tools::DownloadJunk& d : dls) dlBytes += d.bytes;
        struct Seg {
            const wchar_t* name;
            ULONGLONG bytes;
            Color col;
        };
        std::vector<Seg> segs = {{L"Кэши и временные", quick, c.accent},
                                 {L"Давние проекты", idle, MixColor(c.accent, c.gold, 0.55)},
                                 {L"Старое в кэше Unreal", s.ddcOld, c.accent2},
                                 {L"Загрузки", dlBytes, MixColor(c.accent, c.text, 0.45)},
                                 {L"Корзина", bin, c.faint}};
        ULONGLONG total = 0;
        for (const Seg& sg : segs) total += sg.bytes;
        // идёт чистка (или только что кончилась) - карточка вырастает вниз под вихрь
        float grow = ui.Anim(kSceneId + 1, scene::CleanRecent(), 9);
        float sceneH = scene::CleanScene::Height(scene::Clean().items.size(), (float)ui.K());
        float h = ui.S(176) + (sceneH + ui.S(8)) * grow;
        RectF cr(x, y, w, h);
        Card(cr);
        ui.Text(L"Можно освободить", RectF(cr.X + ui.S(20), cr.Y + ui.S(16), ui.S(300), ui.S(22)), c.muted, 12);
        float a = appear(s.at != 0);
        if (s.at) {
            ui.Text(tools::Bytes((ULONGLONG)(total * EaseOut(a))),
                    RectF(cr.X + ui.S(20), cr.Y + ui.S(36), ui.S(320), ui.S(44)), c.text, 28, true);
        } else {
            // пока считается: соты крутятся, а цифры перебираются, как на табло
            ui.HexSpinner(RectF(cr.X + ui.S(18), cr.Y + ui.S(42), ui.S(34), ui.S(34)), c.accent);
            ULONGLONG k = GetTickCount64() / 70;
            unsigned hsh = (unsigned)(k * 2654435761u);
            wchar_t roll[32];
            swprintf(roll, 32, L"%u,%u ГБ", 1 + (hsh >> 8) % 98, (hsh >> 20) % 10);
            ui.Text(roll, RectF(cr.X + ui.S(62), cr.Y + ui.S(36), ui.S(320), ui.S(44)), c.faint, 28, true);
        }
        // полоса из частей
        RectF bar(cr.X + ui.S(20), cr.Y + ui.S(92), cr.Width - ui.S(40), ui.S(14));
        ui.Fill(bar, c.field, bar.Height / 2);
        if (total) {
            GraphicsState bs = g.Save();
            g.SetClip(ui.Round(bar, bar.Height / 2), CombineModeIntersect);
            float bx = bar.X;
            for (const Seg& sg : segs) {
                float sw = bar.Width * (float)((double)sg.bytes / total) * (float)EaseOut(a);
                if (sw < 0.5f) continue;
                ui.Fill(RectF(bx, bar.Y, sw + 0.5f, bar.Height), sg.col);
                bx += sw;
            }
            g.Restore(bs);
        } else if (!s.at) {
            ui.Shimmer(bar, bar.Height / 2, 40);
        }
        if (autos::Cleaning() && !fx::Game() && grow < 0.01f) CleanDust(ui, bar, c.accent, c.gold);
        if (grow > 0.01f) {
            GraphicsState cs = g.Save();
            g.SetClip(cr, CombineModeIntersect);
            CleanSceneIn(RectF(cr.X + ui.S(20), cr.Y + ui.S(170), cr.Width - ui.S(40), sceneH));
            g.Restore(cs);
        }
        // подписи частей
        float lx = bar.X, ly = bar.GetBottom() + ui.S(12);
        for (const Seg& sg : segs) {
            if (!sg.bytes) continue;
            std::wstring t = std::wstring(sg.name) + L" " + tools::Bytes(sg.bytes);
            float tw = ui.Measure(t, 11.5f) + ui.S(26);
            if (lx + tw > bar.GetRight()) break;
            ui.Fill(RectF(lx, ly + ui.S(5), ui.S(10), ui.S(10)), sg.col, ui.S(3));
            ui.Text(t, RectF(lx + ui.S(16), ly, tw, ui.S(20)), c.muted, 11.5f);
            lx += tw + ui.S(10);
        }
        // кнопки справа сверху
        float bx = cr.GetRight() - ui.S(20);
        int lastDays = autos::LastCleanDays();
        std::wstring cleanLabel = autos::Cleaning() ? L"Чищу"
                                  : s.at            ? L"Тихая чистка " + tools::Bytes(quick)
                                                    : std::wstring(L"Тихая чистка");
        float bw = ui.Measure(cleanLabel, 12.5f, true) + ui.S(36);
        if (ui.Button(ID_STATE_CLEAN, RectF(bx - bw, cr.Y + ui.S(20), bw, ui.S(38)), cleanLabel, Kind::Primary, L"",
                      autos::Cleaning()) &&
            !autos::Cleaning())
            autos::CleanNow();
        ui.Tip(ID_STATE_CLEAN, L"Временные файлы старше суток и кэши npm, pip, uv и других; кэши шейдеров не трогаю." +
                                   (lastDays >= 0 ? L" В прошлый раз - " + Days(lastDays) + L", освободила " +
                                                        tools::Bytes(autos::LastCleanFreed()) + L"."
                                                  : std::wstring()));
        bx -= bw + ui.S(10);
        if (s.ddcOld > 256ull * 1024 * 1024) {
            std::wstring t = L"Unreal: старое " + tools::Bytes(s.ddcOld);
            float tw = ui.Measure(t, 12.5f, true) + ui.S(36);
            if (ui.Button(ID_STATE_DDC, RectF(bx - tw, cr.Y + ui.S(20), tw, ui.S(38)), t, Kind::Secondary))
                toolsui::TrimDdc();
            ui.Tip(ID_STATE_DDC,
                   L"Из общего кэша Unreal - только то, что не открывалось 30 дней. Шейдеры текущих "
                   L"проектов остаются.");
        }
        y += h + gap;
    }

    // ---- память: колба (живая сцена, как во вкладке «Процессы» утилит) ----
    {
        float fh = scene::FlaskScene::Height((float)ui.K());
        RectF cr(x, y, w, fh + ui.S(48));
        Card(cr);
        ui.Text(L"Память: кто сколько занимает", RectF(cr.X + ui.S(20), cr.Y + ui.S(14), ui.S(400), ui.S(22)), c.muted,
                12);
        float fw = std::min(cr.Width - ui.S(40), ui.S(760));
        FlaskLive(flaskState_, ID_LIVE_FLASK2, RectF(cr.X + ui.S(20), cr.Y + ui.S(40), fw, fh));
        y += cr.Height + gap;
    }

    bool two = w > ui.S(840);
    float cw = two ? (w - gap) / 2 : w;
    float rowH = ui.S(38);
    // строка с выбором: квадратик, имя, пояснение, размер с полоской
    auto checkRow = [&](int id, const RectF& rr, bool on, const std::wstring& name, const std::wstring& chip,
                        const std::wstring& right, float frac) {
        bool click = ui.Click(id, rr);
        float hov = ui.Anim(id + 5000, ui.Hot(id), 16);
        if (hov > 0) ui.Fill(rr, WithAlpha(c.cardHi, (BYTE)(200 * hov)), ui.S(8));
        RectF box(rr.X + ui.S(10), rr.Y + (rr.Height - ui.S(18)) / 2, ui.S(18), ui.S(18));
        ui.Fill(box, on ? c.accent : c.field, ui.S(5));
        if (!on) ui.Stroke(box, c.border, ui.S(5));
        if (on) {
            Pen ck(c.onAccent, ui.S(2));
            ck.SetStartCap(LineCapRound);
            ck.SetEndCap(LineCapRound);
            PointF pts[3] = {{box.X + ui.S(4), box.Y + ui.S(9.5f)},
                             {box.X + ui.S(7.5f), box.Y + ui.S(13)},
                             {box.X + ui.S(14), box.Y + ui.S(5)}};
            g.DrawLines(&ck, pts, 3);
        }
        float rx = rr.GetRight() - ui.S(10), sizeW = ui.S(76);
        ui.Text(right, RectF(rx - sizeW, rr.Y, sizeW, rr.Height), c.text, 12, true, ui::Right);
        // полоска размера под числом
        RectF sb(rx - sizeW, rr.GetBottom() - ui.S(7), sizeW, ui.S(3));
        ui.Fill(sb, c.field, ui.S(1.5f));
        ui.Fill(RectF(sb.X + sb.Width * (1 - frac), sb.Y, sb.Width * frac, sb.Height), WithAlpha(c.accent, 200),
                ui.S(1.5f));
        float nx = box.GetRight() + ui.S(10), chipW = chip.empty() ? 0 : ui.Measure(chip, 10.5f, true) + ui.S(14);
        float nw = rx - sizeW - ui.S(12) - nx - chipW - ui.S(8);
        ui.Text(name, RectF(nx, rr.Y, nw, rr.Height), c.text, 12.5f);
        if (!chip.empty())
            ui.Chip(RectF(nx + std::min(nw, ui.Measure(name, 12.5f) + ui.S(8)), rr.Y + (rr.Height - ui.S(20)) / 2,
                          chipW, ui.S(20)),
                    chip, WithAlpha(c.accent, 40), c.muted);
        return click;
    };

    // ---- мусор проектов и Загрузки ----
    {
        size_t shown = std::max<size_t>(!s.at                               ? 4
                                        : s.projects.empty() || dls.empty() ? 3
                                                                            : 1,
                                        std::min<size_t>(7, std::max(s.projects.size(), dls.size())));
        float h = ui.S(64) + rowH * shown + ui.S(60);
        RectF pc(x, y, cw, h), dc(two ? x + cw + gap : x, two ? y : y + h + gap, cw, h);
        if (stateFocus_ == L"downloads" && stateFocusY_ < 0) stateFocusY_ = dc.Y + off - view.Y;
        if (stateFocus_ == L"projects" && stateFocusY_ < 0) stateFocusY_ = pc.Y + off - view.Y;

        // проекты
        Card(pc);
        ui.Text(L"Мусор сборки в проектах", RectF(pc.X + ui.S(18), pc.Y + ui.S(14), pc.Width, ui.S(24)), c.text, 14,
                true);
        ui.Text(L"node_modules, Intermediate, target, .venv, build - соберётся заново",
                RectF(pc.X + ui.S(18), pc.Y + ui.S(36), pc.Width - ui.S(36), ui.S(18)), c.faint, 11);
        ULONGLONG maxP = s.projects.empty() ? 1 : s.projects[0].bytes, selBytes = 0;
        for (size_t i = 0; i < s.projects.size() && i < 7; ++i) {
            const tools::ProjectJunk& p = s.projects[i];
            bool on = projSel_.count(p.path) > 0;
            RectF rr(pc.X + ui.S(8), pc.Y + ui.S(60) + rowH * i, pc.Width - ui.S(16), rowH - ui.S(2));
            std::wstring days = p.idleDays >= 60 ? Plural(p.idleDays, L"день", L"дня", L"дней") + L" без правок"
                                                 : (p.idleDays ? std::to_wstring(p.idleDays) + L" дн." : L"сегодня");
            if (checkRow(ID_STATE_PROJ + (int)i, rr, on, p.name + L"\\" + Leaf(p.path), days, tools::Bytes(p.bytes),
                         (float)p.bytes / maxP)) {
                if (on)
                    projSel_.erase(p.path);
                else
                    projSel_.insert(p.path);
            }
            ui.Tip(ID_STATE_PROJ + (int)i, p.kind + L": " + p.path);
        }
        if (s.projects.empty() && s.at)
            ui.EmptyState(ID_STATE_ANIM + 90, RectF(pc.X, pc.Y + ui.S(64), pc.Width, rowH * shown - ui.S(8)),
                          L"Мусора сборки не нашлось: в проектах из колец чисто");
        else if (s.projects.empty())  // ищет: заготовки строк с бликом
            ui.Skeleton(RectF(pc.X + ui.S(8), pc.Y + ui.S(60), pc.Width - ui.S(16), rowH * 4), 4, rowH);
        std::vector<tools::ProjectJunk> pick;
        for (const tools::ProjectJunk& p : s.projects)
            if (projSel_.count(p.path)) pick.push_back(p), selBytes += p.bytes;
        float by = pc.GetBottom() - ui.S(50);
        if (ui.Button(ID_STATE_PROJ_IDLE, RectF(pc.X + ui.S(18), by, ui.S(150), ui.S(34)), L"Выбрать давние",
                      Kind::Ghost)) {
            projSel_.clear();
            for (const tools::ProjectJunk& p : s.projects)
                if (p.idleDays >= 60) projSel_.insert(p.path);
        }
        std::wstring del = pick.empty() ? L"Удалить" : L"Удалить " + tools::Bytes(selBytes);
        float dw = ui.Measure(del, 12.5f, true) + ui.S(36);
        if (ui.Button(ID_STATE_PROJ_DEL, RectF(pc.GetRight() - ui.S(18) - dw, by, dw, ui.S(34)), del,
                      pick.empty() ? Kind::Secondary : Kind::Danger) &&
            !pick.empty()) {
            toolsui::CleanProjectList(pick);
            projSel_.clear();
        }

        // Загрузки
        Card(dc, stateFocus_ == L"downloads");
        ui.Text(L"Лишнее в Загрузках", RectF(dc.X + ui.S(18), dc.Y + ui.S(14), dc.Width, ui.S(24)), c.text, 14, true);
        ui.Text(L"старые установщики, распакованные архивы, копии - в Корзину, вернуть можно",
                RectF(dc.X + ui.S(18), dc.Y + ui.S(36), dc.Width - ui.S(36), ui.S(18)), c.faint, 11);
        ULONGLONG maxD = dls.empty() ? 1 : dls[0].bytes, dlSelBytes = 0;
        for (size_t i = 0; i < dls.size() && i < 7; ++i) {
            const tools::DownloadJunk& d = dls[i];
            bool on = dlSel_.count(d.path) > 0;
            RectF rr(dc.X + ui.S(8), dc.Y + ui.S(60) + rowH * i, dc.Width - ui.S(16), rowH - ui.S(2));
            if (checkRow(ID_STATE_DL + (int)i, rr, on, d.name, d.why, tools::Bytes(d.bytes), (float)d.bytes / maxD)) {
                if (on)
                    dlSel_.erase(d.path);
                else
                    dlSel_.insert(d.path);
            }
            ui.Tip(ID_STATE_DL + (int)i, d.why + L", лежит " + Days(d.days) + L"\n" + d.path);
        }
        if (dls.empty())
            ui.EmptyState(ID_STATE_ANIM + 91, RectF(dc.X, dc.Y + ui.S(64), dc.Width, rowH * shown - ui.S(8)),
                          L"Лишнего не нашлось (или ещё не разбирал - «Обновить»)");
        std::vector<std::wstring> dlPick;
        for (const tools::DownloadJunk& d : dls)
            if (dlSel_.count(d.path)) dlPick.push_back(d.path), dlSelBytes += d.bytes;
        by = dc.GetBottom() - ui.S(50);
        if (ui.Button(ID_STATE_DL_ALL, RectF(dc.X + ui.S(18), by, ui.S(130), ui.S(34)), L"Выбрать всё", Kind::Ghost))
            for (const tools::DownloadJunk& d : dls) dlSel_.insert(d.path);
        std::wstring rec = dlPick.empty() ? L"В Корзину" : L"В Корзину " + tools::Bytes(dlSelBytes);
        float rw = ui.Measure(rec, 12.5f, true) + ui.S(36);
        if (ui.Button(ID_STATE_DL_DEL, RectF(dc.GetRight() - ui.S(18) - rw, by, rw, ui.S(34)), rec,
                      dlPick.empty() ? Kind::Secondary : Kind::Primary) &&
            !dlPick.empty()) {
            autos::RecycleDownloads(dlPick);
            dlSel_.clear();
        }
        y = std::max(pc.GetBottom(), dc.GetBottom()) + gap;
    }

    // ---- рост за неделю и система ----
    {
        float h = ui.S(270);
        RectF gc(x, y, cw, h), sc(two ? x + cw + gap : x, two ? y : y + h + gap, cw, h);
        Card(gc);
        ui.Text(L"Рост за неделю", RectF(gc.X + ui.S(18), gc.Y + ui.S(14), gc.Width, ui.S(24)), c.text, 14, true);
        std::vector<tools::Growth> grow;
        for (const tools::Growth& gr : s.growth)
            if (gr.delta > 16ll * 1024 * 1024 && grow.size() < 6) grow.push_back(gr);
        if (grow.empty()) {
            ui.Text(L"Замеры копятся раз в полчаса: через пару дней здесь будет видно, что растёт быстрее всего.",
                    RectF(gc.X + ui.S(18), gc.Y + ui.S(52), gc.Width - ui.S(36), ui.S(60)), c.muted, 12, false,
                    ui::Left, true);
        } else {
            float a = appear(true);
            LONGLONG maxG = grow[0].delta;
            float lw = gc.Width * 0.38f;
            for (size_t i = 0; i < grow.size(); ++i) {
                float ry = gc.Y + ui.S(54) + i * ui.S(34);
                ui.Text(grow[i].name, RectF(gc.X + ui.S(18), ry, lw - ui.S(8), ui.S(26)), c.text, 12);
                RectF bar(gc.X + ui.S(18) + lw, ry + ui.S(7), gc.Width - lw - ui.S(110), ui.S(12));
                ui.Fill(bar, c.field, ui.S(6));
                ui.Fill(RectF(bar.X, bar.Y, bar.Width * (float)grow[i].delta / maxG * (float)EaseOut(a), bar.Height),
                        i == 0 ? c.accent : WithAlpha(c.accent, 170), ui.S(6));
                ui.Text(L"+" + tools::Bytes((ULONGLONG)grow[i].delta),
                        RectF(bar.GetRight() + ui.S(8), ry, ui.S(80), ui.S(26)), c.muted, 11.5f, true);
            }
        }

        // система
        Card(sc);
        ui.Text(L"Система", RectF(sc.X + ui.S(18), sc.Y + ui.S(14), sc.Width, ui.S(24)), c.text, 14, true);
        float ry = sc.Y + ui.S(48), lineH = ui.S(42), lw = ui.S(118);
        auto line = [&](const std::wstring& label, const std::wstring& value, bool warn) {
            ui.Text(label, RectF(sc.X + ui.S(18), ry, lw, lineH), c.muted, 11.5f);
            ui.Text(value, RectF(sc.X + ui.S(18) + lw, ry, sc.Width - lw - ui.S(150), lineH), warn ? c.warn : c.text,
                    12, false, ui::Left, true);
            RectF btn(sc.GetRight() - ui.S(148), ry + ui.S(5), ui.S(130), ui.S(32));
            ry += lineH;
            return btn;
        };
        // драйвер
        {
            std::wstring v = !s.driver.ok     ? L"проверю в течение дня"
                             : s.driver.newer ? s.driver.installed + L" -> вышел " + s.driver.latest
                                              : s.driver.installed + L", свежий";
            RectF b = line(L"Драйвер NVIDIA", v, s.driver.newer);
            if (s.driver.newer && ui.Button(ID_STATE_DRIVER, b, L"NVIDIA App", Kind::Secondary)) {
                std::wstring app = L"C:\\Program Files\\NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA App.exe";
                ShellExecuteW(nullptr, L"open",
                              GetFileAttributesW(app.c_str()) != INVALID_FILE_ATTRIBUTES
                                  ? app.c_str()
                                  : L"https://www.nvidia.com/drivers/",
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        // диски
        {
            std::wstring v;
            bool warn = false;
            for (const tools::Physical& p : statePhys_) {
                std::wstring n = p.name.size() > 18 ? p.name.substr(0, 17) + L"…" : p.name;
                v += (v.empty() ? L"" : L"\n") + n + L": " + (p.health == 0 ? L"исправен" : L"проверь!");
                warn = warn || p.health != 0;
            }
            line(L"Диски", v.empty() ? L"нет данных" : v, warn);
            if (statePhys_.size() > 1) ry += ui.S(10);
        }
        // автозапуск
        {
            RectF b = line(L"Автозапуск", homeAuto_ ? L"включён" : L"выключен", !homeAuto_);
            if (!homeAuto_ && ui.Button(ID_STATE_AUTOSTART, b, L"Включить", Kind::Primary)) {
                SetAutostart(true);
                stateAt_ = 0;
            }
        }
        // тяжелее всего сейчас
        {
            std::wstring v;
            for (size_t i = 0; i < stateApps_.size() && i < 3; ++i) {
                std::wstring n = stateApps_[i].name;
                if (EndsWithI(n, L".exe")) n = n.substr(0, n.size() - 4);
                v += (v.empty() ? L"" : L", ") + n + L" " + tools::Bytes(stateApps_[i].bytes);
            }
            RectF b = line(L"Тяжелее всего", v.empty() ? L"-" : v, false);
            if (!stateApps_.empty()) {
                std::wstring n = stateApps_[0].name;
                if (EndsWithI(n, L".exe")) n = n.substr(0, n.size() - 4);
                if (n.size() > 12) n = n.substr(0, 11) + L"…";
                if (ui.Button(ID_STATE_APP, b, L"Закрыть " + n, Kind::Ghost)) toolsui::CloseHeavy(stateApps_[0]);
                ui.Tip(ID_STATE_APP, L"Сначала попрошу закрыться (окна сохранят своё), через 5 с - силой");
            }
        }
        if (ui.Button(ID_STATE_GO_AUTO,
                      RectF(sc.X + ui.S(18), sc.GetBottom() - ui.S(46), sc.Width - ui.S(36), ui.S(32)),
                      L"Что программа делает сама  ->", Kind::Ghost))
            Defer([this] { GoPage(PAGE_AUTO); });
        y = std::max(gc.GetBottom(), sc.GetBottom()) + gap;
    }

    pageContentH_ = y + off - view.Y;
    g.Restore(gs);
    // переход из карточки «Загрузки - Показать»: прокрутить к нужной карточке один раз
    if (stateFocusY_ >= 0 && !stateFocus_.empty()) {
        float want = std::max(0.0f, stateFocusY_ - ui.S(8));
        stateFocus_.clear();
        Defer([this, want] {
            ui.ScrollTo(ID_PAGE_SCROLL, want);
            ui.Invalidate();
        });
    }
}

// ---------------- «Автоматика» ----------------

void Studio::AutoPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Автоматика",
           L"Что программа делает сама, чтобы не приходилось лезть и искать. Всё можно выключить.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));
    auto toggle = [&](const RectF& rr, int id, bool& v) {
        bool val = v;
        if (ui.Toggle(id, RectF(rr.X + rr.Width - ui.S(42), rr.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };
    if (autos::Cleaning() || toolsui::Busy()) RepaintIn(250);

    cols.Card(L"Тихая чистка", 2.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Раз в неделю и когда на C: мало места");
        toggle(rr, ID_AUTO_CLEAN, cfg.autoClean);
        int d = autos::LastCleanDays();
        rr = cols.Row(
            b, 1,
            d < 0 ? L"Ещё не было" : L"Была " + Days(d) + L", освободила " + tools::Bytes(autos::LastCleanFreed()));
        if (ui.Button(ID_AUTO_CLEAN_NOW, RectF(rr.GetRight() - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      autos::Cleaning() ? L"Чищу" : L"Почистить сейчас", Kind::Secondary, L"", autos::Cleaning()) &&
            !autos::Cleaning())
            autos::CleanNow();
        cols.Note(
            b, 2.0f,
            L"Временные файлы старше суток, кэши npm, pip, uv, Yarn, NuGet, Go, Gradle, Cargo, дампы. Кэши шейдеров "
            L"и Unreal не трогаю. В игре не запускается; итог - короткой карточкой, мелочь - только в журнал.",
            c.faint);
    });
    cols.Card(L"Разбор Загрузок", 3.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Раз в неделю спрашивать, что убрать");
        toggle(rr, ID_AUTO_DL, cfg.autoDownloads);
        rr = cols.Row(b, 1, L"Установщики и архивы старше " + Plural(cfg.downloadsDays, L"дня", L"дней", L"дней"),
                      0.62f);
        int days = cfg.downloadsDays;
        if (ui.Slider(ID_AUTO_DL_DAYS, RectF(rr.GetRight() - ui.S(170), rr.Y + ui.S(8), ui.S(170), ui.S(28)), days, 3,
                      60)) {
            Change();
            cfg.downloadsDays = days;
            Save();
        }
        rr = cols.Row(b, 2, L"Найдено сейчас: " + std::to_wstring(autos::Downloads().size()));
        if (ui.Button(ID_AUTO_DL_NOW, RectF(rr.GetRight() - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Показать", Kind::Secondary))
            Defer([this] { GoState(L"downloads"); });
        cols.Note(b, 3.0f,
                  L"Старые установщики, архивы, которые уже распакованы рядом, копии «файл (1)», недокачанное. "
                  L"Ничего не удаляется без вопроса и всё уходит в Корзину.",
                  c.faint);
    });
    cols.Card(L"Режим питания", 2.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Переключать сам");
        bool was = cfg.autoPower;
        toggle(rr, ID_AUTO_POWER, cfg.autoPower);
        if (was != cfg.autoPower) autos::OnPower();
        cols.Row(b, 1, L"Сейчас: " + autos::PowerStatus(), 0.95f);
        cols.Note(
            b, 2.0f,
            L"От батареи - экономия энергии, на зарядке - сбалансированный, в игре и Unreal на зарядке - "
            L"максимальная производительность (от батареи - сбалансированный). Тот же ползунок, что в Параметрах.",
            c.faint);
    });
    cols.Card(L"Самовосстановление", 2.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Проверять раз в 15 минут");
        toggle(rr, ID_AUTO_REPAIR, cfg.autoRepair);
        cols.Row(b, 1, L"Последняя проверка: " + autos::RepairStatus(), 0.95f);
        cols.Note(b, 2.0f,
                  L"Пропал автозапуск (антивирус, чистильщик) - верну, если он был включён. Пропал или обнулился "
                  L"radial.cfg - достану последнюю копию. Пропал RadialTools.exe - скажу (пересоздать его нельзя). "
                  L"Антивирус не обхожу.",
                  c.faint);
    });
    std::vector<autos::Entry> journal = autos::Journal(40);
    float rows = std::max(1.0f, journal.size() * 0.62f);
    cols.Wide(L"Журнал автоматики", rows, [&](const RectF& b) {
        if (journal.empty()) {
            ui.Text(L"Пока пусто: здесь появится всё, что программа сделала сама.", RectF(b.X, b.Y, b.Width, ui.S(24)),
                    c.muted, 12);
            return;
        }
        float lh = cols.RowH() * 0.62f;
        for (size_t i = 0; i < journal.size(); ++i) {
            float yy = b.Y + i * lh;
            if (i % 2 == 0) ui.Fill(RectF(b.X - ui.S(8), yy, b.Width + ui.S(16), lh), WithAlpha(c.cardHi, 90), ui.S(6));
            ui.Text(journal[i].when, RectF(b.X, yy, ui.S(96), lh), c.faint, 11.5f);
            ui.Text(journal[i].text, RectF(b.X + ui.S(100), yy, b.Width - ui.S(100), lh), c.text, 12);
        }
    });
    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
