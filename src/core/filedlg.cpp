#include "core/filedlg.h"

#include "core/files.h"

namespace filedlg {
namespace {

constexpr int kNameBox = 0x47C;   // cmb13: поле имени в «Открыть» (Vista и новее)
constexpr int kSaveEdit = 0x3E9;  // поле имени в «Сохранить как»: Edit в ComboBox внутри FloatNotifySink
constexpr int kNameEdit = 0x480;  // edt1: поле имени в старых диалогах

HWND g_edit = nullptr;   // куда вернуть текст после перехода
std::wstring g_restore;  // что вернуть
int g_tries = 0;

bool IsClass(HWND h, const wchar_t* cls) {
    wchar_t n[64];
    GetClassNameW(h, n, 64);
    return !wcscmp(n, cls);
}

// cls - нужный класс или nullptr; id 0x3E9 есть и у панели инструментов, поэтому класс важен
HWND FindById(HWND parent, int id, const wchar_t* cls = nullptr) {
    struct Ctx {
        int id;
        const wchar_t* cls;
        HWND found;
    } ctx{id, cls, nullptr};
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (GetDlgCtrlID(h) == c->id && (!c->cls || IsClass(h, c->cls))) {
                c->found = h;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&ctx);
    return ctx.found;
}

bool HasClass(HWND parent, const wchar_t* cls) {
    struct Ctx {
        const wchar_t* cls;
        bool found;
    } ctx{cls, false};
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t n[64];
            GetClassNameW(h, n, 64);
            if (!wcscmp(n, c->cls)) {
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&ctx);
    return ctx.found;
}

// Поле ввода имени: Edit внутри ComboBoxEx32 / ComboBox или сам Edit.
HWND NameEdit(HWND dlg) {
    HWND box = FindById(dlg, kNameBox);
    if (!box) {
        HWND e = FindById(dlg, kSaveEdit, L"Edit");
        if (e && IsClass(GetParent(e), L"ComboBox")) return e;
    }
    if (!box) box = FindById(dlg, kNameEdit);
    if (!box) return nullptr;
    if (IsClass(box, L"Edit")) return box;
    if (HWND e = FindWindowExW(box, nullptr, L"Edit", nullptr)) return e;
    if (HWND cb = FindWindowExW(box, nullptr, L"ComboBox", nullptr))
        if (HWND e = FindWindowExW(cb, nullptr, L"Edit", nullptr)) return e;
    return nullptr;
}

std::wstring GetText(HWND h) {
    wchar_t buf[1024] = {};
    DWORD_PTR r = 0;
    SendMessageTimeoutW(h, WM_GETTEXT, 1024, (LPARAM)buf, SMTO_ABORTIFHUNG, 300, &r);
    return buf;
}

void SetText(HWND h, const std::wstring& s) {
    DWORD_PTR r = 0;
    SendMessageTimeoutW(h, WM_SETTEXT, 0, (LPARAM)s.c_str(), SMTO_ABORTIFHUNG, 300, &r);
}

// Диалог переходит в папку не сразу: ждём, пока поле опустеет или в нём останется наш путь, и ставим имя.
void CALLBACK Restore(HWND, UINT, UINT_PTR id, DWORD) {
    if (!IsWindow(g_edit) || ++g_tries > 3) {
        KillTimer(nullptr, id);
        g_edit = nullptr;
        return;
    }
    SetText(g_edit, g_restore);
    SendMessageTimeoutW(g_edit, EM_SETSEL, 0, -1, SMTO_ABORTIFHUNG, 300, nullptr);
    if (g_tries >= 2) {  // диалог иногда стирает поле уже после перехода - ставим ещё раз
        KillTimer(nullptr, id);
        g_edit = nullptr;
    }
}

}  // namespace

bool Is(HWND w) {
    if (!w || !IsWindow(w)) return false;
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"#32770")) return false;
    // у диалога выбора файла есть вид папки; у обычных окон-сообщений его нет
    if (!HasClass(w, L"DUIViewWndClassName") && !HasClass(w, L"SHELLDLL_DefView")) return false;
    return NameEdit(w) != nullptr;
}

bool Go(HWND dlg, const std::wstring& path, bool isDir) {
    HWND edit = NameEdit(dlg);
    if (!edit) return false;
    std::wstring keep = GetText(edit);
    std::wstring folder = isDir ? path : ParentDir(path);
    if (folder.empty()) return false;
    // путь без кавычек и с завершающей чертой: диалог понимает его как папку, а не как имя файла
    if (folder.back() != L'\\') folder += L'\\';
    SetText(edit, folder);
    HWND ok = GetDlgItem(dlg, IDOK);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)ok);

    g_edit = edit;
    g_tries = 0;
    if (isDir) {
        g_restore = keep;
    } else {
        size_t s = path.find_last_of(L"\\/");
        g_restore = s == std::wstring::npos ? path : path.substr(s + 1);
    }
    SetTimer(nullptr, 0, 180, Restore);
    return true;
}

}  // namespace filedlg
