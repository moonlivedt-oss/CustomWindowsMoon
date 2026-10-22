// Раздел «Утилиты»: PATH и переменные, диски (место, здоровье, что занимает, кэши), автозагрузка, порты и блокировки.
// Долгое (сканирование, очистка, задача с правами) - в своём потоке, страница только читает результат.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "app/toolsui.h"
#include "core/bg.h"
#include "core/elevate.h"
#include "core/files.h"
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

void ToolsThread(const std::shared_ptr<ToolsJob>& job) {
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
}

}  // namespace

std::wstring Ellip(const std::wstring& s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + L"…" : s; }

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
        ui.Text(seen < 0 ? std::wstring(L"Читаю таблицу MFT...") : L"файлов: " + std::to_wstring(seen),
                RectF(cx - tw / 2, cy + ui.S(17), tw, ui.S(18)), c.faint, 10.5f, false, ui::Center);
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
    if (!bg::Run([job] { ToolsThread(job); })) job->done = true;
}

void Studio::ToolsScan(const std::wstring& dir, bool setup) {
    if (toolsJob_ && !toolsJob_->done) toolsJob_->cancel = true;
    toolsScanPath_ = dir;
    std::wstring d = dir;
    toolsJob_.reset();
    ToolsRun(L"Что занимает место", [d, setup](const tools::Log& log, ToolsJob& job) {
        if (setup) {  // «Включить быстрый подсчёт»: один запрос UAC, потом тот же подсчёт уже по MFT
            log(L"Настраиваю задачу с правами - Windows спросит разрешение.");
            if (!net::SetupAdmin()) log(L"Разрешение не дано - считаю по файлам.");
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // обход диска не мешает работе
        log(L"Считаю " + d + L"...");
        // задача с правами уже есть и диск NTFS - читаем таблицу MFT целиком (секунды) или берём её из кэша; нет -
        // обход по файлам
        std::vector<tools::FolderSize> r;
        std::wstring note;
        tools::MftState state = tools::MFT_READY;
        bool mft = tools::ScanSizesMft(d, r, note, &job.mftReading, &state, &job.cancel);
        job.mftState = (int)state;
        if (mft) {
            log(L"Посчитано по таблице MFT: " + note + L".");
        } else {
            if (!note.empty()) log(L"MFT не подошла (" + note + L"), считаю по файлам...");
            r = tools::ScanSizes(d, &job.cancel, &job.progress, [&job](const std::vector<tools::FolderSize>& sofar) {
                AcquireSRWLockExclusive(&job.lock);  // список растёт по мере подсчёта
                job.sizes = sofar;
                ++job.sizesGen;
                ReleaseSRWLockExclusive(&job.lock);
            });
        }
        AcquireSRWLockExclusive(&job.lock);
        job.sizes = std::move(r);
        ++job.sizesGen;
        job.hasSizes = true;
        ReleaseSRWLockExclusive(&job.lock);
        return !job.cancel;
    });
}

void Studio::GoTools(int tab, const std::wstring& scan) {
    toolsTab_ = std::clamp(tab, 0, 7);
    toolsAt_ = 0;
    GoPage(PAGE_TOOLS);
    if (!scan.empty()) ToolsScan(scan);
}

void Studio::ToolsPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Утилиты",
           L"PATH и переменные, диски, автозагрузка, порты. Быстрые действия - в кольце утилит на углу.");
    RectF tabs(Column(in, ui.S(900)).X, in.Y + ui.S(58), std::min(in.Width, ui.S(840)), ui.S(38));
    int tab = toolsTab_;
    if (ui.Segmented(ID_TOOLS_TAB, tabs,
                     {L"PATH", L"Диски", L"Автозагрузка", L"Порты", L"Процессы", L"Версии", L"Защита", L"Приватность"},
                     tab)) {
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
    ULONGLONG every = toolsTab_ == 3 || (toolsTab_ == 6 && secView_.running) ? 3000 : 15000;
    if (!toolsAt_ || now - toolsAt_ > every) {
        toolsAt_ = now;
        if (toolsTab_ == 0) {
            toolsPath_ = tools::ReadPath();
            toolsWhich_ = tools::WhichAll();
            toolsEnv_ = tools::ReadEnv();
        } else if (toolsTab_ == 1) {
            toolsVols_ = tools::VolumesCached(3000);
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
            tools::StartupWeight(toolsStarts_);  // вес для дорожки входа: кто сколько держит
        } else if (toolsTab_ == 3) {
            toolsPorts_ = tools::Listeners();
            tools::ListenerProjects(toolsPorts_);  // у кабеля подпись - проект, который занял порт
        } else if (toolsTab_ == 5 && versions_.empty()) {
            versions_ = toolsui::Versions();
        } else if (toolsTab_ == 6) {
            secView_ = toolsui::SecurityFinds();
        } else if (toolsTab_ == 7) {
            toolsPriv_ = tools::PrivacyTweaks();
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
    ToolsKit kit{ui, cols, view, rowH, lineH, busy, now, log, refresh};

    if (toolsTab_ == 0) {  // ---------------- PATH ----------------
        ToolsPathTab(kit);
    } else if (toolsTab_ == 1) {  // ---------------- диски ----------------
        ToolsDiskTab(kit);
    } else if (toolsTab_ == 7) {  // ---------------- приватность ----------------
        wide(L"Приватность Windows", rowsFor(toolsPriv_.size()) * 1.5f + 2.2f, [&](const RectF& b) {
            float y = b.Y;
            auto adminRun = [&](const std::wstring& req, const std::wstring& title) {
                ToolsRun(title, [req](const tools::Log& l, ToolsJob&) {
                    elevate::Put(L"PrivacyJob", req);
                    return net::RunAdmin(net::JOB_PRIVACY, l);
                });
            };
            for (size_t i = 0; i < toolsPriv_.size(); ++i) {
                tools::Tweak& t = toolsPriv_[i];
                bool on = t.on;
                if (ui.Toggle(ID_TOOLS_PRIV + (int)i, RectF(b.X, y + ui.S(4), ui.S(42), ui.S(30)), on) && !busy) {
                    if (t.admin) {
                        adminRun((on ? L"on|" : L"off|") + t.id, t.title);
                    } else if (!on && !t.ours) {
                        Toast(L"Это выключено не RadialMenu - возвращать нечего (настройка самой Windows).");
                    } else {
                        std::wstring msg;
                        tools::SetPrivacyTweak(t.id, on, [&](const std::wstring& s) { msg = s; });
                        Toast(msg);
                    }
                    refresh();
                }
                ui.Text(t.title, RectF(b.X + ui.S(56), y, b.Width * 0.6f, ui.S(24)), t.on ? c.text : c.muted, 12.5f,
                        true);
                if (t.admin)
                    ui.Text(L"для всего компьютера, нужны права",
                            RectF(b.X + b.Width * 0.6f, y, b.Width * 0.4f, ui.S(24)), c.faint, 10.5f, false, ui::Right);
                ui.Text(t.why, RectF(b.X + ui.S(56), y + ui.S(20), b.Width - ui.S(56), ui.S(22)), c.muted, 10.5f);
                y += lineH * 1.5f;
            }
            y += ui.S(8);
            if (ui.Button(ID_TOOLS_PRIV + 60, RectF(b.X, y, ui.S(220), ui.S(36)), L"Вернуть всё как было",
                          Kind::Secondary) &&
                !busy) {
                std::wstring msg;
                tools::RevertPrivacy([&](const std::wstring& s) { msg = s; });
                bool anyAdmin = false;
                for (const tools::Tweak& t : toolsPriv_) anyAdmin = anyAdmin || (t.admin && t.on);
                if (anyAdmin) adminRun(L"off|*", L"Приватность: как было");
                Toast(msg);
                refresh();
            }
            note(b, (y + ui.S(46) - b.Y) / rowH,
                 L"Каждый переключатель пишет известные значения реестра, прежние - в бэкап до первой правки; «как "
                 L"было» возвращает их (или убирает значение, если его не было). Свои настройки - сразу, для всего "
                 L"компьютера - одно окно UAC.",
                 c.muted);
        });
    } else if (toolsTab_ == 2) {  // ---------------- автозагрузка ----------------
        float sh = scene::StartScene::Height((float)ui.K());
        wide(L"Дорожка входа", sh / rowH, [&](const RectF& b) { StartLive(RectF(b.X, b.Y, b.Width, sh)); });
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
    } else if (toolsTab_ == 3) {  // ---------------- порты ----------------
        size_t cables = 0;
        for (const tools::Listener& l : toolsPorts_)
            if (Cable(l)) ++cables;
        if (getenv("RADIAL_DEMO")) cables = 5;
        float ph = scene::PortsScene::Height(std::min<size_t>(cables, 8), (float)ui.K());
        wide(L"Порты разработки", ph / rowH, [&](const RectF& b) {
            if (!cables && !getenv("RADIAL_DEMO")) {
                ui.EmptyState(ID_LIVE_PORTS, RectF(b.X, b.Y, b.Width, ph),
                              L"Порты разработки свободны: ни один сервер проекта сейчас не слушает");
                return;
            }
            PortsLive(RectF(b.X, b.Y, b.Width, ph));
        });
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

    if (toolsTab_ == 4) {  // ---------------- процессы: колба памяти и батарея ----------------
        float fh = scene::FlaskScene::Height((float)ui.K());
        wide(L"Память: колба", fh / rowH,
             [&](const RectF& b) { FlaskLive(flaskScene_, ID_LIVE_FLASK, RectF(b.X, b.Y, b.Width, fh)); });
        float bh = scene::BatteryScene::Height((float)ui.K());
        wide(L"Фон: куда уходит заряд", bh / rowH, [&](const RectF& b) { BatteryLive(RectF(b.X, b.Y, b.Width, bh)); });
    } else if (toolsTab_ == 5) {  // ---------------- версии ----------------
        if (versionsBusy_ && toolsJob_ && toolsJob_->done) {
            versionsBusy_ = false;
            versions_ = toolsui::Versions();
        }
        size_t n = getenv("RADIAL_DEMO") ? 5 : versions_.size();
        float vh = scene::VersScene::Height(n, (float)ui.K());
        wide(L"Версии инструментов", (vh + ui.S(52)) / rowH, [&](const RectF& b) {
            if (ui.Button(ID_TOOLS_VER_CHECK, RectF(b.X, b.Y, ui.S(200), ui.S(36)),
                          versionsBusy_ ? L"Проверяю" : L"Проверить сейчас", Kind::Secondary, L"", versionsBusy_) &&
                !busy) {
                versionsBusy_ = true;
                ToolsRun(L"Версии", [](const tools::Log& l, ToolsJob&) {
                    l(L"Спрашиваю endoflife.date, GitHub и MSYS2...");
                    std::vector<tools::ToolVersion> v = tools::ToolVersions();
                    if (!v.empty()) toolsui::SetVersions(v);
                    return !v.empty();
                });
            }
            ui.Text(L"python, node, cmake, git - что найдётся первым в PATH против вышедшего; раз в неделю само",
                    RectF(b.X + ui.S(214), b.Y, b.Width - ui.S(214), ui.S(36)), c.faint, 11);
            RectF vr(b.X, b.Y + ui.S(52), b.Width, vh);
            if (!n)
                ui.EmptyState(ID_LIVE_VERS, vr, L"Ещё не проверялось: нажми «Проверить сейчас» (около 20 секунд)");
            else
                VersLive(vr);
        });
    } else if (toolsTab_ == 6) {                // ---------------- защита ----------------
        if (secView_.running) RepaintIn(1000);  // итог проверки придёт сам
        std::vector<tools::FindKind> kinds;
        for (const tools::Suspect& s : secView_.finds)
            if (std::find(kinds.begin(), kinds.end(), s.kind) == kinds.end()) kinds.push_back(s.kind);
        std::sort(kinds.begin(), kinds.end());
        if (secKind_ > (int)kinds.size()) secKind_ = 0;
        std::vector<size_t> shown;
        for (size_t i = 0; i < secView_.finds.size() && shown.size() < 60; ++i)
            if (!secKind_ || secView_.finds[i].kind == kinds[secKind_ - 1]) shown.push_back(i);
        float findH = lineH * 1.6f;
        wide(L"Находки", (shown.size() * findH + ui.S(kinds.size() > 1 ? 104 : 58)) / rowH, [&](const RectF& b) {
            std::wstring head = secView_.running ? std::wstring(L"Проверяю: подписи, места запуска, автозапуск...")
                                : !secView_.checked
                                    ? std::wstring(L"Ещё не проверялось")
                                    : L"Проверено " +
                                          Plural(secView_.processes, L"программа", L"программы", L"программ") + L" и " +
                                          Plural(secView_.autoruns, L"запись", L"записи", L"записей") + L" автозапуска";
            ui.Text(head, RectF(b.X, b.Y, b.Width - ui.S(390), ui.S(36)), c.muted, 11.5f);
            if (secView_.ignored &&
                ui.Button(ID_TOOLS_SEC_UNHIDE, RectF(b.X + b.Width - ui.S(386), b.Y, ui.S(190), ui.S(36)),
                          L"Показать скрытые " + std::to_wstring(secView_.ignored))) {
                toolsui::ClearIgnored();
                refresh();
            }
            if (ui.Button(ID_TOOLS_SEC_RESCAN, RectF(b.X + b.Width - ui.S(186), b.Y, ui.S(186), ui.S(36)),
                          secView_.running   ? L"Проверяю"
                          : secView_.checked ? L"Проверить заново"
                                             : L"Проверить",
                          Kind::Primary, L"", secView_.running) &&
                !secView_.running) {
                toolsui::SecurityRescan();
                secView_.running = true;
                refresh();
            }
            float y = b.Y + ui.S(48);
            if (kinds.size() > 1) {
                std::vector<std::wstring> labels{L"Все"};
                for (tools::FindKind k : kinds) {  // в фильтре - первое слово: «задача», «расширение»
                    std::wstring n = toolsui::FindKindName(k);
                    labels.push_back(n.substr(0, n.find(L' ')));
                }
                int sel = secKind_;
                RectF fr(b.X, y, std::min(b.Width, ui.S(118) * labels.size()), ui.S(34));
                if (ui.Segmented(ID_TOOLS_SEC_KIND, fr, labels, sel)) secKind_ = sel;
                y += ui.S(46);
            }
            if (shown.empty()) {
                ui.Text(secView_.checked
                            ? L"Ничего странного: всё запущенное и автозапуск подписаны или лежат на своих "
                              L"местах."
                            : L"Первая проверка идёт в фоне - подписи считаются до минуты.",
                        RectF(b.X, y, b.Width, lineH), c.muted, 11.5f);
                y += lineH;
            }
            for (size_t n = 0; n < shown.size(); ++n) {
                const tools::Suspect& s = secView_.finds[shown[n]];
                RectF rr(b.X - ui.S(6), y, b.Width + ui.S(12), findH);
                bool hot = rowHot(rr);
                if (hot) ui.Fill(rr, WithAlpha(c.cardHi, 170), ui.S(6));
                bool danger = s.score >= 6;
                // значок вида находки (сертификат, брандмауэр, служба...), рядом - насколько серьёзно
                ui.Icon(ExeDir() + L"\\icons\\" + toolsui::FindKindIcon(s.kind),
                        RectF(b.X + ui.S(2), y + ui.S(6), ui.S(32), ui.S(32)));
                ui.Text(danger ? L"опасно" : L"странно", RectF(b.X + ui.S(40), y + ui.S(2), ui.S(70), ui.S(22)),
                        danger ? c.warn : c.gold, 11, true);
                float tx = b.X + ui.S(116), tw = b.Width - ui.S(116) - (hot ? ui.S(250) : 0);
                std::wstring name = Ellip(s.name, 60);
                float nw = std::min(tw, ui.Measure(name, 12, true) + ui.S(12));
                ui.Text(name, RectF(tx, y + ui.S(2), nw, ui.S(22)), c.text, 12, true);
                ui.Text(toolsui::FindKindName(s.kind), RectF(tx + nw, y + ui.S(3), std::max(0.0f, tw - nw), ui.S(22)),
                        c.faint, 10.5f);
                ui.Text(Ellip(s.why + (s.path.empty() ? L"" : L". " + s.path), 150),
                        RectF(tx, y + ui.S(23), tw, ui.S(22)), c.muted, 10.5f);
                if (hot) {
                    float bx = b.X + b.Width - ui.S(244);
                    if (ui.Button(ID_TOOLS_SEC + (int)n * 2, RectF(bx, y + ui.S(8), ui.S(130), ui.S(28)), L"Что делать",
                                  Kind::Primary))
                        toolsui::SecurityAct(s);
                    if (ui.Button(ID_TOOLS_SEC + (int)n * 2 + 1,
                                  RectF(bx + ui.S(138), y + ui.S(8), ui.S(106), ui.S(28)), L"Это моё")) {
                        toolsui::IgnoreFind(s);
                        Toast(L"Больше не показываю: " + s.name);
                        refresh();
                        break;
                    }
                }
                y += findH;
            }
        });
        // расширения браузеров по правам (Н4): всё, что нашла «Защита», доступ к входам и паролям - сверху
        std::vector<size_t> extShown;
        int extRisky = 0;
        for (const tools::BrowserExt& e : secView_.exts) extRisky += tools::ExtRisk(e.rights) >= 4;
        for (size_t i = 0; i < secView_.exts.size() && extShown.size() < 40; ++i)
            if (!extKind_ || tools::ExtRisk(secView_.exts[i].rights) >= 4) extShown.push_back(i);
        std::wstring extIcon = ExeDir() + L"\\icons\\tools-extension.png";
        if (GetFileAttributesW(extIcon.c_str()) == INVALID_FILE_ATTRIBUTES)
            extIcon = ExeDir() + L"\\icons\\tools-shield.png";
        wide(L"Расширения браузеров", (std::max<size_t>(extShown.size(), 1) * findH + ui.S(58)) / rowH,
             [&](const RectF& b) {
                 std::wstring head =
                     !secView_.checked ? std::wstring(L"Появятся после проверки")
                     : secView_.exts.empty()
                         ? std::wstring(L"Расширений с особыми правами нет")
                         : Plural((int)secView_.exts.size(), L"расширение", L"расширения", L"расширений") +
                               L" с правами, к входам и паролям (буфер, куки, программы): " + std::to_wstring(extRisky);
                 ui.Text(head, RectF(b.X, b.Y, b.Width - ui.S(250), ui.S(36)), c.muted, 11.5f);
                 int sel = extKind_;
                 if (ui.Segmented(ID_TOOLS_EXT_KIND, RectF(b.X + b.Width - ui.S(240), b.Y, ui.S(240), ui.S(34)),
                                  {L"Все", L"Опасные"}, sel))
                     extKind_ = sel;
                 float y = b.Y + ui.S(48);
                 if (extShown.empty())
                     ui.Text(extKind_ ? L"Опасных нет: ни одно расширение не читает входы, буфер и куки сразу на всех "
                                        L"сайтах."
                                      : L"Chrome, Edge, Brave и Яндекс: расширения с доступом к сайтам и данным.",
                             RectF(b.X, y, b.Width, lineH), c.muted, 11.5f);
                 for (size_t n = 0; n < extShown.size(); ++n) {
                     const tools::BrowserExt& e = secView_.exts[extShown[n]];
                     RectF rr(b.X - ui.S(6), y, b.Width + ui.S(12), findH);
                     bool hot = rowHot(rr);
                     if (hot) ui.Fill(rr, WithAlpha(c.cardHi, 170), ui.S(6));
                     int risk = tools::ExtRisk(e.rights);
                     ui.Icon(extIcon, RectF(b.X + ui.S(2), y + ui.S(6), ui.S(32), ui.S(32)));
                     if (risk >= 2)
                         ui.Text(risk >= 4 ? L"опасно" : L"все сайты",
                                 RectF(b.X + ui.S(40), y + ui.S(2), ui.S(74), ui.S(22)), risk >= 4 ? c.warn : c.gold,
                                 11, true);
                     float tx = b.X + ui.S(116), tw = b.Width - ui.S(116) - (hot ? ui.S(200) : 0);
                     std::wstring name = Ellip(e.name, 60);
                     float nw = std::min(tw, ui.Measure(name, 12, true) + ui.S(12));
                     ui.Text(name, RectF(tx, y + ui.S(2), nw, ui.S(22)), c.text, 12, true);
                     ui.Text(e.browser + (e.profile.empty() ? L"" : L", " + e.profile) +
                                 (e.store ? L"" : L", не из магазина"),
                             RectF(tx + nw, y + ui.S(3), std::max(0.0f, tw - nw), ui.S(22)), e.store ? c.faint : c.gold,
                             10.5f);
                     ui.Text(Ellip(tools::ExtRightsText(e.rights), 150), RectF(tx, y + ui.S(23), tw, ui.S(22)), c.muted,
                             10.5f);
                     if (hot && ui.Button(ID_TOOLS_EXT + (int)n,
                                          RectF(b.X + b.Width - ui.S(192), y + ui.S(8), ui.S(192), ui.S(28)),
                                          L"Открыть в браузере", Kind::Primary)) {
                         tools::BrowserExt copy = e;
                         Defer([copy] { toolsui::OpenExtension(copy); });
                     }
                     y += findH;
                 }
             });
        bool signReady = tools::CodeSigningReady();
        wide(L"Подпись своих exe", 1.5f, [&](const RectF& b) {
            ui.Text(signReady
                        ? L"Сертификат MoonRadial Local есть: сборка подписывает RadialMenu.exe и RadialTools.exe. "
                          L"Чужой файл с этой подписью «Защита» считает подделкой."
                        : L"Своим сертификатом сборка подпишет RadialMenu.exe и RadialTools.exe - у Kaspersky "
                          L"меньше поводов, помощник с правами сверяет подпись. Сертификат доверен только у вас.",
                    RectF(b.X + ui.S(52), b.Y, b.Width - ui.S(272), ui.S(52)), c.muted, 11.5f, false, ui::Left, true);
            ui.Icon(ExeDir() + L"\\icons\\tools-sign.png", RectF(b.X, b.Y + ui.S(4), ui.S(40), ui.S(40)));
            if (!signReady &&
                ui.Button(ID_TOOLS_SIGN, RectF(b.X + b.Width - ui.S(206), b.Y + ui.S(6), ui.S(206), ui.S(36)),
                          L"Настроить подпись", Kind::Secondary, L"", busy) &&
                !busy)
                ToolsRun(L"Подпись", [](const tools::Log& l, ToolsJob&) { return tools::SetupCodeSigning(l); });
            if (toolsJob_ && toolsJob_->title == L"Подпись")
                ui.Text(log(), RectF(b.X, b.Y + ui.S(52), b.Width, ui.S(40)), c.faint, 10.5f);
        });
    }

    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
