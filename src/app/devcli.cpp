#include "app/devcli.h"

#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <vector>

#include "core/config.h"
#include "core/filedlg.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boost.h"
#include "desk/gamemode.h"
#include "desk/lockscreen.h"
#include "desk/ocr.h"
#include "desk/startbg.h"
#include "desk/whreg.h"

namespace devcli {

namespace {

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

// GDI+ на время одного режима.
struct Gdip {
    ULONG_PTR token = 0;
    Gdip() {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    }
    ~Gdip() { Gdiplus::GdiplusShutdown(token); }
};

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
        exitCode = boost::PurgeMain(boost::Load(boost::Dir(LoadOwnConfig().boostDir)));
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
