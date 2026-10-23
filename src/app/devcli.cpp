#include "app/devcli.h"
#include "app/devcli_internal.h"
#include "app/autostart.h"
#include "app/toolsui.h"
#include "app/weekreport.h"

#include <atomic>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/bg.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/elevate.h"
#include "core/filedlg.h"
#include "core/files.h"
#include "core/procsnap.h"
#include "core/text.h"
#include "core/themes.h"
#include "app/afterwork.h"
#include "app/gameprep.h"
#include "app/resume.h"
#include "desk/appvol.h"
#include "desk/boost.h"
#include "desk/gamemode.h"
#include "desk/lockscreen.h"
#include "desk/gameboost.h"
#include "desk/minecraft.h"
#include "desk/monitors.h"
#include "desk/recent.h"
#include "desk/modefx.h"
#include "desk/ocr.h"
#include "desk/startbg.h"
#include "desk/whreg.h"
#include "gfx/color.h"
#include "menu/menu.h"
#include "net/net.h"
#include "net/wifi.h"
#include "gfx/qr.h"
#include "studio/studio.h"
#include "tools/builderr.h"
#include "tools/projver.h"
#include "tools/tools.h"

namespace devcli {

std::vector<std::wstring> Args() {
    std::vector<std::wstring> out;
    int n = 0;
    if (LPWSTR* v = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; ++i) out.push_back(v[i]);  // [0] - сам exe
        LocalFree(v);
    }
    return out;
}

// Значение после флага: «--ocr картинка.png» -> «картинка.png».
bool Value(const std::vector<std::wstring>& args, const wchar_t* flag, std::wstring& out) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) {
            out = args[i + 1];
            return true;
        }
    return false;
}

bool Has(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args)
        if (a == flag) return true;
    return false;
}

// Итог проверки - в файл рядом с exe (ocr_out.txt, lock_out.txt, game_out.txt).
void WriteOut(const wchar_t* name, const std::wstring& text) {
    std::string bytes = Narrow(text);
    HANDLE f = CreateFileW((ExeDir() + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
}

Config LoadOwnConfig() {
    Config c;
    LoadConfig(ExeDir() + L"\\radial.cfg", c);
    return c;
}

namespace {

// --ocr картинка.png - распознать текст в ocr_out.txt
int Ocr(const std::wstring& path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> png;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        png.resize(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, png.data(), (DWORD)png.size(), &got, nullptr);
        png.resize(got);
        CloseHandle(f);
    }
    std::wstring err, text = ocr::Recognize(png, &err);
    WriteOut(L"ocr_out.txt", L"error=[" + err + L"]\r\n" + text);
    return 0;
}

// --dlg путь - перевести открытый диалог «Открыть/Сохранить» в путь
int Dialog(const std::wstring& path) {
    HWND dlg = nullptr;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(h) || !filedlg::Is(h)) return TRUE;
            *reinterpret_cast<HWND*>(lp) = h;
            return FALSE;
        },
        (LPARAM)&dlg);
    if (!dlg || !filedlg::Go(dlg, path, (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0)) return 1;
    ULONGLONG until = GetTickCount64() + 1500;  // таймер возврата имени
    MSG m;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(10);
    }
    return 0;
}

// --explorer-reg / --notify-reg / --settings-reg / --search-reg / --all-reg - собрать .reg по теме (не применяя)
int Reg(const std::vector<std::wstring>& args) {
    ThemeInfo t = CurrentTheme(LoadOwnConfig());
    std::wstring out;
    if (Has(args, L"--explorer-reg")) out = whreg::ExplorerReg(t.bg, t.ac);
    if (Has(args, L"--notify-reg")) out = whreg::NotificationReg(t.bg, t.ac);
    if (Has(args, L"--settings-reg")) out = whreg::SettingsReg(t.bg, t.panel, t.ac);
    if (Has(args, L"--search-reg")) out = whreg::SearchReg(t.bg, t.ac);
    if (Has(args, L"--all-reg")) out = whreg::AllReg(t.bg, t.panel, t.ac, nullptr);
    return out.empty() ? 1 : 0;
}

// --lock-apply - собрать экран блокировки по теме и поставить, итог в lock_out.txt
int LockApply() {
    Gdip gdip;
    auto job = lockscreen::ApplyAsync(LoadOwnConfig(), nullptr);
    while (!job->done) Sleep(20);
    WriteOut(L"lock_out.txt", job->message);
    return job->ok ? 0 : 1;
}

// --game-test - игровой режим на 3 секунды, состояние системы до, во время и после - в game_out.txt
int GameTest() {
    auto state = [] {
        BOOL anim = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
        DWORD tr = 1, sz = sizeof(tr);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &tr, &sz);
        return L"anim=" + std::to_wstring(anim) + L" transparency=" + std::to_wstring(tr) + L"\r\n";
    };
    Config c = LoadOwnConfig();
    GameMode gm;
    std::wstring log = L"before: " + state();
    gm.SetManual(true, c);
    log += L"during: " + state();
    Sleep(3000);
    gm.SetManual(false, c);
    log += L"after: " + state();
    WriteOut(L"game_out.txt", log);
    return 0;
}

// Очистка всего, что пересоздаётся быстро (как «Всё сразу» в кольце): кэши шейдеров и Unreal не трогаются.
int Clean() {
    std::wstring s;
    const std::wstring nl = L"\r\n";
    ULONGLONG freed = 0;
    for (const tools::Junk& j : tools::FindJunk(true)) {
        if (j.slow) {
            s += L"пропущен (долго собирается заново): " + j.name + L" " + tools::Bytes(j.bytes) + nl;
            continue;
        }
        freed += tools::CleanJunk(j, [&](const std::wstring& line) { s += line + nl; });
    }
    s += L"Всего освобождено: " + tools::Bytes(freed) + nl;
    WriteOut(L"clean_out.txt", s);
    return 0;
}

}  // namespace

bool HasFlag(const wchar_t* flag) { return Has(Args(), flag); }

bool Run(int& exitCode) {
    std::vector<std::wstring> args = Args();
    std::wstring v;
    if (Value(args, L"--ocr", v)) {
        exitCode = Ocr(v);
    } else if (Value(args, L"--dlg", v)) {
        exitCode = Dialog(v);
    } else if (Has(args, L"--explorer-reg") || Has(args, L"--notify-reg") || Has(args, L"--settings-reg") ||
               Has(args, L"--search-reg") || Has(args, L"--all-reg")) {
        exitCode = Reg(args);
    } else if (Has(args, L"--start-apply")) {  // оформить Пуск Start11 по теме, как кнопка студии
        Gdip gdip;
        std::wstring msg;
        exitCode = startbg::Apply(CurrentTheme(LoadOwnConfig()), msg) ? 0 : 1;
    } else if (Has(args, L"--purge")) {  // очистка памяти; запускает задача планировщика с правами
        // какие области чистить - из запроса в HKCU; PurgeMain знает только свои имена, прочее пропускает
        boost::Settings s;
        s.areas = Split(elevate::Take(L"PurgeAreas"), L',');
        s.areas.erase(std::remove(s.areas.begin(), s.areas.end(), std::wstring()), s.areas.end());
        exitCode = boost::PurgeMain(s);
    } else if (Has(args, L"--crash-test")) {  // нарочно упасть: проверить, что дамп и строка ложатся в crash
        *(volatile int*)nullptr = 0;
        exitCode = 1;
    } else if (Has(args, L"--crash-decode")) {  // --crash-decode [строка]: адрес падения -> функция, в crash_out.txt
        size_t i = std::find(args.begin(), args.end(), L"--crash-decode") - args.begin();
        std::vector<std::wstring> lines;
        if (i + 1 < args.size()) {
            lines.push_back(args[i + 1]);
        } else {  // без строки - все падения из папки crash
            for (const crash::Report& r : crash::All(crash::Dir()))
                if (!r.what.empty()) lines.push_back(r.when + L"  " + r.what);
        }
        std::wstring out;
        for (const std::wstring& l : lines) {
            std::wstring fn = crash::Decode(l);
            out += l + L"\r\n    -> " + (fn.empty() ? L"нет карты символов этой сборки" : fn) + L"\r\n";
        }
        WriteOut(L"crash_out.txt", out.empty() ? L"падений нет\r\n" : out);
        exitCode = 0;
    } else if (Has(args, L"--week-report")) {  // --week-report out.png [demo]: недельный отчёт картинкой (П12)
        Gdip gdip;
        size_t i = std::find(args.begin(), args.end(), L"--week-report") - args.begin();
        std::wstring out = i + 1 < args.size() ? args[i + 1] : ExeDir() + L"\\week_report.png";
        bool demo = i + 2 < args.size() && args[i + 2] == L"demo";
        static Config cfg = LoadOwnConfig();
        if (demo) {
            weekreport::Data prev = weekreport::DemoData(true);
            exitCode = weekreport::Render(weekreport::DemoData(), &prev, UiPalette(cfg), out) ? 0 : 1;
        } else {
            toolsui::PrimeForSnapshot(&cfg);  // рост и неделя - как у работающей программы
            exitCode = weekreport::Render(weekreport::Collect(), nullptr, UiPalette(cfg), out) ? 0 : 1;
        }
    } else if (Has(args, L"--net-job")) {  // действие с правами для раздела «Сеть»; запускает задача планировщика
        exitCode = net::AdminMain();
    } else if (Has(args, L"--studio-snap")) {  // --studio-snap раздел вкладка файл.png [мс]: раздел студии в картинку
        Gdip gdip;
        size_t i = std::find(args.begin(), args.end(), L"--studio-snap") - args.begin();
        int page = i + 1 < args.size() ? _wtoi(args[i + 1].c_str()) : 0;
        int tab = i + 2 < args.size() ? _wtoi(args[i + 2].c_str()) : 0;
        std::wstring out = i + 3 < args.size() ? args[i + 3] : ExeDir() + L"\\studio_snap.png";
        int ms = i + 4 < args.size() ? _wtoi(args[i + 4].c_str()) : 2500;
        if (const char* sc = getenv("RADIAL_SCENE"))
            if (!strcmp(sc, "clean")) bg::Run([] { FakeClean(nullptr); });
        static Config snapCfg;
        if (page == 10 && !getenv("RADIAL_NOPRIME")) {  // RADIAL_NOPRIME - снять, как выглядит подсчёт  // «Состояние»:
                                                        // размеры считаются заранее, как в работающей программе
            snapCfg = LoadOwnConfig();
            toolsui::PrimeForSnapshot(&snapCfg);
        }
        if (page == 7 && tab == 6) {  // «Защита» в студии: находки - заранее, как после фоновой проверки
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            toolsui::PrimeSecurityForSnapshot();
        }
        exitCode = EditorSnapshot(GetModuleHandleW(nullptr), ExeDir() + L"\\radial.cfg", page, tab, out, ms);
    } else if (Has(args,
                   L"--tools-ring-snap")) {  // --tools-ring-snap файл.png [сектор] [demo]: кольцо утилит в картинку
        size_t i = std::find(args.begin(), args.end(), L"--tools-ring-snap") - args.begin();
        std::wstring out = i + 1 < args.size() ? args[i + 1] : ExeDir() + L"\\tools_ring.png";
        int root = i + 2 < args.size() ? _wtoi(args[i + 2].c_str()) : 0;
        bool demo = i + 3 < args.size() && args[i + 3] == L"demo";  // выдуманные пункты - эталон снимка
        SetEnvironmentVariableW(L"RADIAL_DEMO", L"1");              // цвета темы - без поправки на время суток
        _putenv("RADIAL_DEMO=1");
        OleInitialize(nullptr);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Gdip gdip;
        static Config cfg = LoadOwnConfig();
        static RadialMenu menu;
        if (menu.Create(GetModuleHandleW(nullptr))) {
            UINT dpi = GetDpiForSystem();
            while (!menu.Prewarm(&cfg, dpi, 1000)) {}
            toolsui::SnapRing(menu, &cfg, dpi, root, out, demo);
        }
        TerminateProcess(GetCurrentProcess(), 0);  // как --ring-bench: разбирать окна и кэши GDI+ незачем
    } else if (Has(args, L"--modefx-film")) {      // --modefx-film game|dev on|off out.png: кадры анимации режима
        Gdip gdip;
        size_t i = std::find(args.begin(), args.end(), L"--modefx-film") - args.begin();
        bool game = i + 1 >= args.size() || args[i + 1] != L"dev", on = i + 2 >= args.size() || args[i + 2] != L"off";
        std::vector<std::wstring> chips =
            game ? (on ? std::vector<std::wstring>{L"Красота выключена", L"Обои на паузе", L"Таймер 0.5 мс",
                                                   L"Игре высокий приоритет", L"Фон в экономии: 4",
                                                   L"Уведомления молчат"}
                       : std::vector<std::wstring>{L"Красота вернулась", L"Обои снова идут", L"Приоритеты как были"})
                 : (on ? std::vector<std::wstring>{L"Обои на паузе", L"Видеопамять", L"Шейдеры", L"Сборки UBT"}
                       : std::vector<std::wstring>{L"Обои снова идут", L"Фон как был"});
        std::wstring title = game ? (on ? L"Игровой режим: NFS14" : L"Игровой режим выключен")
                                  : (on ? L"Режим разработки: Unreal Editor" : L"Режим разработки выключен");
        exitCode = ModeFx::Film(game ? ModeFx::GAME : ModeFx::DEV, on, title, chips, CurrentTheme(LoadOwnConfig()),
                                i + 3 < args.size() ? args[i + 3] : ExeDir() + L"\\modefx_film.png");
    } else if (Has(args, L"--boost-diag")) {  // видеокарты, профили, история сессий -> boost_out.txt
        std::wstring o;
        for (const gboost::Adapter& a : gboost::Adapters())
            o += L"adapter: " + a.name + L" vram=" + std::to_wstring((int)a.vramMb) +
                 (a.integrated ? L" (встроенная)" : L"") + L"\n";
        for (const mc::Instance& in : mc::Instances())
            o += L"mc: " + in.id + L" | " + in.name + L" | " + std::to_wstring(in.maxMb) + L" МБ | " +
                 std::to_wstring(in.mods) + L" модов | pretouch=" + (in.preTouch ? L"1" : L"0") + L" | icon=" +
                 in.icon + L" | game=" + in.game + L"\n";
        o += std::wstring(L"prism: ") + mc::PrismExe() + L" | pagefile auto=" + (mc::PagefileAuto() ? L"1" : L"0") +
             L"\n";
        for (const gboost::Session& s : gboost::History(10))
            o += L"session: " + s.name + L" " + std::to_wstring(s.ms / 60000) + L" мин\n";
        WriteOut(L"boost_out.txt", o);
        exitCode = 0;
    } else if (Has(args, L"--scene-film")) {  // --scene-film clean|guard|guard0 out.png: лист кадров живой сцены
        Gdip gdip;
        size_t i = std::find(args.begin(), args.end(), L"--scene-film") - args.begin();
        exitCode = SceneFilm(i + 1 < args.size() ? args[i + 1] : L"clean",
                             i + 2 < args.size() ? args[i + 2] : ExeDir() + L"\\scene_film.png");
    } else if (Value(args, L"--sandbox-wsb", v)) {  // собрать .wsb для файла, не запуская песочницу
        // --check: режим проверки поведения с инструментами из папки защиты
        std::wstring tools = Has(args, L"--check") ? ExpandPath(LoadOwnConfig().securityDir) : L"";
        std::wstring err, path = tools::SandboxConfig(v, false, err, tools);
        WriteOut(L"sandbox_out.txt", path.empty() ? L"error: " + err : path);
        exitCode = path.empty() ? 1 : 0;
    } else if (Has(args, L"--ring-bench")) {
        size_t i = std::find(args.begin(), args.end(), L"--ring-bench") - args.begin();
        exitCode = RingBench(i + 1 < args.size() ? std::max(500, _wtoi(args[i + 1].c_str())) : 4000);
    } else if (Has(args, L"--forecast")) {  // прогноз места по free.tsv -> forecast_out.txt
        std::wstring o;
        for (const tools::Forecast& f : tools::SpaceForecast())
            o += tools::ForecastText(f) + L", свободно " + tools::Bytes(f.free) + L"\r\n";
        WriteOut(L"forecast_out.txt", o.empty() ? L"замеров нет (free.tsv пишется раз в 3 часа фоновым подсчётом)" : o);
    } else if (Value(args, L"--find-dir", v)) {  // --find-dir <имя>: поиск папок по таблицам MFT -> find_out.txt
        ULONGLONG t0 = GetTickCount64(), age = 0;
        std::vector<tools::FoundDir> found;
        bool any = tools::MftFindDirs(v, 30, found, &age);
        ULONGLONG t1 = GetTickCount64();
        std::vector<tools::FoundDir> again;
        tools::MftFindDirs(v, 30, again, nullptr);  // второй раз - из индекса в памяти
        ULONGLONG t2 = GetTickCount64();
        std::wstring o = any ? L"" : L"таблиц MFT нет\r\n";
        o += L"первый поиск " + std::to_wstring(t1 - t0) + L" мс (с загрузкой), повторный " + std::to_wstring(t2 - t1) +
             L" мс, таблице " + std::to_wstring(age / 60) + L" мин\r\n";
        for (const tools::FoundDir& f : found)
            o += f.path + L"\t" + tools::Bytes(f.bytes) + L"\t" + std::to_wstring(f.files) + L"\r\n";
        WriteOut(L"find_out.txt", o);
        exitCode = any ? 0 : 1;
    } else if (Has(args, L"--build-err")) {  // --build-err <cmake.exe> <папка сборки>: как «Найти ошибку» (Л31)
        size_t i = std::find(args.begin(), args.end(), L"--build-err") - args.begin();
        if (i + 2 < args.size()) {
            std::wstring out;
            int code = tools::RunCapture(L"\"" + args[i + 1] + L"\" --build \"" + args[i + 2] + L"\"", out, 600000);
            builderr::Error e;
            std::wstring o = L"код " + std::to_wstring(code) + L"\r\n";
            if (builderr::First(out, e))
                o += builderr::Plain(e) + L"\r\n" + builderr::Hint(e) + L"\r\n" + builderr::GotoArg(e, args[i + 2]);
            WriteOut(L"build_out.txt", o);
            exitCode = code;
        } else {
            exitCode = 2;
        }
    } else if (Value(args, L"--undelete", v)) {  // вернуть из Корзины (Л3); код - сколько вернулось
        exitCode = tools::RestoreRecycled({v});
    } else if (Value(args, L"--mft-test", v)) {  // --mft-test <папка>, из консоли администратора -> mft_out.txt
        exitCode = MftTest(v);
    } else if (Has(args, L"--clean")) {  // очистить кэши и временные файлы, итог в clean_out.txt
        exitCode = Clean();
    } else if (Has(args, L"--autostart-on")) {  // включить запуск вместе с Windows, как пункт трея
        SetAutostart(true);
        exitCode = AutostartOn() ? 0 : 1;
    } else if (Has(args, L"--tools-diag")) {
        exitCode = ToolsDiag();
    } else if (Has(args, L"--sec-diag")) {  // «Защита»: находки и подписи всего запущенного -> sec_out.txt
        exitCode = SecDiag();
    } else if (Has(args, L"--watch-diag")) {  // слежка, батарея, память; --slow - ещё версии и дубли
        exitCode = WatchDiag(Has(args, L"--slow"));
    } else if (Has(args, L"--snap-suite")) {  // --snap-suite <эталоны> <выход> [update]
        size_t i = std::find(args.begin(), args.end(), L"--snap-suite") - args.begin();
        if (i + 2 < args.size())
            exitCode = SnapSuite(args[i + 1], args[i + 2], i + 3 < args.size() && args[i + 3] == L"update");
        else
            exitCode = 2;
    } else if (Has(args, L"--monitors-diag")) {  // мониторы: DDC/CI, яркость, вход -> monitors_out.txt
        std::string u = Narrow(monitors::Diag());
        HANDLE f = CreateFileW((ExeDir() + L"\\monitors_out.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0,
                               nullptr);
        DWORD put = 0;
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
            CloseHandle(f);
        }
        exitCode = 0;
    } else if (Value(args, L"--projver-diag", v)) {  // Л33: версии проекта и что подставит терминал -> projver_out.txt
        std::wstring o;
        for (const tools::EnvPick& p : tools::ProjectPicks(v))
            o += p.tool + L": нужен " + tools::VerText(p.need.v) + (p.need.atLeast ? L"+" : L"") + L" (" + p.need.from +
                 L"), первый в PATH " + tools::VerText(p.first) + L", подставлю " + tools::VerText(p.use.v) + L" из " +
                 p.use.dir + L"\r\n";
        tools::TermEnv e = tools::DetectEnv(v);
        o += L"терминал: ";
        for (const std::wstring& w : e.what) o += w + L"; ";
        WriteOut(L"projver_out.txt", o + L"\r\n" + e.setup);
        exitCode = 0;
    } else if (Has(args, L"--gameprep-diag")) {  // Л21: что помешало бы игре сейчас -> gameprep_out.txt
        gameprep::In in;
        for (const PROCESSENTRY32W& p : *procsnap::Get(0))
            in.updates = in.updates || !_wcsicmp(p.szExeFile, L"TiWorker.exe");
        in.steam = afterwork::SteamDownloading();
        in.partials = afterwork::FreshPartials();
        tools::BatteryInfo bat = tools::Battery();
        in.battery = bat.present && !bat.ac;
        in.percent = bat.percent;
        gameprep::Out o;
        bool any = gameprep::Check(in, o);
        WriteOut(L"gameprep_out.txt",
                 std::wstring(L"обновления ") + (in.updates ? L"да" : L"нет") + L", Steam " +
                     (in.steam ? L"качает" : L"нет") + L", недокачанных " + std::to_wstring(in.partials) + L", заряд " +
                     std::to_wstring(in.percent) + (in.battery ? L"% от батареи" : L"% на зарядке") + L"\r\n" +
                     (any ? o.title + L"\r\n" + o.text + L"\r\n[" + o.yes + L"]" : L"мешать нечему"));
        exitCode = 0;
    } else if (Has(args, L"--resume-diag")) {  // Л4: что открыто сейчас и что запомнено -> resume_out.txt
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<resume::Thing> now = resume::Now();
        if (SUCCEEDED(co)) CoUninitialize();
        std::wstring o = L"сейчас: " + resume::Describe(now, 4000) + L"\r\n";
        for (const resume::Thing& t : now) o += L"  " + t.path + L"\r\n";
        WriteOut(L"resume_out.txt", o);
        exitCode = 0;
    } else if (Has(args, L"--appvol-diag")) {  // Л36: звуковые сеансы и запомненные уровни -> appvol_out.txt
        std::wstring o;
        for (const appvol::SessionNow& s : appvol::Current())
            o += s.exe + L" pid " + std::to_wstring(s.pid) + L" " + std::to_wstring((int)(s.level * 100 + 0.5f)) +
                 L"%\r\n";
        o += L"запомнено:\r\n" + appvol::SavedText(appvol::Saved());
        WriteOut(L"appvol_out.txt", o);
        exitCode = 0;
    } else if (Has(args, L"--devserv-diag")) {  // Л19: серверы разработки без порогов (час, 100 МБ) -> devserv_out.txt
        std::vector<tools::ProcInfo> ps = tools::Processes();
        std::map<std::wstring, ULONGLONG> seen;
        toolsui::FindForgotten(ps, seen, 0, 0);  // первая встреча - «видны сейчас»
        std::wstring o;
        for (const tools::Forgotten& f : toolsui::FindForgotten(ps, seen, 0, 0)) {
            o += f.project + L"\t" + tools::Bytes(f.bytes) + L"\r\n";
            for (const tools::DevServer& s : f.servers)
                o += L"  " + s.process + L" pid " + std::to_wstring(s.pid) + L" :" + std::to_wstring(s.port) + L"\r\n";
        }
        o += L"(проекты, видные в заголовках окон, не попадают; всего проектов с серверами: " +
             std::to_wstring(seen.size()) + L")\r\n";
        WriteOut(L"devserv_out.txt", o);
        exitCode = 0;
    } else if (Has(args, L"--recent-diag")) {  // недавние файлы и папки -> recent_out.txt
        std::wstring s;
        ULONGLONG t0 = GetTickCount64();
        std::vector<recent::Entry> list = recent::List();  // первый вызов только запускает чтение в фоне
        for (int i = 0; i < 100 && list.empty(); ++i) {
            Sleep(50);
            list = recent::List();
        }
        for (const recent::Entry& e : list) s += (e.dir ? L"[папка] " : L"") + e.path + L"\r\n";
        s += L"(" + std::to_wstring(GetTickCount64() - t0) + L" мс)\r\n";
        std::string u = Narrow(s);
        HANDLE f =
            CreateFileW((ExeDir() + L"\\recent_out.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD put = 0;
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
            CloseHandle(f);
        }
        exitCode = 0;
    } else if (Has(args, L"--sys-diag")) {  // восстановление, приватность, система, разработка -> sys_out.txt
        exitCode = SysDiag();
    } else if (Has(args, L"--watch-selftest")) {  // логика слежки на выдуманных данных -> selftest_out.txt
        exitCode = WatchSelftest();
    } else if (Value(args, L"--qr-dump",
                     v)) {  // --qr-dump <текст>: матрица QR (версия, маска, строки 0/1) -> qr_out.txt
        qr::Code c = qr::Encode(Narrow(v));
        std::wstring s = std::to_wstring(c.version) + L" " + std::to_wstring(c.mask) + L"\n";
        for (int y = 0; y < c.size; ++y) {
            for (int x = 0; x < c.size; ++x) s += c.At(x, y) ? L'1' : L'0';
            s += L"\n";
        }
        WriteOut(L"qr_out.txt", s);
    } else if (Has(args, L"--wifi-diag")) {  // Wi-Fi: сигнал, канал, соседи, есть ли пароль -> wifi_out.txt
        const std::wstring nl = L"\r\n";
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        net::WifiInfo w = net::Wifi(true);
        if (!w.ok && w.locationOff) {
            w.now.ssid = net::WifiProfileNow();
            w.ok = !w.now.ssid.empty();
        }
        std::wstring s = w.locationOff ? L"местоположение выключено: сигнал и соседи недоступны" + nl : L"";
        s += w.ok ? w.adapter + nl + L"сеть «" + w.now.ssid + L"» сигнал " + std::to_wstring(w.now.signal) + L"% " +
                        std::to_wstring(w.now.rssi) + L" дБм канал " + std::to_wstring(w.now.channel) + nl
                  : L"Wi-Fi не подключён" + nl;
        for (const net::WifiNet& n : w.all)
            s += L"  " + n.ssid + L" канал " + std::to_wstring(n.channel) + L" " + std::to_wstring(n.rssi) + L" дБм" +
                 (net::ChannelsOverlap(n.channel, w.now.channel) ? L" (мешает)" : L"") + nl;
        s += w.advice + nl;
        if (w.linkRx)
            s += L"связь с роутером " + std::to_wstring(w.linkRx) + L" / " + std::to_wstring(w.linkTx) + L" Мбит/с, " +
                 net::WifiBandGuess(w.linkRx) + nl;
        if (w.ok) {
            bool open = w.now.open;
            std::wstring pass = net::WifiPassword(w.now.ssid, &open);
            s += open           ? L"сеть открытая"
                 : pass.empty() ? L"пароль Windows не отдала"
                                : L"пароль есть, символов: " + std::to_wstring(pass.size());
            Gdip gdip;
            if (open || !pass.empty())
                s += net::WifiQrPng(w.now.ssid, pass, open, ExeDir() + L"\\wifi_qr.png") ? L", QR - wifi_qr.png"
                                                                                         : L", QR не вышел";
        }
        WriteOut(L"wifi_out.txt", s + nl);
    } else if (Has(args, L"--speed-test")) {
        exitCode = SpeedDiag();
    } else if (Has(args, L"--net-diag")) {
        exitCode = NetDiag();
    } else if (Has(args, L"--lock-apply")) {
        exitCode = LockApply();
    } else if (Has(args, L"--game-test")) {
        exitCode = GameTest();
    } else if (Has(args, L"--avatar")) {  // нарисовать аватар в %APPDATA%\MoonDesk\avatar.png
        Gdip gdip;
        std::wstring out, msg;
        exitCode = lockscreen::BuildAvatar(LoadOwnConfig(), out, msg) ? 0 : 1;
    } else {
        return false;
    }
    return true;
}

}  // namespace devcli
