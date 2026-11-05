// Порядок на Рабочем столе и в снимках: подсчёт, вопрос, перенос в архив по месяцам с описью и отмена.
#include <shlobj.h>

#include <atomic>
#include <ctime>
#include <memory>

#include "system/auto/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "cards/boostcard.h"
#include "helpers/tidy.h"
#include "system/journal.h"

namespace tidy {

namespace {

constexpr UINT WM_TIDY = WM_APP + 1;

const Config* g_cfg = nullptr;
HWND g_wnd = nullptr;
BoostCard g_card;
ULONGLONG g_checkAt = 0;
std::atomic<bool> g_busy{false};

struct Move {
    std::wstring from, to;
};

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring DataDir() { return Known(FOLDERID_RoamingAppData) + L"\\MoonDesk"; }

std::vector<std::wstring> Folders() {
    std::vector<std::wstring> f;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Desktop, &FOLDERID_Screenshots}) {
        std::wstring p = Known(*id);
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) f.push_back(p);
    }
    return f;
}

std::vector<Move> Plan() {
    std::vector<Move> out;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    for (const std::wstring& dir : Folders()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            ULONGLONG days = now > t ? (now - t) / (24ull * 3600 * 10000000) : 0;
            if (!Candidate(fd.cFileName, fd.dwFileAttributes, days)) continue;
            out.push_back({dir + L"\\" + fd.cFileName, MonthDir(dir, fd.ftLastWriteTime) + L"\\" + fd.cFileName});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

std::wstring Free(const std::wstring& p) {  // занято - «имя (2).ext»
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    size_t dot = p.find_last_of(L'.'), slash = p.find_last_of(L'\\');
    std::wstring stem = dot != std::wstring::npos && dot > slash ? p.substr(0, dot) : p;
    std::wstring ext = dot != std::wstring::npos && dot > slash ? p.substr(dot) : L"";
    for (int i = 2; i < 1000; ++i) {
        std::wstring q = stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW(q.c_str()) == INVALID_FILE_ATTRIBUTES) return q;
    }
    return L"";
}

void WriteText(const std::wstring& path, const std::wstring& t) {
    std::string u = Narrow(t);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
    CloseHandle(f);
}

std::wstring ReadText(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), 8 << 20), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    return Widen(u);
}

void DoMove(std::vector<Move> plan) {
    std::wstring manifest;
    int moved = 0;
    for (Move& m : plan) {
        SHCreateDirectoryExW(nullptr, m.to.substr(0, m.to.find_last_of(L'\\')).c_str(), nullptr);
        std::wstring to = Free(m.to);
        if (to.empty() || !MoveFileExW(m.from.c_str(), to.c_str(), 0)) continue;  // занят - пропускаем
        manifest += m.from + L"\t" + to + L"\n";
        ++moved;
    }
    if (!moved) return;
    time_t t = time(nullptr);
    std::wstring dir = DataDir() + L"\\tidy";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\" + std::to_wstring((long long)t) + L".tsv";
    WriteText(path, manifest);
    autos::Note(L"Порядок: в архив по месяцам - " + Plural(moved, L"файл", L"файла", L"файлов") +
                    L" (Рабочий стол, снимки экрана)",
                tools::UndoPack(L"tidy", {path}));
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, Known(FOLDERID_Desktop).c_str(), nullptr);
}

void Show(const std::vector<Move>& plan) {
    int desk = 0, shots = 0;
    std::wstring d = Lower(Known(FOLDERID_Desktop));
    for (const Move& m : plan) (Lower(m.from).rfind(d, 0) == 0 ? desk : shots)++;
    std::wstring where;
    if (desk) where = L"на Рабочем столе " + std::to_wstring(desk);
    if (shots) where += (where.empty() ? L"в снимках экрана " : L", в снимках ") + std::to_wstring(shots);
    g_card.kind = L"Порядок на столе";
    g_card.yesLabel = L"Разложить";
    g_card.noLabel = L"Не сейчас";
    g_card.onYes = [plan] {
        if (!g_busy.exchange(true))
            bg::Run([plan] {
                DoMove(plan);
                g_busy = false;
            });
    };
    g_card.onNo = nullptr;
    g_card.more =
        L"Файлы старше 2 недель уходят в папку «Архив\\ГГГГ-ММ» рядом (по месяцу файла). Ничего не "
        L"удаляется; ярлыки и папки не трогаются. Передумали - «Отменить» в журнале автоматики (студия, "
        L"Состояние - Автоматика).";
    g_card.Show(BoostCard::OFFER, L"Старые файлы: " + where, L"Разложить по месяцам в «Архив», чтобы не мешали?",
                CurrentTheme(*g_cfg), 30000, Toast::G_ASK);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_TIDY) return DefWindowProcW(h, msg, wp, lp);
    std::unique_ptr<std::vector<Move>> plan((std::vector<Move>*)lp);
    bool manual = wp != 0;
    if (plan->size() >= (manual ? 1u : 5u)) {
        Show(*plan);
    } else if (manual) {
        g_card.kind = L"-";
        g_card.Show(BoostCard::DONE, L"Порядок и так", L"Старых файлов на столе и в снимках нет", CurrentTheme(*g_cfg),
                    5000, Toast::G_OK);
    }
    return 0;
}

void Count(bool manual) {
    HWND w = g_wnd;
    bg::Run(
        [w, manual] {
            auto* p = new std::vector<Move>(Plan());
            if (!PostMessageW(w, WM_TIDY, manual, (LPARAM)p)) delete p;
        },
        THREAD_PRIORITY_BELOW_NORMAL);
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg) {
    g_cfg = cfg;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MoonRadialTidy";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    g_card.Create(inst);
    g_checkAt = GetTickCount64() + 15 * 60000;  // после запуска - не сразу
}

void Ask() { Count(true); }

void Tick() {
    g_card.Tick();
    ULONGLONG now = GetTickCount64();
    if (!g_checkAt || now < g_checkAt) return;
    g_checkAt = now + 3600000;
    std::wstring stamp = DataDir() + L"\\tidy\\asked.txt";
    long long last = _wtoi64(ReadText(stamp).c_str()), t = (long long)time(nullptr);
    if (t - last < 7 * 86400) return;  // раз в неделю
    CreateDirectoryW((DataDir() + L"\\tidy").c_str(), nullptr);
    WriteText(stamp, std::to_wstring(t));
    Count(false);
}

void Shutdown() {
    g_card.Destroy();
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
}

std::wstring Undo(const std::wstring& manifest) {
    int back = 0, total = 0;
    for (const std::wstring& line : Split(ReadText(manifest), L'\n')) {
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        ++total;
        std::wstring from = line.substr(0, tab), to = line.substr(tab + 1);
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES && MoveFileExW(to.c_str(), from.c_str(), 0))
            ++back;
    }
    return L"на место вернулось " + std::to_wstring(back) + L" из " + std::to_wstring(total);
}

}  // namespace tidy

namespace tidy {

std::wstring Diag() {
    std::wstring o;
    for (const std::wstring& dir : Folders()) {
        int n = 0;
        for (const Move& m : Plan()) n += m.from.rfind(dir + L"\\", 0) == 0;
        o += dir + L": " + std::to_wstring(n) + L"\n";
    }
    return o;
}

}  // namespace tidy
