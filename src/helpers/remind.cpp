// Напоминание одним жестом: кольцо, файл напоминаний, карточка в срок (в игре - после неё).
#include <shlobj.h>

#include <algorithm>
#include <ctime>

#include "system/auto/autos.h"
#include "core/config.h"
#include "core/files.h"
#include "core/secrets.h"
#include "core/text.h"
#include "core/themes.h"
#include "cards/boostcard.h"
#include "helpers/remind.h"
#include "ring/menu.h"

namespace remind {

namespace {

const Config* g_cfg = nullptr;
RadialMenu* g_menu = nullptr;
bool (*g_quiet)() = nullptr;
BoostCard g_card;
std::vector<Note> g_list;
std::wstring g_caption;  // подпись, взятая из буфера при открытии кольца
ULONGLONG g_checkAt = 0;

long long Now() { return (long long)time(nullptr); }

std::wstring File() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    if (dir.empty()) return L"";
    dir += L"\\MoonDesk";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\reminders.tsv";
}

void Load() {
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 20), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    g_list = Parse(Widen(u));
}

void Persist() {
    std::string u = Narrow(Save(g_list));
    std::wstring path = File(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr) && put == u.size();
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring ClipText() {
    std::wstring t;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) return t;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            t.assign(p, wcsnlen(p, 2000));
            GlobalUnlock(h);
        }
    CloseClipboard();
    return secrets::Kind(t).empty() ? Caption(t) : L"";
}

Item Act(const std::wstring& name, const std::wstring& key, const std::wstring& note) {
    Item it;
    it.name = name;
    it.special = 3;
    it.path = key;
    it.note = note;
    it.iconFull = ExeDir() + L"\\icons\\tools-alarm.png";
    if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\more.png";
    return it;
}

void Add(long long when, const std::wstring& text) {
    g_list.push_back({when, text.empty() ? L"Напоминание" : text});
    std::sort(g_list.begin(), g_list.end(), [](const Note& a, const Note& b) { return a.when < b.when; });
    Persist();
    autos::Note(L"Напоминание на " + Clock(when, Now()) + L": " + text);
    g_card.kind = L"-";
    g_card.yesLabel.clear(), g_card.noLabel.clear();
    g_card.onYes = nullptr, g_card.onNo = nullptr;
    g_card.Show(BoostCard::DONE, L"Напомню в " + Clock(when, Now()), text.empty() ? L"Напоминание" : text,
                CurrentTheme(*g_cfg), 3500, Toast::G_OK);
}

long long AtHour(int h) {
    time_t now = time(nullptr);
    tm t{};
    localtime_s(&t, &now);
    if (h <= t.tm_hour) t.tm_mday += 1;  // завтра
    t.tm_hour = h;
    t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    return (long long)mktime(&t);
}

void Run(const std::wstring& key) {
    if (key.rfind(L"x:", 0) == 0) {  // отменить
        size_t i = (size_t)_wtoi(key.c_str() + 2);
        if (i < g_list.size()) {
            autos::Note(L"Напоминание отменено: " + g_list[i].text);
            g_list.erase(g_list.begin() + (long)i);
            Persist();
        }
        return;
    }
    if (key.rfind(L"h:", 0) == 0) return Add(AtHour(_wtoi(key.c_str() + 2)), g_caption);
    Add(Now() + _wtoi(key.c_str()) * 60ll, g_caption);
}

void Fire(const Note& n) {
    autos::Note(L"Напоминание: " + n.text);
    std::wstring text = n.text;
    g_card.kind = L"-";
    g_card.yesLabel = L"Готово";
    g_card.noLabel = L"Ещё 10 мин";
    g_card.onYes = [] {};
    g_card.onNo = [text] {
        g_list.push_back({Now() + 600, text});
        std::sort(g_list.begin(), g_list.end(), [](const Note& a, const Note& b) { return a.when < b.when; });
        Persist();
    };
    long long late = Now() - n.when;
    std::wstring sub = late > 300 ? L"(было на " + Clock(n.when, Now()) + L")" : L"";
    g_card.Show(BoostCard::OFFER, text, sub, CurrentTheme(*g_cfg), 0, Toast::G_INFO);
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg, RadialMenu* menu, bool (*quiet)()) {
    g_cfg = cfg;
    g_menu = menu;
    g_quiet = quiet;
    g_card.Create(inst);
    Load();
}

std::vector<Item> RingItems() {
    std::wstring cap = g_caption.empty() ? L"«Напоминание»" : L"«" + g_caption + L"»";
    std::vector<Item> ring = {Act(L"Через 10 мин", L"10", cap), Act(L"Через 20 мин", L"20", cap),
                              Act(L"Через час", L"60", cap)};
    time_t now = time(nullptr);
    tm t{};
    localtime_s(&t, &now);
    for (int h : Hours(t.tm_hour, t.tm_min)) {
        wchar_t s[16];
        swprintf(s, 16, L"К %02d:00", h);
        ring.push_back(Act(s, L"h:" + std::to_wstring(h), cap));
    }
    if (!g_list.empty()) {
        Item list =
            Act(L"Уже стоят (" + std::to_wstring(g_list.size()) + L")", L"", L"Щелчок по напоминанию - отменить");
        for (size_t i = 0; i < g_list.size() && i < 12; ++i)
            list.children.push_back(Act(Clock(g_list[i].when, Now()) + L" " + g_list[i].text,
                                        L"x:" + std::to_wstring(i), L"Отменить это напоминание"));
        ring.push_back(list);
    }
    return ring;
}

void ShowRing() {
    if (!g_menu || g_menu->IsOpen()) return;
    g_caption = ClipText();
    std::vector<Item> ring = RingItems();
    g_menu->onNet = [](const std::wstring& key) { Run(key); };
    std::wstring status = g_caption.empty() ? L"Напоминание (подпись берётся из буфера)" : L"Напомнить: " + g_caption;
    g_menu->netStatus = [status] { return status; };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForSystem();
    g_menu->ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

void Tick() {
    g_card.Tick();
    ULONGLONG t = GetTickCount64();
    if (g_list.empty() || t - g_checkAt < 1000) return;
    g_checkAt = t;
    long long now = Now();
    if (g_list.front().when > now) return;
    if (g_quiet && g_quiet()) return;                                          // в игре и при показе экрана - после
    if (g_card.Visible() && g_card.CurrentMode() == BoostCard::OFFER) return;  // по одному
    Note n = g_list.front();
    g_list.erase(g_list.begin());
    Persist();
    Fire(n);
}

void Shutdown() { g_card.Destroy(); }

}  // namespace remind
