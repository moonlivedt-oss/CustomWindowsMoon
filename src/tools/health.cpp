// Рост папок за неделю, здоровье SSD раз в неделю, свежий драйвер NVIDIA.
#include <winsock2.h>  // раньше windows.h: дальше net_internal.h, а winsock2 после windows.h - предупреждение
#include <windows.h>

#include <algorithm>
#include <cwctype>

#include <map>

#include "core/text.h"
#include "net/net_internal.h"
#include "tools/tools.h"

namespace tools {

namespace {

int Today() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int)(t / (24ull * 3600 * 10000000));
}

std::vector<std::wstring> ReadLines(const std::wstring& file) {
    std::vector<std::wstring> out;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string s;
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) s.append(buf, got);
    CloseHandle(f);
    for (const std::wstring& l : Split(Widen(s), L'\n'))
        if (!l.empty()) out.push_back(l);
    return out;
}

void WriteLines(const std::wstring& file, const std::vector<std::wstring>& lines) {
    std::string s;
    for (const std::wstring& l : lines) s += Narrow(l) + "\n";
    std::wstring tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(f);
    MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// части строки, разделённые табуляцией (Split обрезает пробелы - для путей это не годится)
std::vector<std::wstring> Tabs(const std::wstring& l) {
    std::vector<std::wstring> out;
    size_t a = 0;
    for (size_t b; (b = l.find(L'\t', a)) != std::wstring::npos; a = b + 1) out.push_back(l.substr(a, b - a));
    out.push_back(l.substr(a));
    return out;
}

}  // namespace

// ---------------- рост папок ----------------
// sizes.tsv: день<TAB>байты<TAB>имя<TAB>путь; на каждый день и путь - одна строка, хранится 40 дней.

void RecordSizes(const std::vector<std::pair<std::wstring, std::wstring>>& namePath,
                 const std::vector<ULONGLONG>& bytes) {
    std::wstring file = DataDir() + L"\\sizes.tsv";
    int today = Today();
    std::map<std::wstring, bool> fresh;
    for (const auto& np : namePath) fresh[Lower(np.second)] = true;
    std::vector<std::wstring> keep;
    for (const std::wstring& l : ReadLines(file)) {
        std::vector<std::wstring> p = Tabs(l);
        if (p.size() < 4) continue;
        int day = _wtoi(p[0].c_str());
        if (day < today - 40) continue;
        if (day == today && fresh.count(Lower(p[3]))) continue;  // сегодняшний замер заменяется новым
        keep.push_back(l);
    }
    for (size_t i = 0; i < namePath.size() && i < bytes.size(); ++i)
        keep.push_back(std::to_wstring(today) + L"\t" + std::to_wstring(bytes[i]) + L"\t" + namePath[i].first + L"\t" +
                       namePath[i].second);
    WriteLines(file, keep);
}

std::vector<Growth> WeekGrowth(int days) {
    int today = Today();
    struct Pt {
        int day;
        ULONGLONG bytes;
    };
    std::map<std::wstring, std::pair<std::wstring, std::vector<Pt>>> byPath;  // путь -> имя, замеры
    for (const std::wstring& l : ReadLines(DataDir() + L"\\sizes.tsv")) {
        std::vector<std::wstring> p = Tabs(l);
        if (p.size() < 4) continue;
        auto& e = byPath[Lower(p[3])];
        e.first = p[2];
        e.second.push_back({_wtoi(p[0].c_str()), _wcstoui64(p[1].c_str(), nullptr, 10)});
    }
    std::vector<Growth> out;
    for (auto& [path, e] : byPath) {
        std::vector<Pt>& pts = e.second;
        std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) { return a.day < b.day; });
        const Pt& last = pts.back();
        if (last.day < today - 1) continue;  // папку давно не меряли (удалена или больше не отслеживается)
        // самый ранний замер за последние days дней
        const Pt* base = nullptr;
        for (const Pt& p : pts)
            if (p.day >= today - days && p.day < last.day) {
                base = &p;
                break;
            }
        if (!base) continue;
        Growth g;
        g.name = e.first;
        g.path = path;
        g.now = last.bytes;
        g.delta = (LONGLONG)last.bytes - (LONGLONG)base->bytes;
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const Growth& a, const Growth& b) { return a.delta > b.delta; });
    return out;
}

// ---------------- прогноз места ----------------
// free.tsv: день<TAB>том<TAB>свободно байт; на день и том - одна строка, 60 дней.

void RecordFree() {
    std::wstring file = DataDir() + L"\\free.tsv";
    int today = Today();
    std::vector<Volume> vols = Volumes();
    std::map<std::wstring, bool> fresh;
    for (const Volume& v : vols)
        if (v.type == DRIVE_FIXED && v.total) fresh[Lower(v.root)] = true;
    std::vector<std::wstring> keep;
    for (const std::wstring& l : ReadLines(file)) {
        std::vector<std::wstring> p = Tabs(l);
        if (p.size() < 3) continue;
        int day = _wtoi(p[0].c_str());
        if (day < today - 60 || (day == today && fresh.count(Lower(p[1])))) continue;
        keep.push_back(l);
    }
    for (const Volume& v : vols)
        if (v.type == DRIVE_FIXED && v.total)
            keep.push_back(std::to_wstring(today) + L"\t" + v.root + L"\t" + std::to_wstring(v.free));
    WriteLines(file, keep);
}

std::vector<Forecast> SpaceForecast() {
    std::map<std::wstring, std::vector<FreePoint>> byVol;
    for (const std::wstring& l : ReadLines(DataDir() + L"\\free.tsv")) {
        std::vector<std::wstring> p = Tabs(l);
        if (p.size() < 3) continue;
        byVol[p[1]].push_back({_wtoi(p[0].c_str()), _wcstoui64(p[2].c_str(), nullptr, 10)});
    }
    std::vector<Forecast> out;
    for (auto& [root, pts] : byVol) {
        std::sort(pts.begin(), pts.end(), [](const FreePoint& a, const FreePoint& b) { return a.day < b.day; });
        Forecast f;
        f.root = root;
        f.free = pts.back().free;
        f.days = DaysLeft(pts, &f.perDay);
        out.push_back(f);
    }
    return out;
}

std::wstring ForecastText(const Forecast& f) {
    std::wstring d = f.root.substr(0, 2);
    if (f.days < 0) return d + L" не убывает";
    return d + L" закончится примерно через " + Plural(std::max(1, f.days), L"день", L"дня", L"дней") + L" (-" +
           Bytes((ULONGLONG)f.perDay) + L" в сутки)";
}

// ---------------- здоровье SSD ----------------
// health.tsv: имя<TAB>износ<TAB>температура<TAB>состояние - прошлый замер для сравнения.

std::wstring HealthWeekly(std::vector<Physical>& disks) {
    std::wstring file = DataDir() + L"\\health.tsv";
    std::map<std::wstring, std::vector<std::wstring>> prev;
    for (const std::wstring& l : ReadLines(file)) {
        std::vector<std::wstring> p = Tabs(l);
        if (p.size() >= 4) prev[p[0]] = p;
    }
    std::wstring say;
    std::vector<std::wstring> lines;
    for (const Physical& d : disks) {
        auto add = [&](const std::wstring& s) { say += (say.empty() ? L"" : L" ") + s; };
        if (d.health == 1) add(d.name + L": Windows предупреждает о его состоянии.");
        if (d.health >= 2) add(d.name + L": диск неисправен - скопируй важное.");
        if (d.temp >= 70) add(d.name + L": горячий, " + std::to_wstring(d.temp) + L" °C.");
        auto it = prev.find(d.name);
        if (it != prev.end() && d.wear >= 0) {
            int was = _wtoi(it->second[1].c_str());
            if (was >= 0 && d.wear - was >= 3)
                add(d.name + L": износ вырос с " + std::to_wstring(was) + L"% до " + std::to_wstring(d.wear) +
                    L"% за неделю.");
        }
        if (d.wear >= 80) add(d.name + L": износ " + std::to_wstring(d.wear) + L"% - пора думать о замене.");
        lines.push_back(d.name + L"\t" + std::to_wstring(d.wear) + L"\t" + std::to_wstring(d.temp) + L"\t" +
                        std::to_wstring(d.health));
    }
    if (!lines.empty()) WriteLines(file, lines);
    return say;
}

// ---------------- драйвер NVIDIA ----------------

namespace {

// установленный: DriverVersion «32.0.16.1074» -> последние пять цифр «61074» -> «610.74»
bool InstalledNvidia(std::wstring& gpu, std::wstring& ver) {
    const wchar_t* cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    for (int i = 0; i < 16; ++i) {
        wchar_t sub[8];
        swprintf(sub, 8, L"%04d", i);
        std::wstring key = std::wstring(cls) + L"\\" + sub;
        wchar_t prov[128] = L"", desc[256] = L"", dv[64] = L"";
        DWORD sz = sizeof(prov);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"ProviderName", RRF_RT_REG_SZ, nullptr, prov, &sz) !=
            ERROR_SUCCESS)
            continue;
        if (Lower(prov).find(L"nvidia") == std::wstring::npos) continue;
        sz = sizeof(desc);
        RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc", RRF_RT_REG_SZ, nullptr, desc, &sz);
        sz = sizeof(dv);
        RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion", RRF_RT_REG_SZ, nullptr, dv, &sz);
        std::wstring digits;
        for (wchar_t c : std::wstring(dv))
            if (iswdigit(c)) digits += c;
        if (digits.size() < 5) continue;
        digits = digits.substr(digits.size() - 5);
        gpu = desc;
        ver = digits.substr(0, 3) + L"." + digits.substr(3);
        return true;
    }
    return false;
}

std::string Get(const wchar_t* host, const std::wstring& path) {
    std::string body;
    net::HttpGet(host, path.c_str(), 8000, &body, nullptr);
    return body;
}

// <Name>...</Name><Value>N</Value> - значение по имени (без учёта регистра)
std::wstring LookupValue(const std::string& xml, const std::wstring& want) {
    std::wstring s = Widen(xml), w = Lower(want);
    for (size_t a = 0; (a = s.find(L"<Name>", a)) != std::wstring::npos;) {
        size_t b = s.find(L"</Name>", a);
        if (b == std::wstring::npos) break;
        std::wstring name = Lower(s.substr(a + 6, b - a - 6));
        size_t v = s.find(L"<Value>", b), ve = s.find(L"</Value>", b);
        if (name == w && v != std::wstring::npos && ve != std::wstring::npos) return s.substr(v + 7, ve - v - 7);
        a = b;
    }
    return L"";
}

std::wstring JsonField(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t a = js.find(k);
    if (a == std::string::npos) return L"";
    a = js.find('"', js.find(':', a) + 1);
    size_t b = a == std::string::npos ? a : js.find('"', a + 1);
    return b == std::string::npos ? L"" : Widen(js.substr(a + 1, b - a - 1));
}

double Num(const std::wstring& v) { return _wtof(v.c_str()); }

}  // namespace

Driver NvidiaDriver() {
    Driver d;
    if (!InstalledNvidia(d.gpu, d.installed)) return d;
    // серия по имени: «NVIDIA GeForce RTX 5050 Laptop GPU» -> «GeForce RTX 50 Series (Notebooks)»
    std::wstring name = d.gpu, l = Lower(name);
    bool laptop = l.find(L"laptop") != std::wstring::npos;
    std::wstring series;
    for (const wchar_t* fam : {L"rtx ", L"gtx "}) {
        size_t a = l.find(fam);
        if (a == std::wstring::npos) continue;
        std::wstring num;
        for (size_t i = a + 4; i < l.size() && iswdigit(l[i]); ++i) num += l[i];
        // регистр неважен: LookupValue сравнивает без него
        if (num.size() == 4) series = L"geforce " + std::wstring(fam) + num.substr(0, 2) + L" series";
    }
    if (series.empty()) return d;
    if (laptop) series += L" (notebooks)";
    std::wstring psid =
        LookupValue(Get(L"www.nvidia.com", L"/Download/API/lookupValueSearch.aspx?TypeID=2&ParentID=1"), series);
    if (psid.empty()) return d;
    std::wstring pfid =
        LookupValue(Get(L"www.nvidia.com", L"/Download/API/lookupValueSearch.aspx?TypeID=3&ParentID=" + psid), name);
    if (pfid.empty()) return d;
    std::string js =
        Get(L"gfwsl.geforce.com",
            L"/services_toolkit/services/com/nvidia/services/AjaxDriverService.php?func=DriverManualLookup"
            L"&psid=" +
                psid + L"&pfid=" + pfid + L"&osID=57&languageCode=1033&isWHQL=1&dch=1&sort1=0&numberOfResults=1");
    d.latest = JsonField(js, "Version");
    d.date = JsonField(js, "ReleaseDateTime");
    d.ok = !d.latest.empty();
    d.newer = d.ok && Num(d.latest) > Num(d.installed) + 0.001;
    return d;
}

}  // namespace tools
