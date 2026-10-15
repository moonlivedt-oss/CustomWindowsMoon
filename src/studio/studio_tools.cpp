// Раздел «Утилиты»: PATH и переменные, диски (место, здоровье, что занимает, кэши), автозагрузка, порты и блокировки.
// Долгое (сканирование, очистка, задача с правами) - в своём потоке, страница только читает результат.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include "core/text.h"
#include "gfx/draw.h"
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

}  // namespace

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
        log(L"Считаю " + d + L"...");
        std::vector<tools::FolderSize> r = tools::ScanSizes(d, &job.cancel, &job.progress);
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
    if (busy) ui.Invalidate();
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
            if (toolsPhys_.empty()) toolsPhys_ = tools::PhysicalDisks();
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
                RectF rowR(b.X - ui.S(6), y, b.Width + ui.S(12), lineH);
                bool hot = !e.system && rowHot(rowR);
                if (hot) ui.Fill(rowR, WithAlpha(c.cardHi, 170), ui.S(6));
                int kk = e.system ? -1 : ui_++;
                Color col = e.dead ? c.warn : e.dup ? c.gold : c.text;
                ui.Text(e.system ? L"система" : L"свой", RectF(b.X, y, ui.S(64), lineH), c.faint, 10.5f);
                float bw = e.system ? 0 : ui.S(210);
                std::wstring txt = e.raw + (e.dead ? L"   (нет папки)" : e.dup ? L"   (дубль)" : L"");
                ui.Text(txt, RectF(b.X + ui.S(66), y, b.Width - ui.S(66) - bw - ui.S(150), lineH), col, 11.5f);
                if (!e.provides.empty())
                    ui.Text(e.provides, RectF(b.X + b.Width - bw - ui.S(148), y, ui.S(144), lineH), c.muted, 10.5f,
                            false, ui::Right);
                if (hot) {
                    float bx = b.X + b.Width - bw;
                    int k = kk;
                    auto move = [&](int to) {
                        std::vector<std::wstring> v = user;
                        std::swap(v[k], v[to]);
                        ToolsRun(L"PATH", [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                        refresh();
                    };
                    if (ui.Button(ID_TOOLS_ROW + (int)i * 4, RectF(bx, y + ui.S(2), ui.S(64), ui.S(26)), L"Выше") &&
                        k > 0 && !busy)
                        move(k - 1);
                    if (ui.Button(ID_TOOLS_ROW + (int)i * 4 + 1, RectF(bx + ui.S(70), y + ui.S(2), ui.S(64), ui.S(26)),
                                  L"Ниже") &&
                        k + 1 < (int)user.size() && !busy)
                        move(k + 1);
                    if (ui.Button(ID_TOOLS_ROW + (int)i * 4 + 2, RectF(bx + ui.S(140), y + ui.S(2), ui.S(70), ui.S(26)),
                                  L"Убрать") &&
                        !busy) {
                        std::vector<std::wstring> v = user;
                        v.erase(v.begin() + k);
                        ToolsRun(L"PATH", [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                        refresh();
                    }
                }
                y += lineH;
            }
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
        wide(L"Что занимает место", rowsFor(shown) + 2.0f + (scanning ? 1 : 0), [&](const RectF& b) {
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
            if (scanning)
                ui.Text(L"Считаю... просмотрено файлов: " + std::to_wstring(seen), RectF(b.X, y, b.Width, lineH),
                        c.muted, 11.5f);
            ULONGLONG top = sizes.empty() ? 1 : std::max<ULONGLONG>(1, sizes[0].bytes);
            for (size_t i = 0; i < shown; ++i) {
                const tools::FolderSize& f = sizes[i];
                float yy = y + (scanning ? lineH : 0) + i * lineH;
                bool isDir = f.name.front() != L'(';
                // вся строка - «зайти внутрь»; размер полоской на всю ширину строки
                RectF rr(b.X - ui.S(6), yy, b.Width - ui.S(110), lineH);
                if (rowHover(ID_TOOLS_SIZE + (int)i * 2, rr) && isDir && !busy) ToolsScan(f.path);
                RectF bar(b.X, yy + lineH - ui.S(5), (REAL)((b.Width - ui.S(240)) * (double)f.bytes / top), ui.S(3));
                ui.Fill(bar, WithAlpha(c.accent, 150), ui.S(1.5f));
                ui.Text(f.name, RectF(b.X, yy, b.Width - ui.S(360), lineH), isDir ? c.text : c.muted, 12, isDir);
                ui.Text(tools::Bytes(f.bytes), RectF(b.X + b.Width - ui.S(360), yy, ui.S(110), lineH), c.text, 12, true,
                        ui::Right);
                if (f.files)
                    ui.Text(Plural(f.files, L"файл", L"файла", L"файлов"),
                            RectF(b.X + b.Width - ui.S(240), yy, ui.S(124), lineH), c.faint, 10.5f, false, ui::Right);
                if (rowHot(RectF(b.X - ui.S(6), yy, b.Width + ui.S(12), lineH)) &&
                    ui.Button(ID_TOOLS_SIZE + (int)i * 2 + 1,
                              RectF(b.X + b.Width - ui.S(100), yy + ui.S(2), ui.S(100), ui.S(26)), L"Открыть")) {
                    std::wstring p = f.path;
                    Defer([p] { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
                }
            }
        });
        // кэши
        wide(L"Кэши и временные файлы", rowsFor(toolsJunk_.size()) + 1.6f, [&](const RectF& b) {
            if (toolsJob_ && toolsJob_->done && toolsJob_->hasJunk) {
                toolsJunk_ = toolsJob_->junk;
                toolsJob_->hasJunk = false;
            }
            float y = b.Y;
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
                    ToolsRun(L"Очистка", [jj](const tools::Log& l, ToolsJob&) {
                        tools::CleanJunk(jj, l);
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
                if (rowHot(portR) && ui.Button(ID_TOOLS_PORT + (int)i,
                              RectF(b.X + b.Width - ui.S(120), y + ui.S(2), ui.S(120), ui.S(26)), L"Завершить")) {
                    bool ok = tools::KillPid(l.pid);
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
