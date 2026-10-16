// Раздел «Утилиты»: PATH и переменные, диски (место, здоровье, что занимает, кэши), автозагрузка, порты и блокировки.
// Долгое (сканирование, очистка, задача с правами) - в своём потоке, страница только читает результат.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "core/fx.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/hex.h"
#include "net/net.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

DWORD WINAPI ToolsThread(void* p) {
    auto* holder = (std::shared_ptr<ToolsJob>*)p;
    std::shared_ptr<ToolsJob> job = *holder;
    delete holder;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    tools::Log log = [job](const std::wstring& s) {
        AcquireSRWLockExclusive(&job->lock);
        job->log.push_back(s);
        ReleaseSRWLockExclusive(&job->lock);
        if (job->wnd) InvalidateRect(job->wnd, nullptr, FALSE);
    };
    bool ok = job->work(log, *job);
    AcquireSRWLockExclusive(&job->lock);
    job->ok = ok;
    job->done = true;
    ReleaseSRWLockExclusive(&job->lock);
    if (job->wnd) InvalidateRect(job->wnd, nullptr, FALSE);
    CoUninitialize();
    return 0;
}

std::wstring Ellip(const std::wstring& s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + L"…" : s; }

// В кольце места - столько крупных папок, остальные одним серым куском.
constexpr size_t kRingParts = 12;
// Цвет доли: свои цвета темы по кругу, вторая шестёрка - те же, но глуше.
Color SizeColor(const UiColors& c, int i, int n) {
    if (i >= (int)kRingParts) return c.faint;
    (void)n;
    Color base[6] = {c.accent,
                     c.gold,
                     c.accent2,
                     MixColor(c.accent, c.gold, 0.5),
                     MixColor(c.accent2, c.accent, 0.5),
                     MixColor(c.gold, c.text, 0.45)};
    Color col = base[i % 6];
    return i >= 6 ? MixColor(col, c.card, 0.35) : col;
}

}  // namespace

// Соты места: улей, разрезанный на доли по кругу, как круговая диаграмма. Доли дорастают плавно (список растёт
// по ходу подсчёта) - цвет перетекает по сотам; наведённая доля приподнимается, остальные гаснут. Щелчок - зайти
// внутрь: старые соты разлетаются, доля втягивается в середину, и улей собирается заново. hot - строка под
// курсором в списке справа (её доля подсвечивается); возвращает долю, по которой щёлкнули, или -1.
int Studio::SizeRing(const RectF& r, const std::wstring& dir, const std::vector<tools::FolderSize>& sizes,
                     bool scanning, long seen, int& hot) {
    const UiColors& c = ui.C();
    Graphics& g = ui.G();
    float side = std::min(r.Width, r.Height);
    float cx = r.X + side / 2, cy = r.Y + side / 2;
    constexpr int kR = 6;  // радиус улья в сотах; середина (до 2) пустая - там подпись
    const float s = side / (1.7320508f * (2 * kR + 1) + 0.5f);
    ULONGLONG now = GetTickCount64();
    bool game = fx::Game();
    if (dir != ringDir_) {
        ringDir_ = dir;
        ringShown_.clear();
        hiveGhosts_.clear();
        if (!game)
            for (const HiveCell& h : hive_) {
                if (h.s < 0.05f) continue;
                bool in = h.part >= 0 && h.part == hiveZoom_;
                float dx = h.x - cx, dy = h.y - cy, d = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
                float v = in ? 0 : ui.S(260) + d * 2.5f;
                hiveGhosts_.push_back({h.x, h.y, dx / d * v, dy / d * v, h.col, in});
            }
        hiveGone_ = now;
        hiveBorn_ = now + (hiveGhosts_.empty() ? 0 : 220);
        hive_.clear();
        hiveZoom_ = -1;
    }
    float dt = ringAt_ && now - ringAt_ < 200 ? (now - ringAt_) / 1000.0f : 0.016f;
    ringAt_ = now;
    dt *= (float)(1000.0 / std::max(250.0, fx::Dur(1000)));
    ULONGLONG total = 0;
    for (const tools::FolderSize& f : sizes) total += f.bytes;
    size_t n = std::min(sizes.size(), kRingParts);
    std::vector<float> part(n + 1, 0.0f);
    bool moving = false;
    for (size_t i = 0; i <= n; ++i) {
        ULONGLONG bytes = 0;
        if (i < n)
            bytes = sizes[i].bytes;
        else
            for (size_t k = n; k < sizes.size(); ++k) bytes += sizes[k].bytes;
        float target = total ? (float)((double)bytes / total) : 0;
        float& s = ringShown_[i < n ? sizes[i].path : L"*"];
        s += (target - s) * std::min(1.0f, dt * 7 / (float)std::max(0.3, fx::Dur(1)));
        if (fx::Game() || std::fabs(target - s) < 0.0015f)
            s = target;
        else
            moving = true;
        part[i] = s;
    }
    // соты: середина и угол от верха по часовой стрелке (по нему - доля, как в круговой диаграмме)
    struct Spot {
        float x, y, a;
        int d;
    };
    static std::vector<Spot> spots;  // одинаковы для всех вызовов при том же kR, считаются в единицах соты
    static std::vector<int> order;
    if (spots.empty()) {
        for (int q = -kR; q <= kR; ++q)
            for (int rr = std::max(-kR, -q - kR); rr <= std::min(kR, -q + kR); ++rr) {
                int d = std::max({std::abs(q), std::abs(rr), std::abs(q + rr)});
                if (d <= 2) continue;
                hex::Cell hc = hex::Center(q, rr, 1);
                float a = std::fmod((float)std::atan2(hc.y, hc.x) * 57.29578f + 90 + 720, 360.0f);
                spots.push_back({(float)hc.x, (float)hc.y, a, d});
            }
        for (size_t i = 0; i < spots.size(); ++i) order.push_back((int)i);
        std::sort(order.begin(), order.end(), [](int a, int b) {
            return spots[a].a != spots[b].a ? spots[a].a < spots[b].a : spots[a].d < spots[b].d;
        });
    }
    if (hive_.size() != spots.size()) {
        hive_.assign(spots.size(), HiveCell{});
        for (HiveCell& h : hive_) h.col = c.field;
    }
    // какая доля у какой соты: по порядку угла, сколько сот - по размеру доли
    std::vector<int> partOf(spots.size(), -1);
    {
        double acc = 0;
        size_t k = 0;
        for (size_t i = 0; i <= n; ++i) {
            acc += part[i];
            size_t until = std::min(spots.size(), (size_t)std::lround(acc * spots.size()));
            for (; k < until; ++k) partOf[order[k]] = (int)i;
        }
    }
    // попадание: ближайшая сота
    RectF box(cx - side / 2, cy - side / 2, side, side);
    bool click = ui.Click(ID_TOOLS_RING, box);
    POINT m = ui.Mouse();
    int under = -1;
    if (ui.Hot(ID_TOOLS_RING))
        for (size_t i = 0; i < spots.size(); ++i) {
            float dx = m.x - (cx + spots[i].x * s), dy = m.y - (cy + spots[i].y * s);
            if (dx * dx + dy * dy < s * s * 0.8f) under = partOf[i];
        }
    if (under >= 0) hot = under < (int)n ? under : -2;
    int lit = hot >= 0 ? hot : hot == -2 ? (int)n : -1;
    // пружины и цвета
    float born = now > hiveBorn_ ? (now - hiveBorn_) / 1000.0f : -1;
    if (game) born = 100;
    float tblend = std::min(1.0f, dt * 9);
    std::vector<PointF> pts(6);
    for (size_t i = 0; i < spots.size(); ++i) {
        HiveCell& h = hive_[i];
        const Spot& sp = spots[i];
        float delay = 0.035f * sp.d + sp.a / 360 * 0.22f;
        float target = born >= delay ? 1.0f : 0.0f;
        if (game) {
            h.s = target, h.vs = 0;
        } else {
            h.vs += ((target - h.s) * 170 - h.vs * 13) * dt;
            h.s += h.vs * dt;
            if (std::fabs(target - h.s) > 0.002f || std::fabs(h.vs) > 0.01f) moving = true;
        }
        int pi = partOf[i];
        Color want;
        if (pi < 0) {
            // доли ещё нет: пока считается - по сотам бежит волна от середины
            float wave = scanning && !game ? 0.5f + 0.5f * std::sin(now / 160.0f - sp.d * 0.9f) : 0;
            want = MixColor(c.field, c.accent, 0.18 * wave);
            if (scanning) moving = true;
        } else {
            want = pi < (int)n ? SizeColor(c, pi, (int)n) : c.faint;
        }
        Color old = h.col;
        h.col = game ? want : MixColor(h.col, want, tblend);
        if (old.GetValue() != h.col.GetValue()) moving = true;
        h.part = pi;
        float lt = pi >= 0 && pi == lit ? 1.0f : 0.0f;
        if (std::fabs(lt - h.lift) > 0.005f) {
            h.lift += (lt - h.lift) * std::min(1.0f, dt * 14);
            moving = true;
        } else {
            h.lift = lt;
        }
        // наведённая доля выдвигается от середины по своему направлению
        float ox = 0, oy = 0;
        if (h.lift > 0) {
            float len = std::sqrt(sp.x * sp.x + sp.y * sp.y);
            ox = sp.x / len * ui.S(4) * h.lift;
            oy = sp.y / len * ui.S(4) * h.lift;
        }
        h.x = cx + sp.x * s + ox;
        h.y = cy + sp.y * s + oy;
        float sc = s * 0.9f * h.s * (1 + 0.1f * h.lift);
        if (sc < 0.3f) continue;
        Color col = h.col;
        if (lit >= 0 && pi != lit) col = MixColor(col, c.card, 0.55);
        if (h.lift > 0) col = MixColor(col, Color(255, 255, 255), 0.16 * h.lift);
        for (int k = 0; k < 6; ++k) {
            float a = k * 1.0471976f;
            pts[k] = PointF(h.x + sc * std::cos(a), h.y + sc * std::sin(a));
        }
        SolidBrush b(col);
        g.FillPolygon(&b, pts.data(), 6);
    }
    // соты прошлой папки
    if (!hiveGhosts_.empty()) {
        float gt = (now - hiveGone_) / 1000.0f;
        float fade = 1 - std::min(1.0f, gt / 0.45f);
        if (fade <= 0) {
            hiveGhosts_.clear();
        } else {
            moving = true;
            for (HiveGhost& gh : hiveGhosts_) {
                float sc = s * 0.9f;
                if (gh.in) {  // втягивается в середину и растворяется
                    gh.x += (cx - gh.x) * std::min(1.0f, dt * 9);
                    gh.y += (cy - gh.y) * std::min(1.0f, dt * 9);
                    sc *= fade;
                } else {
                    gh.x += gh.vx * dt;
                    gh.y += gh.vy * dt;
                    sc *= 0.5f + 0.5f * fade;
                }
                for (int k = 0; k < 6; ++k) {
                    float a = k * 1.0471976f;
                    pts[k] = PointF(gh.x + sc * std::cos(a), gh.y + sc * std::sin(a));
                }
                SolidBrush b(WithAlpha(gh.col, (BYTE)(255 * fade)));
                g.FillPolygon(&b, pts.data(), 6);
            }
        }
    }
    if (moving) ui.Tick();
    if (click && under >= 0 && under < (int)n) hiveZoom_ = under;
    // середина
    float ri = s * 1.7320508f * 2.4f;
    float tw = ri * 1.8f;
    RectF mid1(cx - tw / 2, cy - ui.S(22), tw, ui.S(22)), mid2(cx - tw / 2, cy + ui.S(1), tw, ui.S(20));
    if (hot >= 0 && hot < (int)n) {
        const tools::FolderSize& f = sizes[hot];
        ui.Text(f.name, mid1, c.text, 12.5f, true, ui::Center);
        int pct = total ? (int)std::lround(100.0 * f.bytes / total) : 0;
        ui.Text(tools::Bytes(f.bytes) + L", " + std::to_wstring(pct) + L"%", mid2, c.muted, 11.5f, false, ui::Center);
    } else if (hot == -2) {
        ui.Text(L"Остальное", mid1, c.text, 12.5f, true, ui::Center);
        ui.Text(Plural((int)(sizes.size() - n), L"папка", L"папки", L"папок"), mid2, c.muted, 11.5f, false, ui::Center);
    } else if (scanning) {
        ui.HexSpinner(RectF(cx - ui.S(15), cy - ui.S(34), ui.S(30), ui.S(30)), c.accent);
        ui.Text(tools::Bytes(total), RectF(cx - tw / 2, cy - ui.S(2), tw, ui.S(20)), c.text, 12.5f, true, ui::Center);
        ui.Text(L"файлов: " + std::to_wstring(seen), RectF(cx - tw / 2, cy + ui.S(17), tw, ui.S(18)), c.faint, 10.5f,
                false, ui::Center);
    } else {
        ui.Text(tools::Bytes(total), mid1, c.text, 15, true, ui::Center);
        std::wstring leaf = dir;
        while (leaf.size() > 3 && leaf.back() == L'\\') leaf.pop_back();
        size_t cut = leaf.find_last_of(L'\\');
        if (cut != std::wstring::npos && leaf.size() > 3) leaf = leaf.substr(cut + 1);
        ui.Text(leaf, mid2, c.muted, 11.5f, false, ui::Center);
    }
    if (under >= 0 && under < (int)n) ui.Tip(ID_TOOLS_RING, L"Щелчок - зайти в " + sizes[under].name);
    return click && under >= 0 && under < (int)n ? under : -1;
}

void Studio::ToolsRun(const std::wstring& title, std::function<bool(const tools::Log&, ToolsJob&)> work) {
    if (toolsJob_ && !toolsJob_->done) return;
    auto job = std::make_shared<ToolsJob>();
    job->title = title;
    job->work = std::move(work);
    job->wnd = wnd;
    toolsJob_ = job;
    auto* holder = new std::shared_ptr<ToolsJob>(job);
    if (HANDLE th = CreateThread(nullptr, 0, ToolsThread, holder, 0, nullptr))
        CloseHandle(th);
    else
        delete holder;
}

void Studio::ToolsScan(const std::wstring& dir) {
    if (toolsJob_ && !toolsJob_->done) toolsJob_->cancel = true;
    toolsScanPath_ = dir;
    std::wstring d = dir;
    toolsJob_.reset();
    ToolsRun(L"Что занимает место", [d](const tools::Log& log, ToolsJob& job) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // обход диска не мешает работе
        log(L"Считаю " + d + L"...");
        std::vector<tools::FolderSize> r =
            tools::ScanSizes(d, &job.cancel, &job.progress, [&job](const std::vector<tools::FolderSize>& sofar) {
                AcquireSRWLockExclusive(&job.lock);  // список растёт по мере подсчёта
                job.sizes = sofar;
                ReleaseSRWLockExclusive(&job.lock);
            });
        AcquireSRWLockExclusive(&job.lock);
        job.sizes = std::move(r);
        job.hasSizes = true;
        ReleaseSRWLockExclusive(&job.lock);
        return !job.cancel;
    });
}

void Studio::GoTools(int tab, const std::wstring& scan) {
    toolsTab_ = std::clamp(tab, 0, 3);
    toolsAt_ = 0;
    GoPage(PAGE_TOOLS);
    if (!scan.empty()) ToolsScan(scan);
}

void Studio::ToolsPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Утилиты",
           L"PATH и переменные, диски, автозагрузка, порты. Быстрые действия - в кольце утилит на углу.");
    RectF tabs(Column(in, ui.S(900)).X, in.Y + ui.S(58), std::min(in.Width, ui.S(520)), ui.S(38));
    int tab = toolsTab_;
    if (ui.Segmented(ID_TOOLS_TAB, tabs, {L"PATH", L"Диски", L"Автозагрузка", L"Порты"}, tab)) {
        toolsTab_ = tab;
        toolsAt_ = 0;
        ui.ResetScroll(ID_PAGE_SCROLL);
    }
    RectF view(in.X - ui.S(4), in.Y + ui.S(108), in.Width + ui.S(8), in.Height - ui.S(108));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));
    float rowH = cols.RowH();
    auto card = [&](const wchar_t* title, float rows, auto&& body) { cols.Card(title, rows, body); };
    auto wide = [&](const wchar_t* title, float rows, auto&& body) { cols.Wide(title, rows, body); };
    // строка списка: подсветка при наведении, по клику - действие (если есть)
    auto rowHover = [&](int id, const RectF& rr) {
        bool click = ui.Click(id, rr);
        if (ui.Hot(id)) ui.Fill(rr, WithAlpha(c.cardHi, 200), ui.S(6));
        return click;
    };
    // курсор над строкой: кнопки строки показываются только тогда, иначе список - лес одинаковых кнопок
    auto rowHot = [&](const RectF& rr) {
        POINT m = ui.Mouse();
        return ui.interactive && m.x >= rr.X && m.x < rr.X + rr.Width && m.y >= rr.Y && m.y < rr.Y + rr.Height;
    };
    auto note = [&](const RectF& b, float n, const std::wstring& s, Color col) { cols.Note(b, n, s, col); };
    float lineH = ui.S(30);
    auto rowsFor = [&](size_t n) { return (float)(n * lineH / rowH); };

    bool busy = toolsJob_ && !toolsJob_->done;
    if (busy) RepaintIn(120);  // ход работы - несколько раз в секунду, не каждый кадр
    // данные вкладки - при входе и раз в несколько секунд (порты чаще)
    ULONGLONG now = GetTickCount64();
    ULONGLONG every = toolsTab_ == 3 ? 3000 : 15000;
    if (!toolsAt_ || now - toolsAt_ > every) {
        toolsAt_ = now;
        if (toolsTab_ == 0) {
            toolsPath_ = tools::ReadPath();
            toolsWhich_ = tools::WhichAll();
            toolsEnv_ = tools::ReadEnv();
        } else if (toolsTab_ == 1) {
            toolsVols_ = tools::Volumes();
            if (toolsPhys_.empty()) LoadPhysAsync();
            // размеры кэшей - сами, один раз при входе на вкладку (считаются в фоне)
            if (toolsJunk_.empty() && !toolsJunkAsked_ && !(toolsJob_ && !toolsJob_->done)) {
                toolsJunkAsked_ = true;
                ToolsRun(L"Кэши", [](const tools::Log& l, ToolsJob& job) {
                    l(L"Считаю размеры кэшей...");
                    std::vector<tools::Junk> j = tools::FindJunk(true);
                    AcquireSRWLockExclusive(&job.lock);
                    job.junk = j;
                    job.hasJunk = true;
                    ReleaseSRWLockExclusive(&job.lock);
                    return true;
                });
            }
        } else if (toolsTab_ == 2) {
            toolsStarts_ = tools::StartupItems();
        } else {
            toolsPorts_ = tools::Listeners();
        }
    }
    auto log = [&]() -> std::wstring {
        if (!toolsJob_) return L"";
        AcquireSRWLockShared(&toolsJob_->lock);
        std::wstring s =
            toolsJob_->title + (toolsJob_->done ? (toolsJob_->ok ? L": готово" : L": не получилось") : L"...");
        for (size_t i = toolsJob_->log.size() > 5 ? toolsJob_->log.size() - 5 : 0; i < toolsJob_->log.size(); ++i)
            s += L"\n" + toolsJob_->log[i];
        ReleaseSRWLockShared(&toolsJob_->lock);
        return s;
    };
    auto refresh = [&] { toolsAt_ = 0; };

    if (toolsTab_ == 0) {  // ---------------- PATH ----------------
        int ndead = 0, ndup = 0;
        for (const tools::PathEntry& e : toolsPath_) {
            ndead += e.dead;
            ndup += e.dup && !e.dead;
        }
        size_t nsys = 0, sysBad = 0;
        for (const tools::PathEntry& e : toolsPath_)
            if (e.system) {
                ++nsys;
                sysBad += e.dead || e.dup;
            }
        size_t visible = toolsPath_.size() - (toolsShowSys_ ? 0 : nsys) + 1;
        wide(L"PATH", rowsFor(visible) + 3.2f, [&](const RectF& b) {
            std::vector<std::wstring> user;
            for (const tools::PathEntry& e : toolsPath_)
                if (!e.system) user.push_back(e.raw);
            float y = b.Y;
            int ui_ = 0;
            // сначала свой PATH (его и правят), системный свёрнут: он длинный, а трогать его почти не нужно
            std::vector<size_t> order;
            for (size_t i = 0; i < toolsPath_.size(); ++i)
                if (!toolsPath_[i].system) order.push_back(i);
            order.push_back(SIZE_MAX);  // строка «системный PATH»
            if (toolsShowSys_)
                for (size_t i = 0; i < toolsPath_.size(); ++i)
                    if (toolsPath_[i].system) order.push_back(i);
            // кто кого затеняет: команда есть в двух папках - побеждает та, что выше
            struct Link {
                size_t win, lose;
                std::wstring cmd;
            };
            std::vector<Link> links;
            {
                auto norm = [](std::wstring s) {
                    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
                    CharLowerBuffW(s.data(), (DWORD)s.size());
                    return s;
                };
                auto dirOf = [&](const std::wstring& file) {
                    size_t cut = file.find_last_of(L'\\');
                    return norm(cut == std::wstring::npos ? file : file.substr(0, cut));
                };
                std::vector<std::wstring> dirs;
                for (const tools::PathEntry& e : toolsPath_) dirs.push_back(norm(e.full));
                auto find = [&](const std::wstring& d) {
                    for (size_t j = 0; j < dirs.size(); ++j)
                        if (dirs[j] == d) return j;
                    return SIZE_MAX;
                };
                for (const tools::Which& w : toolsWhich_) {
                    size_t wi = find(dirOf(w.path));
                    if (wi == SIZE_MAX) continue;
                    for (const std::wstring& o : w.other) {
                        size_t li = find(dirOf(o));
                        if (li != SIZE_MAX && li != wi) links.push_back({wi, li, w.cmd});
                    }
                }
            }
            // свои строки можно перетаскивать за левый край; остальные расступаются
            size_t nUser = user.size();
            float userTop = y;
            POINT m = ui.Mouse();
            if (pathDrag_ >= (int)nUser) pathDrag_ = -1;
            if (pathDrag_ >= 0) {
                float at = m.y - pathGrab_ - userTop;
                pathDragTo_ = std::clamp((int)std::lround(at / lineH), 0, (int)nUser - 1);
            }
            std::vector<float> rowY(toolsPath_.size(), -1);
            size_t hoverRow = SIZE_MAX;
            int dragRow = -1;
            for (size_t i : order) {
                if (i == SIZE_MAX) {
                    RectF rr(b.X - ui.S(6), y, b.Width + ui.S(12), lineH);
                    if (rowHover(ID_TOOLS_SYS_TOGGLE, rr)) toolsShowSys_ = !toolsShowSys_;
                    ui.Text(std::wstring(toolsShowSys_ ? L"Скрыть" : L"Показать") + L" системный PATH (" +
                                std::to_wstring(nsys) + L")" +
                                (sysBad ? L", лишних " + std::to_wstring(sysBad) : std::wstring()) +
                                L" - Windows ищет в нём раньше, чем в своём",
                            RectF(b.X, y, b.Width, lineH), sysBad ? c.gold : c.accent, 11.5f, true);
                    y += lineH;
                    continue;
                }
                const tools::PathEntry& e = toolsPath_[i];
                int kk = e.system ? -1 : ui_++;
                float ry = y;
                if (kk >= 0) {
                    // место строки с учётом перетаскиваемой, доезжает пружиной
                    int slot = kk;
                    if (pathDrag_ >= 0 && kk != pathDrag_) {
                        if (pathDrag_ < pathDragTo_ && kk > pathDrag_ && kk <= pathDragTo_) slot = kk - 1;
                        if (pathDragTo_ < pathDrag_ && kk >= pathDragTo_ && kk < pathDrag_) slot = kk + 1;
                    }
                    ry = userTop + ui.Glide(-2000 - kk, RectF(0, slot * lineH, 1, 1)).Y;
                    if (kk == pathDrag_) {
                        dragRow = (int)i;
                        rowY[i] = m.y - pathGrab_;
                        y += lineH;
                        continue;  // рисуется последней, поверх остальных
                    }
                }
                rowY[i] = ry;
                RectF rowR(b.X - ui.S(6), ry, b.Width + ui.S(12), lineH);
                bool hot = pathDrag_ < 0 && rowHot(rowR);
                if (hot) hoverRow = i;
                y += lineH;
            }
            // дуги затенения для строки под курсором: от победителя к проигравшему, по ним бегут штрихи
            auto rowCenter = [&](size_t i) { return rowY[i] + lineH / 2; };
            bool anyLink = false;
            for (const Link& l : links) {
                bool on = hoverRow != SIZE_MAX && (l.win == hoverRow || l.lose == hoverRow);
                if (!on || rowY[l.win] < 0 || rowY[l.lose] < 0) continue;
                anyLink = true;
                float y1 = rowCenter(l.win), y2 = rowCenter(l.lose);
                float bend = ui.S(10) + std::min(ui.S(10), std::fabs(y2 - y1) * 0.04f);
                Pen arc(c.gold, ui.S(1.8f));
                arc.SetDashStyle(DashStyleDash);
                arc.SetDashOffset(-(float)std::fmod(GetTickCount64() / std::max(1.0, fx::Dur(60)), 1000.0));
                arc.SetEndCap(LineCapArrowAnchor);
                ui.G().DrawBezier(&arc, PointF(b.X - ui.S(2), y1), PointF(b.X - bend - ui.S(4), y1),
                             PointF(b.X - bend - ui.S(4), y2), PointF(b.X - ui.S(2), y2));
            }
            if (anyLink && !fx::Game()) ui.Tick();
            auto drawRow = [&](size_t i, bool lifted) {
                const tools::PathEntry& e = toolsPath_[i];
                float ry = rowY[i];
                RectF rowR(b.X - ui.S(6), ry, b.Width + ui.S(12), lineH);
                if (lifted) {
                    ui.Fill(RectF(rowR.X - ui.S(2), rowR.Y + ui.S(2), rowR.Width + ui.S(4), rowR.Height + ui.S(3)),
                            WithAlpha(Color(0, 0, 0), 90), ui.S(8));
                    ui.Fill(rowR, c.cardHi, ui.S(7));
                    ui.Stroke(rowR, WithAlpha(c.accent, 200), ui.S(7), ui.S(1.4f));
                } else if (i == hoverRow && !e.system) {
                    ui.Fill(rowR, WithAlpha(c.cardHi, 170), ui.S(6));
                }
                // подсветка участников затенения
                bool winner = false, loser = false;
                std::wstring loseTo;
                for (const Link& l : links) {
                    if (hoverRow == SIZE_MAX || (l.win != hoverRow && l.lose != hoverRow)) continue;
                    if (l.win == i) winner = true;
                    if (l.lose == i) {
                        loser = true;
                        loseTo += (loseTo.empty() ? L"" : L", ") + l.cmd;
                    }
                }
                if ((winner || loser) && i != hoverRow) ui.Fill(rowR, WithAlpha(c.gold, 22), ui.S(6));
                Color col = e.dead ? c.warn : e.dup ? c.gold : c.text;
                // левый край своей строки - ручка: под курсором вместо «свой» точки для перетаскивания
                int gid = ID_TOOLS_ROW + (int)i * 4 + 3;
                RectF grip(b.X - ui.S(6), ry, ui.S(62), lineH);
                if (!e.system && !busy) ui.Hover(gid, grip);
                if (!e.system && (ui.Hot(gid) || lifted)) {
                    SolidBrush dot(lifted ? c.accent : c.muted);
                    for (int r = 0; r < 3; ++r)
                        for (int q = 0; q < 2; ++q)
                            ui.G().FillEllipse(&dot, b.X + q * ui.S(6), ry + lineH / 2 - ui.S(7) + r * ui.S(6), ui.S(3),
                                          ui.S(3));
                    ui.Text(L"тяни", RectF(b.X + ui.S(16), ry, ui.S(48), lineH), c.faint, 10.5f);
                    ui.Tip(gid, L"Перетащи, чтобы поменять порядок: кто выше, того Windows находит первым");
                } else {
                    ui.Text(e.system ? L"система" : L"свой", RectF(b.X, ry, ui.S(64), lineH), c.faint, 10.5f);
                }
                float bw = e.system ? 0 : ui.S(210);
                std::wstring txt = e.raw + (e.dead ? L"   (нет папки)" : e.dup ? L"   (дубль)" : L"");
                ui.Text(txt, RectF(b.X + ui.S(66), ry, b.Width - ui.S(66) - bw - ui.S(150), lineH), col, 11.5f);
                if (loser)
                    ui.Text(L"уступает: " + loseTo, RectF(b.X + b.Width - bw - ui.S(148), ry, ui.S(144), lineH), c.gold,
                            10.5f, true, ui::Right);
                else if (!e.provides.empty())
                    ui.Text(e.provides, RectF(b.X + b.Width - bw - ui.S(148), ry, ui.S(144), lineH),
                            winner ? c.gold : c.muted, 10.5f, winner, ui::Right);
                if (i == hoverRow && !e.system && !lifted) {
                    float bx = b.X + b.Width - bw;
                    int k = 0;
                    for (size_t j = 0; j < i; ++j) k += !toolsPath_[j].system;
                    auto move = [&](int to) {
                        std::vector<std::wstring> v = user;
                        std::swap(v[k], v[to]);
                        ToolsRun(L"PATH", [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                        refresh();
                    };
                    if (ui.Button(ID_TOOLS_ROW + (int)i * 4, RectF(bx, ry + ui.S(2), ui.S(64), ui.S(26)), L"Выше") &&
                        k > 0 && !busy)
                        move(k - 1);
                    if (ui.Button(ID_TOOLS_ROW + (int)i * 4 + 1, RectF(bx + ui.S(70), ry + ui.S(2), ui.S(64), ui.S(26)),
                                  L"Ниже") &&
                        k + 1 < (int)user.size() && !busy)
                        move(k + 1);
                    if (ui.Button(ID_TOOLS_ROW + (int)i * 4 + 2,
                                  RectF(bx + ui.S(140), ry + ui.S(2), ui.S(70), ui.S(26)), L"Убрать") &&
                        !busy) {
                        std::vector<std::wstring> v = user;
                        v.erase(v.begin() + k);
                        ToolsRun(L"PATH", [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                        refresh();
                    }
                }
                // перетаскивание: началось - запоминаем, отпустили - пишем новый порядок
                if (!e.system) {
                    int k = 0;
                    for (size_t j = 0; j < i; ++j) k += !toolsPath_[j].system;
                    if (pathDrag_ < 0 && ui.Dragging(gid)) {
                        pathDrag_ = k;
                        pathDragTo_ = k;
                        pathGrab_ = ui.PressPt().y - ry;
                    }
                    if (ui.Released(gid) && pathDrag_ == k) {
                        int to = pathDragTo_;
                        pathDrag_ = -1;
                        ui.Invalidate();
                        if (to != k && to >= 0 && !busy) {
                            std::vector<std::wstring> v = user;
                            std::wstring moved = v[k];
                            v.erase(v.begin() + k);
                            v.insert(v.begin() + to, moved);
                            ToolsRun(L"PATH",
                                     [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                            refresh();
                        }
                    }
                }
            };
            for (size_t i : order)
                if (i != SIZE_MAX && (int)i != dragRow && rowY[i] >= 0) drawRow(i, false);
            if (dragRow >= 0) {
                ui.Invalidate();
                drawRow((size_t)dragRow, true);
            }
            if (pathDrag_ >= 0 && !ui.Down()) pathDrag_ = -1;  // кнопку отпустили где-то вне окна
            y += ui.S(8);
            RectF fr(b.X, y, b.Width - ui.S(140), ui.S(36));
            FieldOrText(
                ID_TOOLS_ADD, fr, view, toolsAddPath_, [this](const std::wstring& s) { toolsAddPath_ = s; },
                L"Папка для PATH пользователя");
            if (ui.Button(ID_TOOLS_ADD_BTN, RectF(b.X + b.Width - ui.S(130), y, ui.S(130), ui.S(36)), L"Добавить") &&
                !busy) {
                std::wstring p = toolsAddPath_;
                ToolsRun(L"PATH", [p](const tools::Log& l, ToolsJob&) { return tools::AddUserPath(p, l); });
                toolsAddPath_.clear();
                refresh();
            }
            y += ui.S(46);
            float bw = (b.Width - ui.S(20)) / 3;
            if (ui.Button(ID_TOOLS_CLEAN, RectF(b.X, y, bw, ui.S(36)),
                          L"Почистить (" + std::to_wstring(ndead + ndup) + L")", Kind::Primary) &&
                !busy) {
                ToolsRun(L"PATH", [](const tools::Log& l, ToolsJob&) { return tools::CleanUserPath(l) >= 0; });
                refresh();
            }
            if (ui.Button(ID_TOOLS_SYSCLEAN, RectF(b.X + bw + ui.S(10), y, bw, ui.S(36)), L"Системный (права)") &&
                !busy) {
                ToolsRun(L"Системный PATH",
                         [](const tools::Log& l, ToolsJob&) { return net::RunAdmin(net::JOB_SYSPATH_CLEAN, l); });
                refresh();
            }
            if (ui.Button(ID_TOOLS_RESTORE, RectF(b.X + 2 * (bw + ui.S(10)), y, bw, ui.S(36)), L"Вернуть прошлый") &&
                !busy) {
                std::vector<std::wstring> bk = tools::PathBackups();
                if (bk.empty()) {
                    Toast(L"Бэкапов PATH пока нет");
                } else if (MessageBoxW(wnd, (L"Вернуть PATH пользователя из\n" + bk[0] + L"?").c_str(), L"PATH",
                                       MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    std::wstring f = bk[0];
                    ToolsRun(L"PATH", [f](const tools::Log& l, ToolsJob&) { return tools::RestoreUserPath(f, l); });
                    refresh();
                }
            }
            y += ui.S(46);
            note(b, (y - b.Y) / rowH,
                 (toolsJob_ ? log() + L"\n" : L"") +
                     L"Windows ищет программу сначала в системном PATH, потом в своём, и берёт первую найденную. "
                     L"Каждая правка сохраняет прежний PATH в бэкап.",
                 c.muted);
        });
        wide(L"Что найдётся первым", rowsFor(toolsWhich_.size()), [&](const RectF& b) {
            float y = b.Y;
            for (const tools::Which& w : toolsWhich_) {
                ui.Text(w.cmd, RectF(b.X, y, ui.S(70), lineH), c.text, 12, true);
                ui.Text(w.path, RectF(b.X + ui.S(74), y, b.Width - ui.S(200), lineH), c.muted, 11);
                if (!w.other.empty())
                    ui.Text(L"затенено ещё " + std::to_wstring(w.other.size()),
                            RectF(b.X + b.Width - ui.S(122), y, ui.S(122), lineH), c.gold, 10.5f, false, ui::Right);
                y += lineH;
            }
        });
        wide(L"Переменные окружения", rowsFor(toolsEnv_.size()) + 1.2f, [&](const RectF& b) {
            float y = b.Y;
            for (const tools::EnvVar& v : toolsEnv_) {
                ui.Text(v.system ? L"система" : L"свой", RectF(b.X, y, ui.S(64), lineH), c.faint, 10.5f);
                ui.Text(v.name, RectF(b.X + ui.S(66), y, ui.S(200), lineH), c.text, 11.5f, true);
                ui.Text(v.value + (v.deadPath ? L"   (нет такого пути)" : L""),
                        RectF(b.X + ui.S(270), y, b.Width - ui.S(270), lineH), v.deadPath ? c.warn : c.muted, 11);
                y += lineH;
            }
            y += ui.S(8);
            float w3 = (b.Width - ui.S(150)) / 2;
            FieldOrText(
                ID_TOOLS_ENV_NAME, RectF(b.X, y, w3 - ui.S(6), ui.S(36)), view, toolsEnvName_,
                [this](const std::wstring& s) { toolsEnvName_ = s; }, L"Имя");
            FieldOrText(
                ID_TOOLS_ENV_VALUE, RectF(b.X + w3, y, w3 - ui.S(6), ui.S(36)), view, toolsEnvValue_,
                [this](const std::wstring& s) { toolsEnvValue_ = s; }, L"Значение (пусто - удалить)");
            if (ui.Button(ID_TOOLS_ENV_SET, RectF(b.X + b.Width - ui.S(140), y, ui.S(140), ui.S(36)), L"Записать") &&
                !busy) {
                std::wstring n = Trim(toolsEnvName_), v = toolsEnvValue_;
                if (!n.empty()) {
                    ToolsRun(L"Переменная",
                             [n, v](const tools::Log& l, ToolsJob&) { return tools::SetUserVar(n, v, l); });
                    refresh();
                }
            }
        });
    } else if (toolsTab_ == 1) {  // ---------------- диски ----------------
        card(L"Тома", rowsFor(toolsVols_.size()) * 1.6f + 0.4f, [&](const RectF& b) {
            float y = b.Y;
            for (const tools::Volume& v : toolsVols_) {
                double used = v.total ? 1.0 - (double)v.free / v.total : 0;
                ui.Text(v.root.substr(0, 2) + L"  " + v.label, RectF(b.X, y, ui.S(220), ui.S(22)), c.text, 12.5f, true);
                ui.Text(L"свободно " + tools::Bytes(v.free) + L" из " + tools::Bytes(v.total),
                        RectF(b.X + b.Width - ui.S(260), y, ui.S(260), ui.S(22)), tools::DiskWarn(v) ? c.warn : c.muted,
                        11.5f, false, ui::Right);
                RectF bar(b.X, y + ui.S(26), b.Width, ui.S(8));
                ui.Fill(bar, c.field, ui.S(4));
                ui.Fill(RectF(bar.X, bar.Y, (REAL)(bar.Width * used), bar.Height),
                        tools::DiskWarn(v) ? c.warn : c.accent, ui.S(4));
                y += lineH * 1.6f;
            }
        });
        if (toolsPhys_.empty() && physJob_ && physJob_->done) {  // здоровье дисков пришло из потока
            AcquireSRWLockShared(&physJob_->lock);
            toolsPhys_ = physJob_->disks;
            ReleaseSRWLockShared(&physJob_->lock);
        }
        card(L"Физические диски", rowsFor(toolsPhys_.size()) + 1.5f, [&](const RectF& b) {
            float y = b.Y;
            for (const tools::Physical& d : toolsPhys_) {
                std::wstring s = std::wstring(d.ssd ? L"SSD" : L"HDD") + L", " + tools::Bytes(d.size) + L", " +
                                 (d.health == 0   ? L"исправен"
                                  : d.health == 1 ? L"предупреждение"
                                                  : L"плохо");
                if (d.wear >= 0) s += L", износ " + std::to_wstring(d.wear) + L"%";
                if (d.temp > 0) s += L", " + std::to_wstring(d.temp) + L" °C";
                if (d.hours > 0) s += L", " + std::to_wstring(d.hours) + L" ч работы";
                ui.Text(d.name, RectF(b.X, y, b.Width * 0.45f, lineH), c.text, 12, true);
                ui.Text(s, RectF(b.X + b.Width * 0.45f, y, b.Width * 0.55f, lineH), d.health ? c.warn : c.muted, 11.5f);
                y += lineH;
            }
            if (ui.Button(ID_TOOLS_HEALTH, RectF(b.X, y + ui.S(6), ui.S(240), ui.S(36)), L"Износ и температура") &&
                !busy) {
                std::vector<tools::Physical> base = toolsPhys_;
                ToolsRun(L"Здоровье дисков", [base](const tools::Log& l, ToolsJob& job) {
                    std::vector<tools::Physical> d = base;
                    bool ok = tools::DiskHealth(d, l);
                    AcquireSRWLockExclusive(&job.lock);
                    job.phys = d;
                    job.hasPhys = true;
                    ReleaseSRWLockExclusive(&job.lock);
                    return ok;
                });
            }
            note(b, (y + ui.S(48) - b.Y) / rowH, L"Нужны права: один раз - задача планировщика, она только читает.",
                 c.faint);
        });
        if (toolsJob_ && toolsJob_->done && toolsJob_->hasPhys) {
            toolsPhys_ = toolsJob_->phys;
            toolsJob_->hasPhys = false;
        }
        // что занимает место
        std::vector<tools::FolderSize> sizes;
        bool scanning = false;
        long seen = 0;
        if (toolsJob_ && toolsJob_->title == L"Что занимает место") {
            AcquireSRWLockShared(&toolsJob_->lock);
            sizes = toolsJob_->sizes;
            ReleaseSRWLockShared(&toolsJob_->lock);
            scanning = !toolsJob_->done;
            seen = toolsJob_->progress;
        }
        size_t shown = std::min<size_t>(sizes.size(), 20);
        // слева кольцо долей (наведение и щелчок - как по строке списка), справа сам список
        bool ring = !sizes.empty() || scanning;
        float ringW = ring ? ui.S(250) : 0;
        float rows = rowsFor(shown) + 2.0f;
        if (ring) rows = std::max(rows, (ui.S(46) + ringW) / rowH);
        if (!ring) rows += 2.6f;  // место под приглашение посчитать
        wide(L"Что занимает место", rows, [&](const RectF& b) {
            std::wstring dir = toolsScanPath_.empty() ? L"C:\\" : toolsScanPath_;
            RectF fr(b.X, b.Y, b.Width - ui.S(290), ui.S(36));
            FieldOrText(
                ID_TOOLS_SCAN_PATH, fr, view, dir, [this](const std::wstring& s) { toolsScanPath_ = s; },
                L"Папка или диск");
            if (ui.Button(ID_TOOLS_SCAN, RectF(b.X + b.Width - ui.S(280), b.Y, ui.S(136), ui.S(36)),
                          scanning ? L"Стоп" : L"Посчитать", Kind::Primary)) {
                if (scanning)
                    toolsJob_->cancel = true;
                else if (!busy)
                    ToolsScan(dir);
            }
            if (ui.Button(ID_TOOLS_SCAN_UP, RectF(b.X + b.Width - ui.S(136), b.Y, ui.S(136), ui.S(36)), L"Выше") &&
                !busy) {
                std::wstring d = dir;
                while (d.size() > 3 && d.back() == L'\\') d.pop_back();
                size_t cut = d.find_last_of(L'\\');
                if (cut != std::wstring::npos && d.size() > 3) ToolsScan(cut <= 2 ? d.substr(0, 3) : d.substr(0, cut));
            }
            float y = b.Y + ui.S(46);
            if (!ring) {
                ui.EmptyState(ID_TOOLS_RING, RectF(b.X, y, b.Width, ui.S(110)),
                              L"Укажи папку или диск и нажми «Посчитать» - здесь вырастет кольцо: чем больше папка, "
                              L"тем шире её доля. Щелчок по доле - зайти внутрь.");
                return;
            }
            int hot = -1;
            for (size_t i = 0; i < shown; ++i)
                if (ui.Hot(ID_TOOLS_SIZE + (int)i * 2)) hot = (int)i;
            int clicked = SizeRing(RectF(b.X, y, ringW - ui.S(20), ringW - ui.S(20)), dir, sizes, scanning, seen, hot);
            if (clicked >= 0 && clicked < (int)sizes.size() && sizes[clicked].name.front() != L'(' && !busy)
                ToolsScan(sizes[clicked].path);
            RectF lb(b.X + ringW, b.Y, b.Width - ringW, b.Height);
            ULONGLONG top = sizes.empty() ? 1 : std::max<ULONGLONG>(1, sizes[0].bytes);
            for (size_t i = 0; i < shown; ++i) {
                const tools::FolderSize& f = sizes[i];
                float yy = y + i * lineH;
                bool isDir = f.name.front() != L'(';
                // вся строка - «зайти внутрь»; размер полоской на всю ширину строки
                RectF rr(lb.X - ui.S(6), yy, lb.Width - ui.S(110), lineH);
                if (rowHover(ID_TOOLS_SIZE + (int)i * 2, rr) && isDir && !busy) {
                    hiveZoom_ = (int)i;
                    ToolsScan(f.path);
                }
                if (hot == (int)i && !ui.Hot(ID_TOOLS_SIZE + (int)i * 2))  // наведено на долю в кольце
                    ui.Fill(rr, WithAlpha(c.cardHi, 200), ui.S(6));
                Color mark = SizeColor(c, (int)i, (int)std::min<size_t>(sizes.size(), kRingParts));
                if (i < kRingParts)
                    ui.Fill(RectF(lb.X - ui.S(2), yy + ui.S(10), ui.S(4), lineH - ui.S(20)), mark, ui.S(2));
                RectF bar(lb.X + ui.S(8), yy + lineH - ui.S(5), (REAL)((lb.Width - ui.S(248)) * (double)f.bytes / top),
                          ui.S(3));
                ui.Fill(bar, WithAlpha(i < kRingParts ? mark : c.accent, 150), ui.S(1.5f));
                ui.Text(f.name, RectF(lb.X + ui.S(8), yy, lb.Width - ui.S(368), lineH), isDir ? c.text : c.muted, 12,
                        isDir);
                ui.Text(tools::Bytes(f.bytes), RectF(lb.X + lb.Width - ui.S(360), yy, ui.S(110), lineH), c.text, 12,
                        true, ui::Right);
                if (f.files)
                    ui.Text(Plural(f.files, L"файл", L"файла", L"файлов"),
                            RectF(lb.X + lb.Width - ui.S(240), yy, ui.S(124), lineH), c.faint, 10.5f, false, ui::Right);
                if (rowHot(RectF(lb.X - ui.S(6), yy, lb.Width + ui.S(12), lineH)) &&
                    ui.Button(ID_TOOLS_SIZE + (int)i * 2 + 1,
                              RectF(lb.X + lb.Width - ui.S(100), yy + ui.S(2), ui.S(100), ui.S(26)), L"Открыть")) {
                    std::wstring p = f.path;
                    Defer([p] { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
                }
            }
        });
        // кэши
        // чистка идёт - над списком вихрь, куда осыпаются очищаемые места
        bool vortex = scene::CleanRecent();
        float vortexH = vortex ? scene::CleanScene::Height(scene::Clean().items.size(), (float)ui.K()) + ui.S(10) : 0;
        wide(L"Кэши и временные файлы", rowsFor(toolsJunk_.size()) + 1.6f + vortexH / rowH, [&](const RectF& b) {
            if (toolsJob_ && toolsJob_->done && toolsJob_->hasJunk) {
                toolsJunk_ = toolsJob_->junk;
                toolsJob_->hasJunk = false;
            }
            float y = b.Y;
            if (vortex) {
                CleanSceneIn(RectF(b.X, y, b.Width, vortexH - ui.S(10)));
                y += vortexH;
            }
            for (size_t i = 0; i < toolsJunk_.size(); ++i) {
                const tools::Junk& j = toolsJunk_[i];
                ui.Text(j.name, RectF(b.X, y, ui.S(220), lineH), c.text, 12, true);
                ui.Text(Ellip(j.path, 56), RectF(b.X + ui.S(224), y, b.Width - ui.S(470), lineH), c.faint, 10.5f);
                ui.Text(tools::Bytes(j.bytes), RectF(b.X + b.Width - ui.S(240), y, ui.S(110), lineH), c.gold, 12, true,
                        ui::Right);
                if (ui.Button(ID_TOOLS_JUNK + (int)i,
                              RectF(b.X + b.Width - ui.S(120), y + ui.S(2), ui.S(120), ui.S(26)), L"Очистить") &&
                    !busy &&
                    MessageBoxW(wnd,
                                (L"Удалить содержимое\n" + j.path + L"\n(" + tools::Bytes(j.bytes) +
                                 L")?\n\nЭто кэш: программы скачают или создадут его заново. Вернуть удалённое нельзя.")
                                    .c_str(),
                                L"Очистка", MB_YESNO | MB_ICONWARNING) == IDYES) {
                    tools::Junk jj = j;
                    scene::CleanBegin({{jj.name, jj.bytes}});
                    ToolsRun(L"Очистка", [jj](const tools::Log& l, ToolsJob&) {
                        scene::CleanMark(0, 1);
                        scene::CleanMark(0, 2, tools::CleanJunk(jj, l));
                        scene::CleanEnd();
                        return true;
                    });
                    toolsJunk_.erase(toolsJunk_.begin() + i);
                    break;
                }
                y += lineH;
            }
            if (ui.Button(ID_TOOLS_JUNK_SCAN, RectF(b.X, y + ui.S(6), ui.S(200), ui.S(36)), L"Посчитать размеры") &&
                !busy)
                ToolsRun(L"Кэши", [](const tools::Log& l, ToolsJob& job) {
                    l(L"Считаю размеры кэшей...");
                    std::vector<tools::Junk> j = tools::FindJunk(true);
                    AcquireSRWLockExclusive(&job.lock);
                    job.junk = j;
                    job.hasJunk = true;
                    ReleaseSRWLockExclusive(&job.lock);
                    return true;
                });
            if (toolsJob_ && toolsJob_->title != L"Что занимает место")
                note(b, (y + ui.S(48) - b.Y) / rowH, log(), c.muted);
        });
    } else if (toolsTab_ == 2) {  // ---------------- автозагрузка ----------------
        wide(L"Запускается вместе с Windows", rowsFor(toolsStarts_.size()) * 1.5f + 0.8f, [&](const RectF& b) {
            float y = b.Y;
            for (size_t i = 0; i < toolsStarts_.size(); ++i) {
                tools::StartItem& it = toolsStarts_[i];
                bool on = it.enabled;
                if (it.canToggle) {
                    if (ui.Toggle(ID_TOOLS_START + (int)i, RectF(b.X, y + ui.S(4), ui.S(42), ui.S(30)), on) && !busy) {
                        std::wstring msg;
                        if (tools::SetStartup(it, on, [&](const std::wstring& s) { msg = s; })) it.enabled = on;
                        Toast(msg);
                    }
                } else {
                    ui.Text(it.enabled ? L"вкл" : L"выкл", RectF(b.X, y, ui.S(42), ui.S(26)), c.faint, 11, false,
                            ui::Center);
                }
                ui.Text(it.name, RectF(b.X + ui.S(56), y, b.Width * 0.45f, ui.S(24)), it.enabled ? c.text : c.muted,
                        12.5f, true);
                ui.Text(it.where + (it.canToggle ? L"" : L" - для всех, переключается в Диспетчере задач"),
                        RectF(b.X + ui.S(56) + b.Width * 0.45f, y, b.Width * 0.55f - ui.S(56), ui.S(24)), c.faint,
                        10.5f);
                ui.Text(Ellip(it.command, 110), RectF(b.X + ui.S(56), y + ui.S(20), b.Width - ui.S(56), ui.S(22)),
                        c.muted, 10.5f);
                y += lineH * 1.5f;
            }
            note(b, (y - b.Y) / rowH,
                 L"Выключение - как в Диспетчере задач: запись остаётся, программа просто не запускается. Включить "
                 L"можно обратно.",
                 c.muted);
        });
    } else {  // ---------------- порты ----------------
        wide(L"Кто слушает порты", rowsFor(std::min<size_t>(toolsPorts_.size(), 40)), [&](const RectF& b) {
            float y = b.Y;
            for (size_t i = 0; i < toolsPorts_.size() && i < 40; ++i) {
                const tools::Listener& l = toolsPorts_[i];
                RectF portR(b.X - ui.S(6), y, b.Width + ui.S(12), lineH);
                if (rowHot(portR)) ui.Fill(portR, WithAlpha(c.cardHi, 170), ui.S(6));
                ui.Text(std::wstring(l.udp ? L"UDP " : L"TCP ") + std::to_wstring(l.port),
                        RectF(b.X, y, ui.S(90), lineH), l.dev ? c.gold : c.text, 12, l.dev);
                ui.Text(l.process + L"   PID " + std::to_wstring(l.pid),
                        RectF(b.X + ui.S(96), y, b.Width - ui.S(230), lineH), c.muted, 11.5f);
                if (rowHot(portR) &&
                    ui.Button(ID_TOOLS_PORT + (int)i,
                              RectF(b.X + b.Width - ui.S(120), y + ui.S(2), ui.S(120), ui.S(26)), L"Завершить")) {
                    bool ok = tools::KillPid(l.pid, l.process);
                    Toast(ok ? L"Завершён " + l.process : L"Не удалось завершить " + l.process);
                    refresh();
                }
                y += lineH;
            }
        });
        wide(L"Кто держит файл или папку", rowsFor(toolsLockers_.size()) + 1.8f, [&](const RectF& b) {
            RectF fr(b.X, b.Y, b.Width - ui.S(290), ui.S(36));
            FieldOrText(
                ID_TOOLS_LOCK_PATH, fr, view, toolsLockPath_, [this](const std::wstring& s) { toolsLockPath_ = s; },
                L"Путь к файлу или папке");
            if (ui.Button(ID_TOOLS_LOCK, RectF(b.X + b.Width - ui.S(280), b.Y, ui.S(136), ui.S(36)), L"Проверить",
                          Kind::Primary)) {
                toolsLockers_ = tools::WhoLocks(toolsLockPath_);
                toolsLockChecked_ = true;
            }
            if (ui.Button(ID_TOOLS_LOCK_CLIP, RectF(b.X + b.Width - ui.S(136), b.Y, ui.S(136), ui.S(36)),
                          L"Из буфера")) {
                toolsLockPath_ = tools::ClipboardPath();
                toolsLockers_ = tools::WhoLocks(toolsLockPath_);
                toolsLockChecked_ = true;
            }
            float y = b.Y + ui.S(46);
            if (toolsLockChecked_ && toolsLockers_.empty())
                ui.Text(L"Никто не держит - файл свободен.", RectF(b.X, y, b.Width, lineH), c.muted, 11.5f);
            for (size_t i = 0; i < toolsLockers_.size(); ++i) {
                const tools::Locker& k = toolsLockers_[i];
                ui.Text(k.name + L"   PID " + std::to_wstring(k.pid), RectF(b.X, y, b.Width - ui.S(130), lineH), c.text,
                        12);
                if (ui.Button(ID_TOOLS_LOCKER + (int)i,
                              RectF(b.X + b.Width - ui.S(120), y + ui.S(2), ui.S(120), ui.S(26)), L"Завершить")) {
                    bool ok = tools::KillPid(k.pid);
                    Toast(ok ? L"Завершён " + k.name : L"Не удалось завершить " + k.name);
                    toolsLockers_ = tools::WhoLocks(toolsLockPath_);
                    break;
                }
                y += lineH;
            }
        });
    }

    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
