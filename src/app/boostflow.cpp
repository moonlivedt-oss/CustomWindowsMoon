// Boost: что закрыть перед игрой и Unreal, итог, возврат закрытого, новости во время игры.
#include "app/app_internal.h"

#include <tlhelp32.h>

#include <algorithm>
#include <map>

#include "app/afterwork.h"
#include "app/autos.h"
#include "app/gameprep.h"
#include "app/toolsui.h"
#include "core/bg.h"
#include "core/files.h"
#include "core/procsnap.h"
#include "core/launch.h"
#include "core/fx.h"
#include "core/text.h"
#include "core/themes.h"
#include "gfx/sound.h"

namespace app {

std::wstring Size(double mb) {
    wchar_t b[32];
    if (mb >= 1024)
        swprintf(b, 32, L"%.1f ГБ", mb / 1024);
    else
        swprintf(b, 32, L"%d МБ", (int)mb);
    return b;
}

// «Discord, Telegram, Edge WebView и ещё 3»
std::wstring Names(const std::vector<std::wstring>& names) {
    std::wstring s;
    size_t shown = std::min<size_t>(names.size(), 3);
    for (size_t i = 0; i < shown; ++i) s += (i ? L", " : L"") + names[i];
    if (names.size() > shown) s += L" и ещё " + std::to_wstring(names.size() - shown);
    return s;
}

void RunAfterBoost() {
    if (!g_afterBoost) return;
    auto f = std::move(g_afterBoost);
    g_afterBoost = nullptr;
    f();
}

// Что сделать по выбору в карточке: закрыть (boost::Free) и усыпить
// (gboost::Soften) - в потоке.
struct BoostJob {
    std::vector<boost::Group> close;
    std::vector<DWORD> sleep;
    int sleepApps = 0;
};

void BoostWork(const BoostJob* job) {
    std::wstring dir = BoostDir();
    double before = boost::AvailMb();
    auto* r = new boost::Result;
    if (!job->close.empty()) *r = boost::Free(boost::Load(dir), dir, job->close);
    if (!job->sleep.empty()) {
        gboost::Soften(job->sleep);
        r->slept = job->sleepApps;
        Sleep(400);
    }
    r->freedMb = std::max(0.0, boost::AvailMb() - before);
    PostMessageW(g_wnd, WM_BOOST_DONE, 0, (LPARAM)r);
}

void StartBoost(BoostJob* job) {
    if (g_boostBusy) {
        delete job;
        return;
    }
    g_boostBusy = true;
    g_boostCard.Show(BoostCard::WORKING, L"Освобождаю память...",
                     job->close.empty() ? L"Усыпляю: память программ уходит в подкачку, "
                                          L"процессор - в экономию."
                                        : L"Программы закрываются мягко, как крестиком; кто не "
                                          L"закрылся - через пару "
                                          L"секунд добивается.",
                     CurrentTheme(g_cfg));
    std::shared_ptr<BoostJob> j(job);
    if (!bg::Run([j] { BoostWork(j.get()); })) g_boostBusy = false;
}

std::wstring GameExe() {
    for (DWORD pid : g_game.GamePids()) {
        std::wstring p = gm::ProcessPath(pid);
        if (!p.empty()) return p;
    }
    return L"";
}

// Путь к запущенному Unreal Editor (для списка исключений антивируса).
std::wstring EditorExe() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    PROCESSENTRY32W pe{sizeof(pe)};
    std::wstring out;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && out.empty(); ok = Process32NextW(snap, &pe))
        if (gm::MatchAny(pe.szExeFile, g_cfg.devApps)) out = gm::ProcessPath(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

// Карточка «освободить память»: при запуске игры или редактора (если памяти
// мало по minFreePercent и есть что закрыть) или вручную из трея, угла, студии.
// why - к чему готовимся: «перед игрой», «для Unreal Editor». Что не предлагать
// закрыть: сама игра и редактор.
std::vector<std::wstring> KeepApps() {
    std::vector<std::wstring> keep;
    if (g_game.On())
        for (DWORD pid : g_game.GamePids()) {
            std::wstring full = gm::ProcessPath(pid);
            if (!full.empty()) keep.push_back(gm::FileName(full));
        }
    if (g_dev.On()) keep.push_back(L"UnrealEditor.exe");
    return keep;
}

// Что делать с программой, пока для игры нет своего выбора: WebView держит
// поиск Windows - оставить, мессенджеры с голосом - усыпить (закрытие оборвёт
// звонок), остальное - закрыть, как раньше.
int DefaultAct(const std::wstring& key) {
    if (key.find(L"webview") != std::wstring::npos) return gboost::ACT_KEEP;
    for (const wchar_t* m : {L"discord", L"telegram", L"whatsapp", L"viber", L"skype"})
        if (key == m) return gboost::ACT_SLEEP;
    return gboost::ACT_CLOSE;
}

void ApplyPick(const std::vector<BoostPick::Row>& rows) {
    std::map<std::wstring, int> acts;
    auto* job = new BoostJob;
    for (const BoostPick::Row& r : rows) {
        acts[r.key] = r.act;
        for (const boost::Group& g : g_boostGroups) {
            if (Lower(g.name) != r.key) continue;
            if (r.act == gboost::ACT_CLOSE) job->close.push_back(g);
            if (r.act == gboost::ACT_SLEEP) {
                job->sleep.insert(job->sleep.end(), g.pids.begin(), g.pids.end());
                job->sleepApps++;
            }
        }
    }
    gboost::SaveProfile(g_pickFor, acts);
    if (job->close.empty() && job->sleep.empty()) {
        delete job;
        g_boostCard.Show(BoostCard::DONE, L"Ничего не трогаю", L"Выбор запомнен.", CurrentTheme(g_cfg), 3500);
        RunAfterBoost();
        return;
    }
    StartBoost(job);
}

// Л21: что помешает игре - обновления Windows (идёт TiWorker), загрузка Steam или браузера, занятая сеть, батарея.
// Сбор - в фоне (Загрузки и процессы), карточка - в основном потоке.
void GamePrep() {
    bg::Run([] {
        gameprep::In in;
        std::shared_ptr<const procsnap::List> ps = procsnap::Get(0);
        for (const PROCESSENTRY32W& p : *ps) in.updates = in.updates || !_wcsicmp(p.szExeFile, L"TiWorker.exe");
        in.steam = afterwork::SteamDownloading();
        in.partials = afterwork::FreshPartials();
        in.downMbps = toolsui::NetDownBps() * 8 / 1e6;
        tools::BatteryInfo bat = tools::Battery();
        in.battery = bat.present && !bat.ac;
        in.percent = bat.percent;
        auto* out = new gameprep::Out;
        if (!gameprep::Check(in, *out) || !PostMessageW(g_wnd, WM_GAMEPREP, 0, (LPARAM)out)) delete out;
    });
}

void GamePrepShow(LPARAM lp) {
    std::unique_ptr<gameprep::Out> o((gameprep::Out*)lp);
    if (!g_game.On()) return;  // игру уже закрыли
    ThemeInfo t = CurrentTheme(g_cfg);
    autos::Note(L"Подготовка к игре: " + o->text);
    if (o->action == gameprep::Out::NONE) {
        g_gbCard.Show(BoostCard::DONE, o->title, o->text, t, 12000, Toast::G_WARN);
        return;
    }
    gameprep::Out::Action a = o->action;
    g_gbCard.kind = L"-";  // к моменту, когда заглянете в пропущенные, игра уже идёт или кончилась
    g_gbCard.yesLabel = o->yes;
    g_gbCard.noLabel = L"Играть так";
    g_gbCard.onYes = [a] {
        g_gbCard.yesLabel.clear(), g_gbCard.noLabel.clear();
        // обновления откладываются в Параметрах (кнопка «Приостановить» - без прав), Steam - в его списке загрузок
        ShellExecuteW(nullptr, L"open",
                      a == gameprep::Out::OPEN_UPDATES ? L"ms-settings:windowsupdate" : L"steam://open/downloads",
                      nullptr, nullptr, SW_SHOWNORMAL);
    };
    g_gbCard.onNo = [] { g_gbCard.yesLabel.clear(), g_gbCard.noLabel.clear(); };
    g_gbCard.Show(BoostCard::OFFER, o->title, o->text, t, 20000, Toast::G_WARN);
}

void OfferBoost(bool manual, const std::wstring& why) {
    if (g_boostBusy) return;
    if (!manual && GetTickCount64() < g_offerQuietUntil) return;  // память освободили перед запуском сборки
    boost::Settings s = boost::Load(BoostDir());
    ThemeInfo t = CurrentTheme(g_cfg);
    if (!s.ok) {
        // Boost не настроен: свои силы - самые тяжёлые программы и мягкое «Закрыть»
        toolsui::MemoryOffer(manual ? L"" : why, KeepApps(), manual);
        return;
    }
    int used = 0;
    double avail = boost::AvailMb(&used);
    if (!manual && s.minFreePercent > 0 && 100 - used >= s.minFreePercent) return;  // памяти и так хватает
    g_boostGroups = boost::Scan(s);
    double mb = boost::TotalMb(g_boostGroups);
    if (g_boostGroups.empty() || (!manual && mb < 150)) {
        // в списках Boost закрывать нечего - может, тяжёлое что-то другое
        toolsui::MemoryOffer(manual ? L"" : why, KeepApps(), manual);
        return;
    }
    // профиль: для игры - по имени её exe, для редактора - по его имени
    std::wstring who;
    if (manual) {
        g_pickFor = L"вручную";
    } else if (g_game.On() && g_mcGame.on) {
        g_pickFor = L"mc:" + (g_mcGame.inst.id.empty() ? std::wstring(L"minecraft") : Lower(g_mcGame.inst.id));
        who = g_game.Reason();
    } else if (g_game.On()) {
        std::wstring exe = GameExe();
        g_pickFor = exe.empty() ? g_game.Reason() : gm::FileName(exe);
        who = g_game.Reason();
    } else {
        g_pickFor = L"unrealeditor";
        who = g_dev.Reason();
    }
    std::map<std::wstring, int> prof = gboost::Profile(g_pickFor);
    std::vector<BoostPick::Row> rows;
    bool remembered = false;
    for (const boost::Group& g : g_boostGroups) {
        BoostPick::Row r{g.title.empty() ? g.name : g.title, Lower(g.name), g.mb, DefaultAct(Lower(g.name))};
        auto it = prof.find(r.key);
        if (it != prof.end()) r.act = it->second, remembered = true;
        rows.push_back(r);
    }
    g_pick.onApply = [](const std::vector<BoostPick::Row>& r) { ApplyPick(r); };
    g_pick.onSkip = nullptr;
    g_pick.Show(manual ? L"Освободить память?" : L"Освободить память " + why + L"?",
                L"Сейчас свободно " + Size(avail) +
                    (remembered && !who.empty() ? L". Выбор - как в прошлый раз для " + who + L"."
                                                : L". «Усыпить» - программа остаётся открытой, но не мешает."),
                rows, t);
}

void BoostDone(boost::Result* r) {
    g_boostBusy = false;
    for (const auto& p : r->reopen)
        if (std::find(g_boostClosed.begin(), g_boostClosed.end(), p) == g_boostClosed.end()) g_boostClosed.push_back(p);
    std::wstring what;
    if (r->closed) what += L"Закрыто: " + std::to_wstring(r->closed);
    if (r->slept) what += (what.empty() ? L"Усыплено: " : L", усыплено: ") + std::to_wstring(r->slept);
    g_boostCard.Show(BoostCard::DONE, L"Освобождено " + Size(r->freedMb),
                     what + L". Свободно " + Size(boost::AvailMb()) +
                         (g_cfg.boostRestore && !r->reopen.empty() ? L". После игры закрытое можно будет вернуть."
                          : r->slept                               ? L". Усыплённые проснутся после игры."
                                                                   : L"."),
                     CurrentTheme(g_cfg));
    delete r;
    RunAfterBoost();
}

// Предложить вернуть то, что Boost закрыл перед игрой или редактором.
void OfferRestore() {
    if (!g_cfg.boostRestore || g_boostClosed.empty() || g_game.On() || g_dev.On()) return;
    std::vector<std::wstring> names;
    for (const auto& p : g_boostClosed) {
        std::wstring n = p.substr(p.find_last_of(L'\\') + 1);
        names.push_back(n.size() > 4 ? n.substr(0, n.size() - 4) : n);
    }
    g_boostCard.onYes = [] {
        boost::Reopen(g_boostClosed);
        boost::ClearSession(BoostDir());
        g_boostClosed.clear();
        g_boostCard.Show(BoostCard::DONE, L"Программы открываются", L"Они запускаются свёрнутыми.",
                         CurrentTheme(g_cfg));
    };
    g_boostCard.Show(BoostCard::RESTORE, L"Вернуть закрытые программы?", Names(names), CurrentTheme(g_cfg));
}

// Игра закрылась: приоритеты и таймер назад, ещё одна очистка, предложить
// вернуть закрытое.
void BoostAfterGame() {
    boost::Settings s = boost::Load(BoostDir());
    boost::GameTweaks(s, false);
    gboost::Restore(L'g');
    if (!g_dev.On()) gboost::Restore(L's');  // усыплённые для редактора спят, пока он открыт
    gboost::Quiet(false);
    if (g_cfg.boostAfter && s.purge) boost::RunPurge(s);
    OfferRestore();
}

std::wstring Duration(ULONGLONG ms) {
    int m = (int)(ms / 60000);
    wchar_t b[32];
    if (m >= 60)
        swprintf(b, 32, L"%d ч %d мин", m / 60, m % 60);
    else
        swprintf(b, 32, L"%d мин", std::max(1, m));
    return b;
}

std::wstring MinSec(double sec) {
    int s = (int)std::lround(sec);
    wchar_t b[32];
    if (s < 60)
        swprintf(b, 32, L"%d с", s);
    else
        swprintf(b, 32, L"%d:%02d", s / 60, s % 60);
    return b;
}

// Итог игры (или долгой работы в Unreal, если памяти не хватало): строка в
// историю и карточка.
void SessionSummary(const gboost::Session& s, bool save) {
    if (save) gboost::SaveSession(s);
    if (!g_cfg.boostSummary || s.ms < 3 * 60000 || s.samples < 6) return;
    bool tight = s.stallSamples >= 3 || s.pageGrowMb > 512;
    if (!s.game && !tight) return;  // в редакторе говорим только о проблемах
    // в карточку влезает четыре строки: сначала цифры, потом вывод
    std::wstring sub = L"Память до " + Size(s.peakUsedMb) + L" из " + Size(s.totalMb);
    if (s.peakVramMb > 1 && s.vramMb > 0) sub += L", видео до " + Size(s.peakVramMb) + L" из " + Size(s.vramMb);
    sub += L".";
    if (tight)
        sub += L" Не хватало: подкачка +" + Size(s.pageGrowMb) + L", диск мешал " +
               std::to_wstring(s.stallSamples * 5) + L" с - в следующий раз усыпляй больше.";
    else
        sub += L" Памяти хватало.";
    if (s.purges) sub += L" Кэш чистился " + Plural(s.purges, L"раз", L"раза", L"раз") + L".";
    g_gbCard.Show(BoostCard::DONE, (s.game ? L"Сыграно " : L"В Unreal ") + Duration(s.ms) + L": " + s.name, sub,
                  CurrentTheme(g_cfg), 16000, tight ? Toast::G_WARN : Toast::G_INFO);
}

bool WallpaperPaused(bool cfgFlag) { return cfgFlag && !gm::WallpaperEngine().empty(); }

void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// Один раз: исключения антивируса для папок Unreal - сборка и шейдеры заметно
// быстрее.
void ShowAvHint() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\av_hint.done", ad, MAX_PATH);
    if (Exists(ad) || !g_dev.On()) return;
    if (HANDLE f = CreateFileW(ad, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr); f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    std::wstring list = gboost::UnrealExclusions(EditorExe());
    g_gbCard.yesLabel = L"Скопировать список";
    g_gbCard.noLabel = L"Не нужно";
    g_gbCard.onNo = nullptr;
    g_gbCard.onYes = [list] {
        CopyText(list);
        g_gbCard.Show(BoostCard::DONE, L"Список в буфере",
                      L"Kaspersky: Настройки - Угрозы и исключения - Управление "
                      L"исключениями - Добавить. "
                      L"Папки и процессы - по одной строке.",
                      CurrentTheme(g_cfg), 16000);
    };
    g_gbCard.Show(BoostCard::OFFER, L"Ускорить сборки Unreal?",
                  L"Антивирус проверяет каждый файл компилятора и шейдеров. "
                  L"Исключения для папок движка, проектов и "
                  L"кэша ускоряют сборку. Список соберу, добавишь сам.",
                  CurrentTheme(g_cfg), 30000, Toast::G_INFO);
}

// Новости слежки Boost (поток gboost): видеокарта, видеопамять, шейдеры,
// сборки.
void OnBoostNote(const gboost::Note& n) {
    ThemeInfo t = CurrentTheme(g_cfg);
    switch (n.ev) {
        case gboost::EV_GPU_WRONG: {
            if (!g_cfg.boostGpu) return;
            size_t bar = n.sub.find(L'|');
            // «NVIDIA GeForce RTX 5050 Laptop GPU» -> «RTX 5050»: в карточку влезает четыре строки
            auto shortName = [](std::wstring g) {
                for (const wchar_t* cut :
                     {L"NVIDIA GeForce ", L"AMD Radeon(TM) ", L"(R)", L"(TM)", L" Laptop GPU", L" Graphics"})
                    for (size_t at; (at = g.find(cut)) != std::wstring::npos;) g.erase(at, wcslen(cut));
                return Trim(g);
            };
            std::wstring ig = shortName(n.sub.substr(0, bar)),
                         dg = bar == std::wstring::npos ? L"" : shortName(n.sub.substr(bar + 1));
            std::wstring exe = n.text;
            g_gbCard.yesLabel = L"Включить";
            g_gbCard.noLabel = L"Не нужно";
            g_gbCard.onNo = nullptr;
            g_gbCard.onYes = [exe, dg] {
                bool ok = gboost::PreferDiscrete(exe);
                g_gbCard.Show(BoostCard::DONE, ok ? L"Готово" : L"Не получилось",
                              ok ? L"При следующем запуске игра пойдёт на " + dg + L"."
                                 : L"Windows не дала записать настройку видеокарты.",
                              CurrentTheme(g_cfg));
            };
            g_gbCard.Show(BoostCard::OFFER, L"Игра на встроенной видеокарте",
                          L"«" + gm::FileName(exe) + L"» рисует на " + ig + L", а " + dg +
                              L" простаивает. «Включить» - со следующего запуска игра пойдёт на " + dg + L".",
                          t, 30000, Toast::G_WARN);
            return;
        }
        case gboost::EV_VRAM_HIGH:
            if (!g_cfg.devWatch) return;
            g_gbCard.Show(BoostCard::DONE, L"Видеопамять: " + Size(n.a) + L" из " + Size(n.b),
                          L"Unreal близок к «Out of video memory». Помогут Scalability пониже "
                          L"(Settings - Engine "
                          L"Scalability), один вьюпорт вместо двух и закрытое видео в браузере.",
                          t, 20000, Toast::G_WARN);
            return;
        case gboost::EV_SHADERS_START: {
            if (!g_cfg.devWatch) return;
            int eco = 0;
            if (g_cfg.boostEco)
                eco = gboost::BackgroundEco({L"UnrealEditor", L"ShaderCompileWorker", L"UnrealBuildTool", L"UbaHost",
                                             L"dotnet", L"cl", L"link"},
                                            boost::Load(BoostDir()).guarded);
            g_gbCard.Show(BoostCard::DONE, L"Компилируются шейдеры",
                          L"Работают " + Plural((int)n.a, L"процесс", L"процесса", L"процессов") +
                              (eco ? L", фон приглушён до конца." : L".") + L" Сообщу, когда закончится.",
                          t, 7000, Toast::G_INFO);
            return;
        }
        case gboost::EV_SHADERS_DONE:
            gboost::Restore(L'c');
            if (!g_cfg.devWatch || n.a < 15) return;
            g_gbCard.Show(BoostCard::DONE, L"Шейдеры готовы за " + MinSec(n.a),
                          L"Больше всего работало " + Plural((int)n.b, L"процесс", L"процесса", L"процессов") + L".", t,
                          7000, Toast::G_OK);
            return;
        case gboost::EV_BUILD_DONE:
            if (!g_cfg.devWatch) return;
            if (n.b > 0)
                g_gbCard.Show(BoostCard::DONE, L"Сборка готова за " + MinSec(n.a), n.sub, t, 7000, Toast::G_OK);
            else if (!n.go.empty()) {  // Л31: место ошибки известно - открыть его в VS Code
                std::wstring go = n.go;
                g_gbCard.yesLabel = L"Открыть в VS Code";
                g_gbCard.noLabel = L"Позже";
                g_gbCard.onYes = [go] {
                    g_gbCard.yesLabel.clear(), g_gbCard.noLabel.clear();
                    OpenCodeAt(&g_cfg, go);
                };
                g_gbCard.onNo = nullptr;
                g_gbCard.Show(BoostCard::OFFER, L"Сборка не удалась", n.text, t, 30000, Toast::G_FAIL);
            } else {
                g_gbCard.Show(BoostCard::DONE, L"Сборка не удалась",
                              n.text.empty() ? L"Подробности - в окне вывода." : n.text, t, 20000, Toast::G_FAIL);
            }
            return;
        case gboost::EV_PURGED:
            return;  // тихо: итог игры скажет, сколько раз
    }
}

// Для проверки без игры: карточки на выдуманных данных.
void BoostDemo(int what) {
    ThemeInfo t = CurrentTheme(g_cfg);
    if (what == 0) {
        g_pick.onApply = [](const std::vector<BoostPick::Row>&) {};
        g_pick.Show(L"Освободить память перед игрой?", L"Сейчас свободно 3.1 ГБ. Выбор - как в прошлый раз для NFS14.",
                    {{L"Google Chrome", L"chrome", 1840, 0},
                     {L"Discord", L"discord", 620, 1},
                     {L"Telegram Desktop", L"telegram", 310, 1},
                     {L"Microsoft Edge WebView2", L"msedgewebview2", 280, 2},
                     {L"Yandex.Disk", L"yandexdisk2", 120, 0}},
                    t);
    } else if (what == 1) {
        gboost::Session s;
        s.name = L"NFS14";
        s.ms = 102 * 60000;
        s.samples = 1200;
        s.peakUsedMb = 13500;
        s.totalMb = 15800;
        s.pageGrowMb = 1300;
        s.stallSamples = 7;
        s.peakVramMb = 7100;
        s.vramMb = 8188;
        s.purges = 3;
        bool was = g_cfg.boostSummary;
        g_cfg.boostSummary = true;
        g_gbCard.Show(BoostCard::DONE, L"", L"", t);  // сброс подписей
        SessionSummary(s, false);                     // выдуманная сессия - не в историю
        g_cfg.boostSummary = was;
    } else {
        auto* n = new gboost::Note{gboost::EV_GPU_WRONG, 40, 0, L"D:\\Games\\NFS14\\NFS14.exe",
                                   L"Intel(R) UHD Graphics|NVIDIA GeForce RTX 5050 Laptop GPU"};
        PostMessageW(g_wnd, gboost::WM_GBOOST, n->ev, (LPARAM)n);
    }
}

}  // namespace app
