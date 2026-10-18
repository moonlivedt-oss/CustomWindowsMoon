// Системные гиганты (И11) в программе: размеры считаются в фоне (обход Windows.old - секунды), точные - задачей
// с правами; действия - с вопросом и ходом в карточке, через задачу с правами (JOB_GIANTS) или окна Windows.
#include "app/toolsui_internal.h"

namespace toolsui {

namespace {

SRWLOCK g_giantLock = SRWLOCK_INIT;
std::vector<tools::Giant> g_giants;         // под g_giantLock
std::map<std::wstring, ULONGLONG> g_exact;  // под g_giantLock: размеры из замера с правами
std::atomic<ULONGLONG> g_giantsAt{0};
std::atomic<bool> g_giantsBusy{false};

void Recount() {
    if (g_giantsBusy.exchange(true)) return;
    if (!bg::Run(
            [] {
                AcquireSRWLockShared(&g_giantLock);
                std::map<std::wstring, ULONGLONG> exact = g_exact;
                ReleaseSRWLockShared(&g_giantLock);
                std::vector<tools::Giant> v = tools::SystemGiants(exact);
                AcquireSRWLockExclusive(&g_giantLock);
                g_giants = std::move(v);
                ReleaseSRWLockExclusive(&g_giantLock);
                g_giantsAt = GetTickCount64();
                g_giantsBusy = false;
            },
            THREAD_PRIORITY_BELOW_NORMAL))
        g_giantsBusy = false;
}

bool AdminJob(const std::wstring& job, const tools::Log& log, int timeoutMs) {
    elevate::Put(L"GiantJob", job);
    return net::RunAdmin(net::JOB_GIANTS, log, timeoutMs, &g_jobCancel);
}

}  // namespace

std::vector<tools::Giant> Giants() {
    if (!g_giantsAt || GetTickCount64() - g_giantsAt > 10 * 60 * 1000) Recount();
    AcquireSRWLockShared(&g_giantLock);
    std::vector<tools::Giant> v = g_giants;
    ReleaseSRWLockShared(&g_giantLock);
    return v;
}

bool GiantsCounting() { return g_giantsBusy; }

void GiantsMeasure() {
    RunJob(L"Системные гиганты", [](const tools::Log& log, std::wstring& res) {
        std::map<std::wstring, ULONGLONG> got;
        log(L"Меряю с правами: точки восстановления и закрытые папки прошлой Windows...");
        bool ok = AdminJob(
            L"read",
            [&](const std::wstring& s) {
                std::vector<std::wstring> f = Split(s, L'\t');
                if (f.size() >= 3 && f[0] == L"@giant")
                    got[f[1]] = _wcstoui64(f[2].c_str(), nullptr, 10);
                else
                    log(s);
            },
            10 * 60 * 1000);
        AcquireSRWLockExclusive(&g_giantLock);
        for (auto& [k, b] : got) g_exact[k] = b;
        ReleaseSRWLockExclusive(&g_giantLock);
        g_giantsAt = 0;
        ULONGLONG shadow = got.count(L"shadow") ? got[L"shadow"] : 0;
        res = ok ? L"Готово. Точки восстановления занимают " + tools::Bytes(shadow) + L"."
                 : L"Не получилось - подробности выше.";
        return ok;
    });
}

void GiantAct(const std::wstring& key, bool undo) {
    tools::Giant g;
    for (const tools::Giant& x : Giants())
        if (x.key == key) g = x;
    if (g.key.empty() && key == L"hiber" && undo) g.key = key, g.undo = L"hiber-on", g.name = L"Гибернация";
    if (g.key.empty()) return;
    std::wstring act = undo ? g.undo : g.action;
    if (act == L"page-open") {
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        ShellExecuteW(nullptr, nullptr, (std::wstring(sys) + L"\\SystemPropertiesPerformance.exe").c_str(), nullptr,
                      nullptr, SW_SHOWNORMAL);
        g_card.Show(BoostCard::DONE, L"Подкачка",
                    L"«Дополнительно» - «Виртуальная память» - «Изменить». Для игр и Unreal лучше оставить «по выбору "
                    L"системы» или не меньше 16 ГБ.",
                    Theme(), 20000, Toast::G_INFO);
        return;
    }
    if (act == L"cleanmgr") {
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        ShellExecuteW(nullptr, nullptr, (std::wstring(sys) + L"\\cleanmgr.exe").c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        g_card.Show(BoostCard::DONE, L"Дампы памяти",
                    L"В «Очистке диска» - «Очистить системные файлы», потом отметить «Файлы дампа памяти для "
                    L"системных ошибок».",
                    Theme(), 20000, Toast::G_INFO);
        return;
    }
    if (act == L"sys-clean") return SysClean();
    std::wstring title, sub, yes;
    int timeout = 120000;
    if (act == L"hiber-off") {
        title = L"Выключить гибернацию?";
        sub = L"Освободится " + tools::Bytes(g.bytes) +
              L". Пропадут «Гибернация» и быстрый запуск: включение станет на пару секунд дольше. Вернуть можно "
              L"здесь же.";
        yes = L"Выключить";
    } else if (act == L"hiber-on") {
        title = L"Вернуть гибернацию?";
        sub = L"Снова появится hiberfil.sys (около 40% памяти) и быстрый запуск.";
        yes = L"Вернуть";
    } else if (act == L"shadow-shrink") {
        title = L"Ограничить точки восстановления?";
        sub = L"Сейчас " + (g.bytes ? tools::Bytes(g.bytes) : std::wstring(L"неизвестно сколько")) +
              L". Под точки останется 5% диска, самые старые удалятся; свежие останутся.";
        yes = L"Ограничить";
    } else if (act == L"old-windows") {
        title = L"Убрать прошлую Windows?";
        sub = L"Освободится около " + tools::Bytes(g.bytes) +
              L". Откатиться к прошлой версии после этого будет нельзя. Встроенная очистка, несколько минут.";
        yes = L"Убрать";
        timeout = 50 * 60 * 1000;
    } else {
        return;
    }
    Ask(title, sub, yes, L"Не надо", [act, timeout, name = g.name] {
        RunJob(
            name,
            [act, timeout](const tools::Log& log, std::wstring& res) {
                std::wstring last;
                bool ok = AdminJob(
                    act,
                    [&](const std::wstring& s) {
                        last = s;
                        log(s);
                    },
                    timeout);
                g_giantsAt = 0;  // пересчитать
                autos::Note(L"Системные гиганты: " + last);
                res = ok ? last : L"Не получилось: " + last;
                return ok;
            },
            false, timeout > 120000);
    });
}

}  // namespace toolsui
