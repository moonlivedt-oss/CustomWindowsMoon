// Сборки Minecraft: кольцо, запуск с Boost, падения, AlwaysPreTouch и подкачка.
#include "app/app_internal.h"

#include <algorithm>
#include <map>

#include "app/autos.h"
#include "app/toolsui.h"
#include "core/bg.h"
#include "core/files.h"
#include "core/fx.h"
#include "core/text.h"
#include "core/themes.h"
#include "gfx/sound.h"

namespace app {

// ---------------- Minecraft ----------------

// Отметки «не предлагать снова» в %APPDATA%\MoonDesk\mc.ini: pretouch-no:<id>,
// pagefile-said (день).
std::wstring McIni() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\mc.ini", p, MAX_PATH);
    return p;
}
std::wstring McGet(const std::wstring& key) {
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"mc", key.c_str(), L"", buf, 256, McIni().c_str());
    return buf;
}
void McPut(const std::wstring& key, const std::wstring& v) {
    WritePrivateProfileStringW(L"mc", key.c_str(), v.c_str(), McIni().c_str());
}
int Today() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 864000000000ull);
}

// Игра началась: если это Minecraft - слежка за логом и кучей, Prism - усыпить.
// true - Prism усыплён.
bool McBegin() {
    g_mcGame = McGame{};
    for (DWORD pid : g_game.GamePids()) {
        std::wstring path = gm::ProcessPath(pid), id;
        int xmx = 0;
        if (path.empty() || !mc::IsMinecraft(pid, path, id, xmx)) continue;
        g_mcGame.on = true;
        g_mcGame.pid = pid;
        if (id.empty() || !mc::FindInstance(id, g_mcGame.inst)) {
            wchar_t d[MAX_PATH];
            ExpandEnvironmentStringsW(L"%APPDATA%\\.minecraft", d, MAX_PATH);
            g_mcGame.inst.name = L"Minecraft";
            g_mcGame.inst.game = d;
        }
        if (xmx) g_mcGame.inst.maxMb = xmx;  // в командной строке - то, с чем игра запущена на самом деле
        mc::StartWatch(pid, path, g_mcGame.inst);
        break;
    }
    if (!g_mcGame.on || !g_cfg.boostEco) return false;
    std::vector<DWORD> prism = mc::PrismPids();
    if (prism.empty()) return false;
    gboost::Soften(prism);  // лаунчер во время игры не нужен; проснётся после неё
    return true;
}

// Игра кончилась: падение, итог, AlwaysPreTouch, подкачка.
void McEnd(const gboost::Session& s) {
    mc::Stats st = mc::StopWatch();
    mc::Instance in = g_mcGame.inst;
    g_mcGame = McGame{};
    gboost::SaveSession(s);
    ThemeInfo t = CurrentTheme(g_cfg);
    // 1. упала - важнее всего
    mc::Crash c;
    bool crashed = mc::NewCrash(in.game, st.startWall, c);
    if (crashed || st.oom) {
        std::wstring sub;
        if (crashed) sub = c.what + L".";
        if (!c.mods.empty()) sub += (c.jvm ? L" Где: " : L" Подозрение: ") + c.mods + L".";
        if (st.oom) sub += L" Java не хватило памяти (OutOfMemoryError).";
        std::wstring file = c.file;
        g_mcCard.yesLabel = crashed ? L"Открыть отчёт" : L"Понятно";
        g_mcCard.noLabel = L"Закрыть";
        g_mcCard.onNo = nullptr;
        g_mcCard.onYes = [file] {
            if (!file.empty()) ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        g_mcCard.Show(BoostCard::OFFER,
                      (c.jvm                ? L"Java упала: "
                       : st.oom && !crashed ? L"Не хватило памяти: "
                                            : L"Minecraft упал: ") +
                          in.name,
                      Trim(sub), t, 45000, Toast::G_FAIL);
    }
    // 2. итог
    if (g_cfg.boostSummary && s.ms >= 3 * 60000) {
        std::wstring sub;
        if (st.peakHeapMb > 0 && st.xmxMb > 0)
            sub = L"Куча Java до " + Size(st.peakHeapMb) + L" из " + Size(st.xmxMb) + L", память до " +
                  Size(s.peakUsedMb) + L" из " + Size(s.totalMb) + L".";
        else
            sub = L"Память до " + Size(s.peakUsedMb) + L" из " + Size(s.totalMb) + L".";
        if (st.lags) {
            int sec = (int)std::lround(st.lagMs / 1000);
            sub += L" Мир не успевал " + Plural(st.lags, L"раз", L"раза", L"раз") +
                   (sec > 0 ? L" (" + std::to_wstring(sec) + L" с)" : L"") + L".";
        }
        bool tight = s.stallSamples >= 3 || s.pageGrowMb > 512;
        if (tight)
            sub +=
                L" Подкачка +" + Size(s.pageGrowMb) + L", диск мешал " + std::to_wstring(s.stallSamples * 5) + L" с.";
        else if (!st.lags)
            sub += L" Памяти хватало.";
        g_gbCard.Show(BoostCard::DONE, L"Сыграно " + Duration(s.ms) + L": " + in.name, sub, t, 16000,
                      tight || st.lags > 20 ? Toast::G_WARN : Toast::G_INFO);
    }
    if (crashed) return;
    // 3. AlwaysPreTouch держит всю кучу с первой секунды, а сборке столько не
    // нужно
    if (in.preTouch && !in.id.empty() && st.heapSamples >= 2 && st.peakHeapMb > 0 && in.maxMb > 0 &&
        st.peakHeapMb < in.maxMb * 0.7 && McGet(L"pretouch-no:" + in.id).empty()) {
        std::wstring id = in.id, name = in.name;
        double spare = in.maxMb - st.peakHeapMb;
        g_mcCard2.yesLabel = L"Убрать флаг";
        g_mcCard2.noLabel = L"Оставить";
        g_mcCard2.onNo = [id] { McPut(L"pretouch-no:" + id, L"1"); };
        g_mcCard2.onYes = [id, name] {
            std::wstring err;
            bool ok = mc::RemovePreTouch(id, err);
            g_mcCard2.Show(BoostCard::DONE, ok ? L"AlwaysPreTouch убран: " + name : L"Не получилось",
                           ok ? L"Со следующего запуска Java будет занимать память по мере "
                                L"надобности. Копия настроек - instance.cfg.moon-backup."
                              : err,
                           CurrentTheme(g_cfg), 12000, ok ? Toast::G_OK : Toast::G_WARN);
        };
        g_mcCard2.Show(BoostCard::OFFER, L"Убрать AlwaysPreTouch у " + name + L"?",
                       L"Сборке хватало " + Size(st.peakHeapMb) + L" из " + Size(in.maxMb) +
                           L", а флаг забирает всё с первой секунды. Без него "
                           L"Windows останется ещё ~" +
                           Size(spare) + L".",
                       t, 40000, Toast::G_INFO);
        return;
    }
    // 4. подкачка «по выбору системы» росла прямо в игре - раз в неделю
    // предложить постоянный размер
    int said = _wtoi(McGet(L"pagefile-said").c_str());
    if (s.pageGrowMb > 512 && mc::PagefileAuto() && (!said || Today() - said >= 7)) {
        McPut(L"pagefile-said", std::to_wstring(Today()));
        g_mcCard2.yesLabel = L"Открыть";
        g_mcCard2.noLabel = L"Позже";
        g_mcCard2.onNo = nullptr;
        g_mcCard2.onYes = [] {
            ShellExecuteW(nullptr, nullptr, L"SystemPropertiesPerformance.exe", nullptr, nullptr, SW_SHOWNORMAL);
        };
        g_mcCard2.Show(BoostCard::OFFER, L"Подкачка росла прямо в игре",
                       L"+" + Size(s.pageGrowMb) +
                           L" за игру, отсюда фризы. Дополнительно - Виртуальная "
                           L"память - Изменить: на SSD размер "
                           L"16384 МБ, исходный = максимальный.",
                       t, 40000, Toast::G_INFO);
    }
}

std::wstring McNote(const mc::Instance& in) {
    std::wstring n = in.maxMb ? L"Память " + Size(in.maxMb) : L"Память по умолчанию";
    if (in.mods) n += L", " + Plural(in.mods, L"мод", L"мода", L"модов");
    if (in.preTouch) n += L", AlwaysPreTouch";
    return n + L". Сначала освобожу память, потом запущу.";
}

void McLaunchNow(const std::wstring& id, const std::wstring& name) {
    g_offerQuietUntil = GetTickCount64() + 180000;  // игра стартует - второй раз не спрашивать
    if (mc::Launch(id))
        g_mcCard.Show(BoostCard::DONE, L"Запускаю " + name, L"Prism откроется свёрнутым.", CurrentTheme(g_cfg), 4000,
                      Toast::G_OK);
    else
        g_mcCard.Show(BoostCard::DONE, L"Prism не найден",
                      L"Ищу prismlauncher.exe в "
                      L"%LOCALAPPDATA%\\Programs\\PrismLauncher и Program Files.",
                      CurrentTheme(g_cfg), 9000, Toast::G_FAIL);
}

// Сборка из кольца: сначала память (до запуска - пока AlwaysPreTouch ничего не
// забрал), потом запуск.
void McLaunch(const std::wstring& id) {
    mc::Instance in;
    if (!mc::FindInstance(id, in)) return;
    if (g_mcGame.on) {
        g_mcCard.Show(BoostCard::DONE, L"Minecraft уже запущен", g_mcGame.inst.name, CurrentTheme(g_cfg), 4000,
                      Toast::G_WARN);
        return;
    }
    if (g_boostBusy) return;
    std::wstring name = in.name;
    boost::Settings s = boost::Load(BoostDir());
    double avail = boost::AvailMb(), need = (in.maxMb ? in.maxMb : 4096) + 2500.0;  // куча, сама игра и запас
    if (!s.ok || avail >= need) return McLaunchNow(id, name);
    g_boostGroups = boost::Scan(s);
    if (g_boostGroups.empty()) return McLaunchNow(id, name);
    g_pickFor = L"mc:" + Lower(id);
    std::map<std::wstring, int> prof = gboost::Profile(g_pickFor);
    std::vector<BoostPick::Row> rows;
    for (const boost::Group& g : g_boostGroups) {
        BoostPick::Row r{g.title.empty() ? g.name : g.title, Lower(g.name), g.mb, DefaultAct(Lower(g.name))};
        auto it = prof.find(r.key);
        if (it != prof.end()) r.act = it->second;
        rows.push_back(r);
    }
    g_afterBoost = [id, name] { McLaunchNow(id, name); };
    g_pick.onApply = [](const std::vector<BoostPick::Row>& r) { ApplyPick(r); };
    g_pick.onSkip = [] { RunAfterBoost(); };  // «Не сейчас» - всё равно запустить
    g_pick.Show(L"Освободить память для " + name + L"?",
                L"Сборке нужно " + Size(in.maxMb ? in.maxMb : 4096) + L" сразу при запуске, свободно " + Size(avail) +
                    L". Потом запущу её.",
                rows, CurrentTheme(g_cfg));
}

void McAction(const std::wstring& key) {
    if (key.rfind(L"mc:", 0) == 0) return McLaunch(key.substr(3));
    if (key == L"mc-prism") {
        std::wstring exe = mc::PrismExe();
        if (!exe.empty()) ShellExecuteW(nullptr, nullptr, exe.c_str(), nullptr, ParentDir(exe).c_str(), SW_SHOWNORMAL);
    }
}

// Кольцо сборок у курсора: последние запущенные первыми.
void ShowMcRing() {
    if (g_menu.IsOpen()) return;
    std::vector<Item> ring;
    for (const mc::Instance& in : mc::Instances()) {
        if (ring.size() >= 9) break;
        Item it;
        it.name = in.name;
        it.iconFull = in.icon.empty() ? ExeDir() + L"\\icons\\game.png" : in.icon;
        it.special = 3;
        it.path = L"mc:" + in.id;
        it.note = McNote(in);
        ring.push_back(it);
    }
    Item prism;
    prism.name = L"Prism";
    prism.iconFull = ExeDir() + L"\\icons\\apps.png";
    prism.special = 3;
    prism.path = L"mc-prism";
    prism.note = L"Открыть лаунчер, ничего не запуская";
    ring.push_back(prism);
    g_menu.onNet = [](const std::wstring& key) { McAction(key); };
    g_menu.netStatus = [] { return L"Свободно " + Size(boost::AvailMb()); };
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_menu.ShowNet(&g_cfg, std::move(ring), pt, mi.rcWork, MonitorDpi(mon));
}

// Для проверки без игры: карточки Minecraft на выдуманных данных.
void McDemo(int what) {
    ThemeInfo t = CurrentTheme(g_cfg);
    if (what == 0) {
        ShowMcRing();
    } else if (what == 1) {
        g_mcCard.yesLabel = L"Открыть отчёт";
        g_mcCard.noLabel = L"Закрыть";
        g_mcCard.onYes = nullptr;
        g_mcCard.onNo = nullptr;
        g_mcCard.Show(BoostCard::OFFER, L"Minecraft упал: Beyond Depth",
                      L"Ticking entity: java.lang.NullPointerException. "
                      L"Подозрение: Create (create), Version: 0.5.1.",
                      t, 20000, Toast::G_FAIL);
    } else if (what == 2) {
        g_mcCard2.yesLabel = L"Убрать флаг";
        g_mcCard2.noLabel = L"Оставить";
        g_mcCard2.onYes = nullptr;
        g_mcCard2.onNo = nullptr;
        g_mcCard2.Show(BoostCard::OFFER, L"Убрать AlwaysPreTouch у Beyond Depth?",
                       L"Сборке хватало 5.1 ГБ из 7.9 ГБ, а флаг забирает всё с "
                       L"первой секунды. Без него Windows "
                       L"останется ещё ~2.8 ГБ.",
                       t, 20000, Toast::G_INFO);
    } else {
        g_gbCard.Show(BoostCard::DONE, L"Сыграно 2 ч 5 мин: Beyond Depth",
                      L"Куча Java до 5.1 ГБ из 7.9 ГБ, память до 14.2 ГБ из 15.4 "
                      L"ГБ. Мир не успевал 14 раз (38 с). "
                      L"Подкачка +1.6 ГБ, диск мешал 55 с.",
                      t, 20000, Toast::G_WARN);
    }
}

}  // namespace app
