// Приватность Windows с откатом (И18): переключатели как у O&O ShutUp10, только честные - каждый пишет известные
// значения реестра, а прежние (или «значения не было») кладёт в бэкап до первой правки. «Вернуть» - из бэкапа.
// Свои (HKCU) - сразу, без прав; для всего компьютера (HKLM) - задача с правами JOB_PRIVACY, бэкап у неё.
#include <windows.h>

#include <map>

#include "core/elevate.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

struct RegSet {
    HKEY root;
    const wchar_t* key;
    const wchar_t* name;
    DWORD priv;  // значение «приватно»
};

struct TweakDef {
    const wchar_t* id;
    const wchar_t* title;
    const wchar_t* why;
    bool admin;
    std::vector<RegSet> regs;
};

constexpr wchar_t kCdm[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
constexpr wchar_t kAdv[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

const std::vector<TweakDef>& Defs() {
    static const std::vector<TweakDef> defs = {
        {L"ads_id",
         L"Рекламный идентификатор",
         L"Программы и сайты из Магазина не получают общий номер для подбора рекламы.",
         false,
         {{HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 0}}},
        {L"start_ads",
         L"Реклама и «советы» в Пуске и Параметрах",
         L"Рекомендуемые приложения, предложения в Параметрах, советы после обновлений.",
         false,
         {{HKEY_CURRENT_USER, kCdm, L"SubscribedContent-338388Enabled", 0},
          {HKEY_CURRENT_USER, kCdm, L"SubscribedContent-338389Enabled", 0},
          {HKEY_CURRENT_USER, kCdm, L"SubscribedContent-353694Enabled", 0},
          {HKEY_CURRENT_USER, kCdm, L"SubscribedContent-353696Enabled", 0},
          {HKEY_CURRENT_USER, kCdm, L"SystemPaneSuggestionsEnabled", 0},
          {HKEY_CURRENT_USER, kCdm, L"SoftLandingEnabled", 0},
          {HKEY_CURRENT_USER, kAdv, L"Start_IrisRecommendations", 0}}},
        {L"tailored",
         L"Персональные советы по диагностике",
         L"Microsoft не подбирает подсказки и рекламу по данным диагностики.",
         false,
         {{HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy",
           L"TailoredExperiencesWithDiagnosticDataEnabled", 0}}},
        {L"web_search",
         L"Поиск в интернете из Пуска",
         L"Набранное в Пуске не уходит в Bing - поиск только по компьютеру.",
         false,
         {{HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BingSearchEnabled", 0},
          {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"CortanaConsent", 0}}},
        {L"copilot",
         L"Кнопка Copilot на панели задач",
         L"Убирает кнопку; сам Copilot можно открыть из Пуска.",
         false,
         {{HKEY_CURRENT_USER, kAdv, L"ShowCopilotButton", 0}}},
        {L"typing",
         L"Сбор набранного и рукописного текста",
         L"Словарь набора и почерка не собирается для «улучшения распознавания».",
         false,
         {{HKEY_CURRENT_USER, L"Software\\Microsoft\\InputPersonalization", L"RestrictImplicitTextCollection", 1},
          {HKEY_CURRENT_USER, L"Software\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection", 1},
          {HKEY_CURRENT_USER, L"Software\\Microsoft\\InputPersonalization\\TrainedDataStore", L"HarvestContacts", 0}}},
        {L"feedback",
         L"Просьбы оставить отзыв",
         L"Windows не спрашивает «Насколько вероятно, что вы порекомендуете...».",
         false,
         {{HKEY_CURRENT_USER, L"Software\\Microsoft\\Siuf\\Rules", L"NumberOfSIUFInPeriod", 0}}},
        {L"telemetry",
         L"Телеметрия - минимум",
         L"Данные диагностики - только обязательные (в Pro меньше нельзя). Для всего компьютера - нужны права.",
         true,
         {{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", 0}}},
        {L"recall",
         L"Recall: снимки экрана для ИИ",
         L"Windows не сохраняет снимки экрана для поиска «по памяти». Для всего компьютера - нужны права.",
         true,
         {{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsAI", L"DisableAIDataAnalysis", 1},
          {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsAI", L"TurnOffSavingSnapshots", 1}}},
        {L"activity",
         L"История действий",
         L"Открытые файлы и сайты не копятся в истории и не уходят в облако. Для всего компьютера - нужны права.",
         true,
         {{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"PublishUserActivities", 0},
          {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"UploadUserActivities", 0},
          {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"EnableActivityFeed", 0}}},
    };
    return defs;
}

const TweakDef* Find(const std::wstring& id) {
    for (const TweakDef& d : Defs())
        if (id == d.id) return &d;
    return nullptr;
}

bool Read(const RegSet& r, DWORD& v) {
    DWORD size = sizeof(v);
    REGSAM view = r.root == HKEY_LOCAL_MACHINE ? RRF_SUBKEY_WOW6464KEY : 0;
    return RegGetValueW(r.root, r.key, r.name, RRF_RT_REG_DWORD | view, nullptr, &v, &size) == ERROR_SUCCESS;
}

std::wstring BackupFile(bool admin) { return (admin ? elevate::DataDir() : DataDir()) + L"\\privacy-backup.tsv"; }

// бэкап: строка «корень<TAB>ключ<TAB>имя<TAB>было (1/0)<TAB>значение»; ключ строки - корень|ключ|имя
std::map<std::wstring, std::wstring> LoadBackup(bool admin) {
    std::map<std::wstring, std::wstring> out;
    HANDLE f =
        CreateFileW(BackupFile(admin).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string bytes;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    for (const std::wstring& line : Split(Widen(bytes), L'\n')) {
        std::vector<std::wstring> p = Split(line, L'\t');
        if (p.size() == 5) out[p[0] + L"|" + p[1] + L"|" + p[2]] = p[3] + L"\t" + p[4];
    }
    return out;
}

void SaveBackup(bool admin, const std::map<std::wstring, std::wstring>& b) {
    std::wstring text;
    for (const auto& [k, v] : b) {
        std::vector<std::wstring> p;
        size_t a = 0;
        for (size_t e; (e = k.find(L'|', a)) != std::wstring::npos && p.size() < 2; a = e + 1)
            p.push_back(k.substr(a, e - a));
        p.push_back(k.substr(a));
        if (p.size() == 3) text += p[0] + L"\t" + p[1] + L"\t" + p[2] + L"\t" + v + L"\n";
    }
    std::string bytes = Narrow(text);
    std::wstring file = BackupFile(admin), tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    BOOL ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring RootName(HKEY r) { return r == HKEY_LOCAL_MACHINE ? L"HKLM" : L"HKCU"; }

// Включить (priv) или вернуть один набор. Только в том процессе, у которого есть права на эти ключи.
bool Apply(const TweakDef& d, bool priv, const Log& log) {
    std::map<std::wstring, std::wstring> backup = LoadBackup(d.admin);
    bool ok = true;
    for (const RegSet& r : d.regs) {
        std::wstring bk = RootName(r.root) + L"|" + r.key + L"|" + r.name;
        REGSAM view = r.root == HKEY_LOCAL_MACHINE ? KEY_WOW64_64KEY : 0;
        if (priv) {
            DWORD old = 0;
            bool had = Read(r, old);
            if (!backup.count(bk)) backup[bk] = (had ? L"1\t" : L"0\t") + std::to_wstring(old);
            HKEY k;
            if (RegCreateKeyExW(r.root, r.key, 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &k, nullptr) !=
                ERROR_SUCCESS) {
                ok = false;
                continue;
            }
            ok = RegSetValueExW(k, r.name, 0, REG_DWORD, (const BYTE*)&r.priv, sizeof(DWORD)) == ERROR_SUCCESS && ok;
            RegCloseKey(k);
        } else {
            auto it = backup.find(bk);
            if (it == backup.end()) continue;  // не мы меняли - не трогаем
            bool had = it->second.rfind(L"1", 0) == 0;
            DWORD old = (DWORD)_wtoi(it->second.c_str() + 2);
            HKEY k;
            if (RegOpenKeyExW(r.root, r.key, 0, KEY_SET_VALUE | view, &k) == ERROR_SUCCESS) {
                LONG rc = had ? RegSetValueExW(k, r.name, 0, REG_DWORD, (const BYTE*)&old, sizeof(DWORD))
                              : RegDeleteValueW(k, r.name);
                ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) && ok;
                RegCloseKey(k);
            }
            backup.erase(it);
        }
    }
    SaveBackup(d.admin, backup);
    if (log) log(std::wstring(d.title) + (ok ? (priv ? L": включено." : L": как было.") : L": не всё получилось."));
    return ok;
}

}  // namespace

std::vector<Tweak> PrivacyTweaks() {
    std::vector<Tweak> out;
    std::map<std::wstring, std::wstring> mine = LoadBackup(false);
    for (const TweakDef& d : Defs()) {
        Tweak t;
        t.id = d.id;
        t.title = d.title;
        t.why = d.why;
        t.admin = d.admin;
        t.on = true;
        for (const RegSet& r : d.regs) {
            DWORD v = 0;
            if (!Read(r, v) || v != r.priv) t.on = false;
            t.ours = t.ours || mine.count(RootName(r.root) + L"|" + r.key + L"|" + r.name);
        }
        out.push_back(t);
    }
    return out;
}

bool SetPrivacyTweak(const std::wstring& id, bool priv, const Log& log) {
    const TweakDef* d = Find(id);
    if (!d || d->admin) return false;  // для всего компьютера - через JOB_PRIVACY
    return Apply(*d, priv, log);
}

int RevertPrivacy(const Log& log) {
    int n = 0;
    for (const TweakDef& d : Defs())
        if (!d.admin && Apply(d, false, nullptr)) ++n;
    if (log) log(L"Свои настройки приватности - как были до RadialMenu.");
    return n;
}

// Задача с правами: «on|id», «off|id» или «off|*» (всё своё для компьютера назад). id - только из таблицы.
bool PrivacyAdminMain(const std::wstring& req, const Log& log) {
    std::vector<std::wstring> p = Split(req, L'|');
    if (p.size() != 2 || (p[0] != L"on" && p[0] != L"off")) {
        log(L"Не понял запрос.");
        return false;
    }
    bool priv = p[0] == L"on", ok = true, any = false;
    for (const TweakDef& d : Defs()) {
        if (!d.admin || (p[1] != d.id && !(p[1] == L"*" && !priv))) continue;
        any = true;
        ok = Apply(d, priv, log) && ok;
    }
    if (!any) log(L"Такого переключателя нет.");
    return any && ok;
}

}  // namespace tools
