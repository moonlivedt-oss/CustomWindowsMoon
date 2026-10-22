// Студия, «Утилиты» - вкладка «Диски»: место и здоровье, что занимает место (соты, MFT), кэши, гиганты,
// большие файлы, поиск папок.
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

void Studio::ToolsDiskTab(ToolsKit& tk) {
    const UiColors& c = ui.C();
    tk.Card(L"Тома", tk.RowsFor(toolsVols_.size()) * 1.6f + 0.4f, [&](const RectF& b) {
        float y = b.Y;
        for (const tools::Volume& v : toolsVols_) {
            double used = v.total ? 1.0 - (double)v.free / v.total : 0;
            ui.Text(v.root.substr(0, 2) + L"  " + v.label, RectF(b.X, y, ui.S(220), ui.S(22)), c.text, 12.5f, true);
            ui.Text(L"свободно " + tools::Bytes(v.free) + L" из " + tools::Bytes(v.total),
                    RectF(b.X + b.Width - ui.S(260), y, ui.S(260), ui.S(22)), tools::DiskWarn(v) ? c.warn : c.muted,
                    11.5f, false, ui::Right);
            RectF bar(b.X, y + ui.S(26), b.Width, ui.S(8));
            ui.Fill(bar, c.field, ui.S(4));
            ui.Fill(RectF(bar.X, bar.Y, (REAL)(bar.Width * used), bar.Height), tools::DiskWarn(v) ? c.warn : c.accent,
                    ui.S(4));
            y += tk.lineH * 1.6f;
        }
    });
    if (toolsPhys_.empty() && physJob_ && physJob_->done) {  // здоровье дисков пришло из потока
        AcquireSRWLockShared(&physJob_->lock);
        toolsPhys_ = physJob_->disks;
        ReleaseSRWLockShared(&physJob_->lock);
    }
    tk.Card(L"Физические диски", tk.RowsFor(toolsPhys_.size()) + 1.5f, [&](const RectF& b) {
        float y = b.Y;
        for (const tools::Physical& d : toolsPhys_) {
            std::wstring s = std::wstring(d.ssd ? L"SSD" : L"HDD") + L", " + tools::Bytes(d.size) + L", " +
                             (d.health == 0   ? L"исправен"
                              : d.health == 1 ? L"предупреждение"
                                              : L"плохо");
            if (d.wear >= 0) s += L", износ " + std::to_wstring(d.wear) + L"%";
            if (d.temp > 0) s += L", " + std::to_wstring(d.temp) + L" °C";
            if (d.hours > 0) s += L", " + std::to_wstring(d.hours) + L" ч работы";
            ui.Text(d.name, RectF(b.X, y, b.Width * 0.45f, tk.lineH), c.text, 12, true);
            ui.Text(s, RectF(b.X + b.Width * 0.45f, y, b.Width * 0.55f, tk.lineH), d.health ? c.warn : c.muted, 11.5f);
            y += tk.lineH;
        }
        if (ui.Button(ID_TOOLS_HEALTH, RectF(b.X, y + ui.S(6), ui.S(240), ui.S(36)), L"Износ и температура") &&
            !tk.busy) {
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
        tk.Note(b, (y + ui.S(48) - b.Y) / tk.rowH, L"Нужны права: один раз - задача планировщика, она только читает.",
                c.faint);
    });
    if (toolsJob_ && toolsJob_->done && toolsJob_->hasPhys) {
        toolsPhys_ = toolsJob_->phys;
        toolsJob_->hasPhys = false;
    }
    // что занимает место
    bool scanning = false, mftOffer = false;
    long seen = 0;
    static const std::vector<tools::FolderSize> kNoSizes;
    bool sizing = toolsJob_ && toolsJob_->title == L"Что занимает место";
    const std::vector<tools::FolderSize>& sizes = sizing ? toolsSizes_ : kNoSizes;
    if (sizing) {
        AcquireSRWLockShared(&toolsJob_->lock);  // копия - только когда список поменялся, не каждый кадр
        if (toolsSizesJob_.lock() != toolsJob_ || toolsSizesGen_ != toolsJob_->sizesGen) {
            toolsSizes_ = toolsJob_->sizes;
            toolsSizesJob_ = toolsJob_;
            toolsSizesGen_ = toolsJob_->sizesGen;
        }
        ReleaseSRWLockShared(&toolsJob_->lock);
        scanning = !toolsJob_->done;
        seen = toolsJob_->mftReading ? -1 : (long)toolsJob_->progress;  // -1: задача с правами читает MFT
        int st = toolsJob_->mftState;
        mftOffer = toolsJob_->done && (st == tools::MFT_NONE || st == tools::MFT_OLD);
    }
    // папки, куда обходу нет входа: их размер виден только по MFT
    ULONGLONG closedBytes = 0;
    std::wstring closedNames;
    for (const tools::FolderSize& f : sizes)
        if (f.closed) {
            closedBytes += f.bytes;
            if (closedNames.size() < 80) closedNames += (closedNames.empty() ? L"" : L", ") + f.name;
        }
    bool extra = !scanning && (closedBytes || mftOffer);
    size_t shown = std::min<size_t>(sizes.size(), 20);
    // слева кольцо долей (наведение и щелчок - как по строке списка), справа сам список
    bool ring = !sizes.empty() || scanning;
    float ringW = ring ? ui.S(250) : 0;
    float rows = tk.RowsFor(shown) + 2.0f + (extra ? 1.4f : 0);
    if (ring) rows = std::max(rows, (ui.S(46) + ringW) / tk.rowH);
    if (!ring) rows += 2.6f;  // место под приглашение посчитать
    tk.Wide(L"Что занимает место", rows, [&](const RectF& b) {
        std::wstring dir = toolsScanPath_.empty() ? L"C:\\" : toolsScanPath_;
        RectF fr(b.X, b.Y, b.Width - ui.S(290), ui.S(36));
        FieldOrText(
            ID_TOOLS_SCAN_PATH, fr, tk.view, dir, [this](const std::wstring& s) { toolsScanPath_ = s; },
            L"Папка или диск");
        if (ui.Button(ID_TOOLS_SCAN, RectF(b.X + b.Width - ui.S(280), b.Y, ui.S(136), ui.S(36)),
                      scanning ? L"Стоп" : L"Посчитать", Kind::Primary)) {
            if (scanning)
                toolsJob_->cancel = true;
            else if (!tk.busy)
                ToolsScan(dir);
        }
        if (ui.Button(ID_TOOLS_SCAN_UP, RectF(b.X + b.Width - ui.S(136), b.Y, ui.S(136), ui.S(36)), L"Выше") &&
            !tk.busy) {
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
        if (clicked >= 0 && clicked < (int)sizes.size() && sizes[clicked].name.front() != L'(' && !tk.busy)
            ToolsScan(sizes[clicked].path);
        RectF lb(b.X + ringW, b.Y, b.Width - ringW, b.Height);
        ULONGLONG top = sizes.empty() ? 1 : std::max<ULONGLONG>(1, sizes[0].bytes);
        for (size_t i = 0; i < shown; ++i) {
            const tools::FolderSize& f = sizes[i];
            float yy = y + i * tk.lineH;
            bool isDir = f.name.front() != L'(';
            // вся строка - «зайти внутрь»; размер полоской на всю ширину строки
            RectF rr(lb.X - ui.S(6), yy, lb.Width - ui.S(110), tk.lineH);
            if (tk.RowHover(ID_TOOLS_SIZE + (int)i * 2, rr) && isDir && !tk.busy) {
                hiveZoom_ = (int)i;
                ToolsScan(f.path);
            }
            if (hot == (int)i && !ui.Hot(ID_TOOLS_SIZE + (int)i * 2))  // наведено на долю в кольце
                ui.Fill(rr, WithAlpha(c.cardHi, 200), ui.S(6));
            Color mark = SizeColor(c, (int)i, (int)std::min<size_t>(sizes.size(), kRingParts));
            if (i < kRingParts)
                ui.Fill(RectF(lb.X - ui.S(2), yy + ui.S(10), ui.S(4), tk.lineH - ui.S(20)), mark, ui.S(2));
            RectF bar(lb.X + ui.S(8), yy + tk.lineH - ui.S(5), (REAL)((lb.Width - ui.S(248)) * (double)f.bytes / top),
                      ui.S(3));
            ui.Fill(bar, WithAlpha(i < kRingParts ? mark : c.accent, 150), ui.S(1.5f));
            ui.Text(f.name, RectF(lb.X + ui.S(8), yy, lb.Width - ui.S(368), tk.lineH), isDir ? c.text : c.muted, 12,
                    isDir);
            ui.Text(tools::Bytes(f.bytes), RectF(lb.X + lb.Width - ui.S(360), yy, ui.S(110), tk.lineH), c.text, 12,
                    true, ui::Right);
            if (f.files)
                ui.Text(Plural(f.files, L"файл", L"файла", L"файлов"),
                        RectF(lb.X + lb.Width - ui.S(240), yy, ui.S(124), tk.lineH), c.faint, 10.5f, false, ui::Right);
            if (tk.RowHot(RectF(lb.X - ui.S(6), yy, lb.Width + ui.S(12), tk.lineH)) &&
                ui.Button(ID_TOOLS_SIZE + (int)i * 2 + 1,
                          RectF(lb.X + lb.Width - ui.S(100), yy + ui.S(2), ui.S(100), ui.S(26)), L"Открыть")) {
                std::wstring p = f.path;
                Defer([p] { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
            }
        }
        if (!extra) return;
        float yy = y + shown * tk.lineH + ui.S(8);
        if (closedBytes)
            ui.Text(L"ещё " + tools::Bytes(closedBytes) + L" в закрытых папках (видно только по MFT): " + closedNames,
                    RectF(lb.X + ui.S(8), yy, lb.Width - ui.S(16), tk.lineH), c.muted, 11.5f);
        else if (mftOffer) {
            ui.Text(L"Целый диск считается минутами. С задачей с правами - секунды по таблице MFT.",
                    RectF(lb.X + ui.S(8), yy, lb.Width - ui.S(270), tk.lineH), c.faint, 11.5f);
            if (ui.Button(ID_TOOLS_MFT_SETUP, RectF(lb.X + lb.Width - ui.S(250), yy, ui.S(250), ui.S(32)),
                          L"Включить быстрый подсчёт") &&
                !tk.busy)
                ToolsScan(dir, true);
        }
    });
    // поиск папки по имени в таблицах MFT (И8): миллисекунды, размер рядом
    size_t found = std::min<size_t>(toolsFound_.size(), 15);
    tk.Wide(L"Найти папку", tk.RowsFor(found) + 1.6f, [&](const RectF& b) {
        FieldOrText(
            ID_TOOLS_FIND, RectF(b.X, b.Y, b.Width, ui.S(36)), tk.view, toolsFind_,
            [this](const std::wstring& s) {
                toolsFind_ = s;
                toolsFound_.clear();
                if (Trim(s).size() >= 2) toolsFindAny_ = tools::MftFindDirs(Trim(s), 15, toolsFound_, &toolsFindAge_);
            },
            L"Имя папки - поиск по таблице MFT всех томов, раскладка не важна");
        float y = b.Y + ui.S(46);
        if (!toolsFindAny_) {
            ui.Text(L"Таблиц MFT ещё нет: «Посчитать» целый диск выше с задачей с правами - и поиск заработает.",
                    RectF(b.X, y, b.Width, tk.lineH), c.muted, 11.5f);
            return;
        }
        for (size_t i = 0; i < found; ++i) {
            const tools::FoundDir& f = toolsFound_[i];
            float yy = y + i * tk.lineH;
            ui.Text(f.path, RectF(b.X, yy, b.Width - ui.S(360), tk.lineH), c.text, 11.5f);
            ui.Text(tools::Bytes(f.bytes), RectF(b.X + b.Width - ui.S(350), yy, ui.S(110), tk.lineH), c.gold, 12, true,
                    ui::Right);
            if (ui.Button(ID_TOOLS_FOUND + (int)i * 2,
                          RectF(b.X + b.Width - ui.S(230), yy + ui.S(2), ui.S(120), ui.S(26)), L"Что внутри") &&
                !tk.busy)
                ToolsScan(f.path);
            if (ui.Button(ID_TOOLS_FOUND + (int)i * 2 + 1,
                          RectF(b.X + b.Width - ui.S(100), yy + ui.S(2), ui.S(100), ui.S(26)), L"Открыть")) {
                std::wstring p = f.path;
                Defer([p] { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
            }
        }
        if (!toolsFind_.empty() && toolsFound_.empty() && Trim(toolsFind_).size() >= 2)
            ui.Text(L"Не нашлось", RectF(b.X, y, b.Width, tk.lineH), c.muted, 11.5f);
        else if (found)
            ui.Text(L"Размеры - на момент таблицы (" + std::to_wstring(toolsFindAge_ / 60) + L" мин назад)",
                    RectF(b.X, y + found * tk.lineH, b.Width, tk.lineH), c.faint, 10.5f);
    });
    // системные гиганты (И11)
    std::vector<tools::Giant> giants =
        Demo() ? std::vector<tools::Giant>{{L"hiber", L"Гибернация", L"C:\\hiberfil.sys",
                                            L"Файл сна на диске: размер памяти", L"hiber-off", L"", 6ull << 30, true}}
               : toolsui::Giants();
    bool hiber = false;
    for (const tools::Giant& g : giants) hiber |= g.key == L"hiber";
    if (toolsui::GiantsCounting() && giants.empty()) RepaintIn(500);
    tk.Wide(L"Системные гиганты", tk.RowsFor(giants.size()) * 1.9f + 1.6f, [&](const RectF& b) {
        float y = b.Y, rh = tk.lineH * 1.9f;
        if (giants.empty())
            ui.Text(toolsui::GiantsCounting() ? L"Считаю..."
                                              : L"Ничего крупного: гибернация выключена, прошлой "
                                                L"Windows нет.",
                    RectF(b.X, y, b.Width, tk.lineH), c.muted, 11.5f);
        for (size_t i = 0; i < giants.size(); ++i) {
            const tools::Giant& g = giants[i];
            float yy = y + i * rh;
            ui.Text(g.name, RectF(b.X, yy, b.Width * 0.5f, ui.S(22)), c.text, 12.5f, true);
            std::wstring size = !g.exact && !g.bytes
                                    ? std::wstring(L"размер - с правами")
                                    : (g.exact || g.path.find(L".sys") != std::wstring::npos ? L"" : L"не меньше ") +
                                          tools::Bytes(g.bytes);
            ui.Text(size, RectF(b.X + b.Width - ui.S(390), yy, ui.S(150), ui.S(22)), c.gold, 12, true, ui::Right);
            ui.Text(g.why, RectF(b.X, yy + ui.S(22), b.Width - ui.S(230), rh - ui.S(24)), c.faint, 10.5f, false,
                    ui::Left, true);
            static const std::map<std::wstring, std::wstring> kLabel = {
                {L"hiber-off", L"Выключить"},      {L"old-windows", L"Убрать"},  {L"shadow-shrink", L"Ограничить 5%"},
                {L"sys-clean", L"Остатки обнов."}, {L"page-open", L"Настроить"}, {L"cleanmgr", L"Очистка диска"}};
            auto lab = kLabel.find(g.action);
            if (lab != kLabel.end() &&
                ui.Button(ID_TOOLS_GIANT + (int)i, RectF(b.X + b.Width - ui.S(220), yy, ui.S(220), ui.S(30)),
                          lab->second) &&
                !tk.busy) {
                std::wstring k = g.key;
                Defer([k] { toolsui::GiantAct(k); });
            }
        }
        float yb = y + std::max<size_t>(giants.size(), 1) * rh + ui.S(4);
        if (ui.Button(ID_TOOLS_GIANT + 40, RectF(b.X, yb, ui.S(220), ui.S(34)), L"Точно, с правами") && !tk.busy)
            Defer([] { toolsui::GiantsMeasure(); });
        if (!hiber && !toolsui::GiantsCounting() &&
            ui.Button(ID_TOOLS_GIANT + 41, RectF(b.X + ui.S(230), yb, ui.S(220), ui.S(34)), L"Вернуть гибернацию") &&
            !tk.busy)
            Defer([] { toolsui::GiantAct(L"hiber", true); });
    });
    // большие файлы томов (И9): список кладёт задача с правами при подсчёте по MFT
    if (!Demo() && GetTickCount64() - toolsBigAt_ > 5000) {
        toolsBigAt_ = GetTickCount64();
        toolsBig_ = tools::BigFiles(&toolsBigAge_);
    }
    size_t bigN = std::min<size_t>(toolsBig_.size(), 25);
    tk.Wide(L"Большие файлы", tk.RowsFor(bigN) + 1.4f, [&](const RectF& b) {
        float y = b.Y;
        if (toolsBig_.empty()) {
            ui.Text(
                L"Список появится после подсчёта целого диска по MFT («Посчитать» для C:\\ выше, с задачей "
                L"с правами): задача заодно запоминает 200 самых больших файлов тома.",
                RectF(b.X, y, b.Width, tk.lineH * 1.4f), c.muted, 11.5f, false, ui::Left, true);
            return;
        }
        for (size_t i = 0; i < bigN; ++i) {
            const tools::BigFile& f = toolsBig_[i];
            float yy = y + i * tk.lineH;
            bool can = f.why.empty();
            ui.Text(f.path, RectF(b.X, yy, b.Width - ui.S(360), tk.lineH), can ? c.text : c.muted, 11.5f);
            if (!can) ui.Tip(ID_TOOLS_BIG + (int)i * 2 + 1, L"Не в Корзину: " + f.why);
            ui.Text(tools::Bytes(f.bytes), RectF(b.X + b.Width - ui.S(350), yy, ui.S(110), tk.lineH), c.gold, 12, true,
                    ui::Right);
            if (ui.Button(ID_TOOLS_BIG + (int)i * 2,
                          RectF(b.X + b.Width - ui.S(230), yy + ui.S(2), ui.S(110), ui.S(26)), L"Показать")) {
                std::wstring p = f.path;
                Defer([p] {
                    ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + p + L"\"").c_str(), nullptr,
                                  SW_SHOWNORMAL);
                });
            }
            if (ui.Button(ID_TOOLS_BIG + (int)i * 2 + 1,
                          RectF(b.X + b.Width - ui.S(110), yy + ui.S(2), ui.S(110), ui.S(26)), L"В Корзину",
                          can ? Kind::Secondary : Kind::Ghost) &&
                can && !tk.busy &&
                MessageBoxW(wnd,
                            (L"Отправить в Корзину\n" + f.path + L"\n(" + tools::Bytes(f.bytes) +
                             L")?\n\nВернуть можно из Корзины, пока она не очищена. Очень большие файлы Windows "
                             L"может удалить мимо Корзины - тогда она спросит сама.")
                                .c_str(),
                            L"Большой файл", MB_YESNO | MB_ICONWARNING) == IDYES) {
                std::wstring p = f.path;
                ToolsRun(L"В Корзину", [p](const tools::Log& l, ToolsJob&) { return tools::RecycleBig(p, l) > 0; });
                toolsBigAt_ = 0;
            }
        }
        ui.Text(L"По таблице MFT " + std::to_wstring(toolsBigAge_ / 3600) +
                    L" ч назад; размеры - нынешние. Серые - системное, программы и диски WSL: их здесь не удаляю.",
                RectF(b.X, y + bigN * tk.lineH, b.Width, tk.lineH), c.faint, 10.5f);
    });
    // кэши
    // чистка идёт - над списком вихрь, куда осыпаются очищаемые места
    bool vortex = scene::CleanRecent();
    float vortexH = vortex ? scene::CleanScene::Height(scene::Clean().items.size(), (float)ui.K()) + ui.S(10) : 0;
    tk.Wide(L"Кэши и временные файлы", tk.RowsFor(toolsJunk_.size()) + 1.6f + vortexH / tk.rowH, [&](const RectF& b) {
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
            ui.Text(j.name, RectF(b.X, y, ui.S(220), tk.lineH), c.text, 12, true);
            ui.Text(Ellip(j.path, 56), RectF(b.X + ui.S(224), y, b.Width - ui.S(470), tk.lineH), c.faint, 10.5f);
            ui.Text(tools::Bytes(j.bytes), RectF(b.X + b.Width - ui.S(240), y, ui.S(110), tk.lineH), c.gold, 12, true,
                    ui::Right);
            if (ui.Button(ID_TOOLS_JUNK + (int)i, RectF(b.X + b.Width - ui.S(120), y + ui.S(2), ui.S(120), ui.S(26)),
                          L"Очистить") &&
                !tk.busy && !toolsui::CleanBusy() &&
                MessageBoxW(wnd,
                            (L"Удалить содержимое\n" + j.path + L"\n(" + tools::Bytes(j.bytes) +
                             L")?\n\nЭто кэш: программы скачают или создадут его заново. Вернуть удалённое нельзя.")
                                .c_str(),
                            L"Очистка", MB_YESNO | MB_ICONWARNING) == IDYES &&
                toolsui::CleanClaim()) {  // пока спрашивали, могла начаться тихая чистка
                tools::Junk jj = j;
                scene::CleanBegin({{jj.name, jj.bytes}});
                ToolsRun(L"Очистка", [jj](const tools::Log& l, ToolsJob&) {
                    toolsui::CleanList({jj}, l);
                    return true;
                });
                toolsJunk_.erase(toolsJunk_.begin() + i);
                break;
            }
            y += tk.lineH;
        }
        if (ui.Button(ID_TOOLS_JUNK_SCAN, RectF(b.X, y + ui.S(6), ui.S(200), ui.S(36)), L"Посчитать размеры") &&
            !tk.busy)
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
            tk.Note(b, (y + ui.S(48) - b.Y) / tk.rowH, tk.Log(), c.muted);
    });
}

}  // namespace studio
