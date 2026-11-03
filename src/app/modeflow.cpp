#include "core/usage.h"
// Режимы со стороны программы: свёрнутая игра (Г2), «запомнить игру» (Г1), память в режиме разработки (Р1).
#include "app/app_internal.h"

#include <algorithm>
#include <map>

#include "core/fx.h"
#include "core/llmouse.h"
#include "core/procsnap.h"
#include "core/text.h"
#include "core/themes.h"
#include "modes/modeswitch.h"

namespace app {

BoostCard g_modeCard;

namespace {

GameFocus g_gameFocus;
ULONGLONG g_focusCheckAt = 0;
ULONGLONG g_memAt = 0, g_memHighSince = 0, g_memWarnedAt = 0;

// что уже спрашивали и получили «Не спрашивать»: по строке на ключ (game:имя.exe, gpu:путь)
std::wstring AskedFile() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    CreateDirectoryW(ad, nullptr);
    return std::wstring(ad) + L"\\modes_asked.txt";
}

std::string ReadSmall(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    if (size != INVALID_FILE_SIZE && size < (1u << 20)) {
        out.resize(size);
        if (!ReadFile(f, out.data(), size, &got, nullptr)) got = 0;
        out.resize(got);
    }
    CloseHandle(f);
    return out;
}

void ClearLabels() {
    g_modeCard.yesLabel.clear();
    g_modeCard.noLabel.clear();
}

}  // namespace

bool ModeAsked(const std::wstring& key) {
    std::wstring k = Lower(key);
    for (const std::wstring& l : Split(Widen(ReadSmall(AskedFile())), L'\n'))
        if (Lower(l) == k) return true;
    return false;
}

void ModeMarkAsked(const std::wstring& key) {
    if (ModeAsked(key)) return;
    std::string line = Narrow(Lower(key) + L"\r\n");
    HANDLE f = CreateFileW(AskedFile().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, line.data(), (DWORD)line.size(), &put, nullptr);
    CloseHandle(f);
}

// Поправить radial.cfg с диска (не g_cfg: в нём может держаться тема правила); перечитается сам за секунду.
void EditConfig(const std::function<void(Config&)>& change) {
    Config c;
    if (!LoadConfig(g_cfgPath, c)) return;
    change(c);
    SaveConfig(g_cfgPath, c);
}

void ModeFlowInit(HINSTANCE inst) { g_modeCard.Create(inst); }

void ModeFlowShutdown() { g_modeCard.Destroy(); }

bool GameInBackground() { return g_gameFocus.back; }

// Г2: игра идёт, но её свернули - углам и кольцам вернуть перехват мыши и эффекты; вернулись в игру - снова снять.
void GameBackTick(ULONGLONG now) {
    bool heavy = g_game.On() && !g_game.Light();
    if (!heavy || !g_cfg.gameBack) {
        if (heavy && g_gameFocus.back) {  // переключатель выключили, пока игра в фоне
            fx::SetGame(true);
            llmouse::Suspend(true);
        }
        g_gameFocus.Reset();
        return;
    }
    if (now < g_focusCheckAt) return;
    g_focusCheckAt = now + 500;
    DWORD fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg);
    bool was = g_gameFocus.back;
    bool back = g_gameFocus.Want(now, g_game.GameInFront(), fg == GetCurrentProcessId());
    if (back == was) return;
    if (back) usage::Hit(L"game_back");
    fx::SetGame(!back);
    llmouse::Suspend(!back);
    UpdateTrayTip();
}

// Г1: игру узнали по окну на весь экран - после игры предложить запомнить её в games (тогда режим включится сразу)
void GameLearnOffer(ULONGLONG playedMs) {
    std::wstring path = g_game.TakeGuessed();
    // Пару минут во весь экран - не игра: снимок экрана, заставка, показ слайдов. Такое не предлагаем запомнить,
    // иначе одно «Запомнить» превращает каждый снимок экрана в игровой режим.
    if (path.empty() || !g_cfg.gameLearn || playedMs < 3 * 60000) return;
    std::wstring exe = gm::FileName(path), key = L"game:" + exe;
    if (exe.empty() || ModeAsked(key) || gm::MatchAny(path, g_cfg.games)) return;
    g_modeCard.kind = L"Запомнить игру";
    g_modeCard.yesLabel = L"Это игра";
    g_modeCard.noLabel = L"Не игра";
    g_modeCard.onYes = [exe] {
        usage::Hit(L"game_learn");
        ClearLabels();
        EditConfig([&](Config& c) { c.games += (c.games.empty() ? L"" : L"|") + exe; });
        g_modeCard.Show(BoostCard::DONE, L"«" + exe + L"» - в списке игр",
                        L"В следующий раз игровой режим включится сразу при запуске. Список - в студии: Стол - "
                        L"Красота, «Игровой режим».",
                        CurrentTheme(g_cfg));
    };
    // «Не игра» - в исключения: раньше она лишь переставала спрашивать, а режим всё равно включался
    g_modeCard.onNo = [key, exe] {
        ClearLabels();
        ModeMarkAsked(key);
        EditConfig([&](Config& c) { c.gamesSkip += (c.gamesSkip.empty() ? L"" : L"|") + exe; });
    };
    g_modeCard.Show(BoostCard::OFFER, L"«" + exe + L"» - это игра?",
                    L"Игровой режим включился сам: программа была во весь экран. «Это игра» - в следующий раз режим "
                    L"включится сразу. «Не игра» - больше не будет включаться из-за неё.",
                    CurrentTheme(g_cfg), 30000);
}

// Режим включился по догадке (окно на весь экран, не по списку игр): сразу сказать почему и дать исправить одним
// нажатием - не ждать конца «игры», которая может оказаться экраном блокировки на два часа
void GameGuessOffer() {
    const std::wstring path = g_game.Guessed();
    if (path.empty() || g_game.Manual() || !g_cfg.gameLearn) return;
    std::wstring exe = gm::FileName(path), key = L"game:" + exe;
    if (exe.empty() || ModeAsked(key) || gm::MatchAny(path, g_cfg.games)) return;
    g_modeCard.kind = L"Игровой режим по догадке";
    g_modeCard.yesLabel = L"Не игра";
    g_modeCard.noLabel = L"Это игра";
    g_modeCard.onYes = [key, exe] {
        ClearLabels();
        ModeMarkAsked(key);
        EditConfig([&](Config& c) { c.gamesSkip += (c.gamesSkip.empty() ? L"" : L"|") + exe; });
        g_game.SetManual(false, g_cfg);  // выключить сейчас, не дожидаясь, пока программа уйдёт с экрана
        g_modeCard.Show(BoostCard::DONE, L"Игровой режим выключен",
                        L"«" + exe +
                            L"» больше не будет его включать. Вернуть - в студии: Стол - Красота, "
                            L"«Не игры».",
                        CurrentTheme(g_cfg), 6000);
    };
    g_modeCard.onNo = [key, exe] {
        ClearLabels();
        ModeMarkAsked(key);
        EditConfig([&](Config& c) { c.games += (c.games.empty() ? L"" : L"|") + exe; });
    };
    g_modeCard.Show(BoostCard::OFFER, L"Игровой режим: «" + exe + L"»",
                    L"Включился сам: программа во весь экран и рисует через видеокарту. Если это не игра - нажмите, "
                    L"режим выключится и больше не будет включаться из-за неё.",
                    CurrentTheme(g_cfg), 15000, Toast::G_INFO);
}

// Р1: в режиме разработки физическая память занята на 90% дольше 20 с - сказать, кто её занимает, и предложить
// освободить (тот же выбор, что при запуске редактора). Не чаще раза в 15 минут.
void DevMemTick(ULONGLONG now) {
    if (!g_dev.On() || !g_cfg.devMem || g_game.On() || g_dev.Busy()) {
        g_memHighSince = 0;
        return;
    }
    if (now < g_memAt) return;
    g_memAt = now + 5000;
    MEMORYSTATUSEX m{sizeof(m)};
    if (!GlobalMemoryStatusEx(&m) || m.dwMemoryLoad < 90) {
        g_memHighSince = 0;
        return;
    }
    if (!g_memHighSince) g_memHighSince = now;
    if (now - g_memHighSince < 20000 || (g_memWarnedAt && now - g_memWarnedAt < 15 * 60000)) return;
    g_memWarnedAt = now;
    // кто занимает: частная память по именам (у Chrome десятки процессов), кроме редактора и его рабочих
    std::map<std::wstring, std::pair<std::wstring, ULONGLONG>> by;
    for (const procsnap::Info& p : procsnap::Session()) {
        std::wstring low = Lower(p.name);
        if (gm::MatchAny(p.name, g_cfg.devApps) || low == L"shadercompileworker.exe" || low == L"unrealbuildtool.exe" ||
            low == L"ubahost.exe" || low == L"radialmenu.exe" || low == L"dwm.exe" || low == L"explorer.exe")
            continue;
        auto& e = by[low];
        if (e.first.empty()) e.first = p.name;
        e.second += p.priv;
    }
    std::vector<std::pair<std::wstring, ULONGLONG>> top;
    for (auto& [low, e] : by)
        if (e.second > 300ull << 20) top.push_back({low == L"javaw.exe" ? L"Minecraft" : e.first, e.second});
    // Minecraft первым: его куча - самый большой кусок, и закрыть его проще, чем браузер с вкладками
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        if ((a.first == L"Minecraft") != (b.first == L"Minecraft")) return a.first == L"Minecraft";
        return a.second > b.second;
    });
    std::wstring who;
    for (size_t i = 0; i < top.size() && i < 3; ++i) {
        std::wstring n = top[i].first;
        if (EndsWithI(n, L".exe")) n.resize(n.size() - 4);
        who += (i ? L", " : L"") + n + L" " + Size(top[i].second / 1048576.0);
    }
    // память занял сам редактор - закрывать нечего, «Освободить» ничего бы не дал
    if (who.empty()) return;
    g_modeCard.kind = L"Память в режиме разработки";
    g_modeCard.yesLabel = L"Освободить";
    g_modeCard.noLabel = L"Не сейчас";
    g_modeCard.onYes = [] {
        ClearLabels();
        g_offerQuietUntil = 0;
        OfferBoost(false, L"для " + g_dev.Reason());
    };
    g_modeCard.onNo = [] { ClearLabels(); };

    { if (g_modeCard.Show(BoostCard::OFFER, L"Память почти кончилась: " + std::to_wstring(m.dwMemoryLoad) + L"%",
                    g_dev.Reason() + L" скоро начнёт писать в подкачку и замедлится." +
                        (who.empty() ? L"" : L" Больше всего занимают: " + who + L"."),
                    CurrentTheme(g_cfg), 30000, Toast::G_WARN)) usage::Hit(L"dev_mem"); }
}

void ModeFlowTick(ULONGLONG now) {
    g_modeCard.Tick();
    GameBackTick(now);
    DevMemTick(now);
}

}  // namespace app
