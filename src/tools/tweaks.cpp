// Приватность Windows с откатом (И18): вкладка студии - короткий список из общего каталога RadialShutUp
// (shutup/catalog): те же значения реестра, тот же бэкап в реестре и тот же откат, что в RadialShutUp.exe, - что
// поменяли здесь, там видно как «изменено здесь», и наоборот. Свои (HKCU) - сразу, без прав; для всего
// компьютера (HKLM) - задача с правами JOB_PRIVACY.
#include <windows.h>

#include "core/elevate.h"
#include "core/text.h"
#include "shutup/catalog.h"
#include "tools/tools.h"

namespace tools {

namespace {

// что показывает вкладка студии; весь каталог - в RadialShutUp.exe
const wchar_t* const kIds[] = {L"ads_id", L"start_ads", L"tailored",  L"web_search", L"copilot",
                               L"typing", L"feedback",  L"telemetry", L"recall",     L"activity"};

const shutup::Setting* Find(const std::wstring& id) {
    for (const wchar_t* k : kIds)
        if (id == k) return shutup::Find(id);
    return nullptr;  // только из списка вкладки: задача с правами не включит ничего другого
}

// старый бэкап вкладки (файлы до общего каталога) - один раз в бэкап в реестре
void ImportOld(bool machine) {
    static bool done[2] = {false, false};
    if (done[machine]) return;
    done[machine] = true;
    std::wstring dir = machine ? elevate::DataDir() : DataDir();
    shutup::ImportLegacy(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, dir + L"\\privacy-backup.tsv");
}

}  // namespace

std::vector<Tweak> PrivacyTweaks() {
    ImportOld(false);
    shutup::Backup user = shutup::LoadBackup(HKEY_CURRENT_USER), machine = shutup::LoadBackup(HKEY_LOCAL_MACHINE);
    std::vector<Tweak> out;
    for (const wchar_t* id : kIds) {
        const shutup::Setting* s = shutup::Find(id);
        if (!s) continue;
        shutup::Status st = shutup::StatusOf(*s, user, machine);
        Tweak t;
        t.id = s->id;
        t.title = s->title;
        t.why = s->why;
        t.admin = s->Machine();
        t.on = st.state == shutup::State::On;
        t.ours = st.ours;
        out.push_back(t);
    }
    return out;
}

bool SetPrivacyTweak(const std::wstring& id, bool priv, const Log& log) {
    const shutup::Setting* s = Find(id);
    if (!s || s->Machine()) return false;  // для всего компьютера - через JOB_PRIVACY
    ImportOld(false);
    return shutup::Apply(*s, priv, log);
}

int RevertPrivacy(const Log& log) {
    ImportOld(false);
    shutup::Backup user = shutup::LoadBackup(HKEY_CURRENT_USER), machine = shutup::LoadBackup(HKEY_LOCAL_MACHINE);
    int n = 0;
    for (const wchar_t* id : kIds) {
        const shutup::Setting* s = shutup::Find(id);
        if (s && !s->Machine() && shutup::StatusOf(*s, user, machine).ours && shutup::Apply(*s, false, nullptr)) ++n;
    }
    if (log) log(n ? L"Свои настройки приватности - как были до RadialMenu." : L"Своё возвращать нечего.");
    return n;
}

// Задача с правами: «on|id», «off|id» или «off|*» (всё своё для компьютера назад). id - только из списка вкладки.
bool PrivacyAdminMain(const std::wstring& req, const Log& log) {
    std::vector<std::wstring> p = Split(req, L'|');
    if (p.size() != 2 || (p[0] != L"on" && p[0] != L"off")) {
        log(L"Не понял запрос.");
        return false;
    }
    ImportOld(true);
    bool priv = p[0] == L"on", ok = true, any = false;
    for (const wchar_t* id : kIds) {
        const shutup::Setting* s = shutup::Find(id);
        if (!s || !s->Machine() || (p[1] != id && !(p[1] == L"*" && !priv))) continue;
        any = true;
        ok = shutup::Apply(*s, priv, log) && ok;
    }
    if (!any) log(L"Такого переключателя нет.");
    return any && ok;
}

}  // namespace tools
