#include "core/launch.h"

#include <shellapi.h>

#include "core/config.h"
#include "core/text.h"

std::wstring FindVsCode(const Config* c) {
    std::wstring p = ExpandPath(c->vscode);
    if (Exists(p)) return p;
    for (const wchar_t* cand :
         {L"%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe", L"%ProgramFiles%\\Microsoft VS Code\\Code.exe",
          L"%ProgramFiles(x86)%\\Microsoft VS Code\\Code.exe"}) {
        p = ExpandPath(cand);
        if (Exists(p)) return p;
    }
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"code.cmd", nullptr, MAX_PATH, buf, nullptr)) {
        p = ParentDir(ParentDir(buf)) + L"\\Code.exe";  // ...\bin\code.cmd -> ...\Code.exe
        if (Exists(p)) return p;
    }
    return L"code";
}

void OpenCodeAt(const Config* c, const std::wstring& at) {
    if (at.empty()) return;
    ShellExecuteW(nullptr, nullptr, FindVsCode(c).c_str(), (L"-g \"" + at + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void OpenTerminal(const Config* c, std::wstring dir) {
    if (!dir.empty() && dir.back() == L'\\') dir += L".";
    std::wstring t = Lower(c->terminal);
    wchar_t buf[MAX_PATH];
    bool hasWt = SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, buf, nullptr) != 0;
    if ((t.empty() || t == L"wt") && hasWt) {
        ShellExecuteW(nullptr, nullptr, L"wt.exe", (L"-d \"" + dir + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    } else if (t == L"cmd") {
        ShellExecuteW(nullptr, nullptr, L"cmd.exe", nullptr, dir.c_str(), SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, nullptr, L"powershell.exe", L"-NoExit", dir.c_str(), SW_SHOWNORMAL);
    }
}

// Windows не отдаёт фокус окну, запущенному из фонового процесса, и оно открывается позади.
// Нажатый Alt снимает эту блокировку: на миг становимся активными сами.
void UnlockForeground(HWND self) {
    INPUT in[4]{};
    for (INPUT& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_MENU;
    in[1].ki.wVk = in[2].ki.wVk = 0xE8;  // пустая клавиша, чтобы Alt не открыл меню окна
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(3, in, sizeof(INPUT));
    SetForegroundWindow(self);
    in[3].ki.wVk = VK_MENU;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in[3], sizeof(INPUT));
}
