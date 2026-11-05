#include "core/config.h"

#include <algorithm>
#include <sstream>

#include <shlwapi.h>

#include "core/perf.h"
#include "core/text.h"

namespace {

const wchar_t* const kCornerKeys[4] = {L"tl", L"tr", L"bl", L"br"};

bool ParseBool(const std::wstring& v) { return v == L"1" || Lower(v) == L"true" || Lower(v) == L"yes"; }

bool ParseColor(const std::wstring& v, COLORREF& out) {
    if (v.size() != 7 || v[0] != L'#') return false;
    unsigned long x = wcstoul(v.c_str() + 1, nullptr, 16);
    out = RGB((x >> 16) & 0xFF, (x >> 8) & 0xFF, x & 0xFF);
    return true;
}

// Поле строки «item = подпись | путь | значок»: «|» внутри подписи или макроса (text a|b) пишется как «\|», переводы
// строк выбрасываются - иначе пункт развалится при следующем чтении.
std::wstring Field(const std::wstring& v) {
    std::wstring o;
    for (wchar_t ch : v) {
        if (ch == L'\r' || ch == L'\n') continue;
        if (ch == L'|') o += L'\\';
        o += ch;
    }
    return o;
}

// Разбор строки item: делит по «|», кроме «\|».
std::vector<std::wstring> SplitItem(const std::wstring& v) {
    std::vector<std::wstring> out(1);
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size() && v[i + 1] == L'|') {
            out.back() += L'|';
            ++i;
        } else if (v[i] == L'|') {
            out.emplace_back();
        } else {
            out.back() += v[i];
        }
    }
    for (std::wstring& part : out) part = Trim(part);
    return out;
}

// Имя группы в [скобках]: «settings», «@left» и прочие служебные имена (и всё, что начинается с «\») пишутся с «\»
// впереди - при чтении он снимается, и группа остаётся группой.
std::wstring SectionName(const std::wstring& name) {
    std::wstring n;
    for (wchar_t ch : name)
        if (ch != L'\r' && ch != L'\n') n += ch;
    std::wstring low = Lower(Trim(n));
    if (low == L"settings" || (!low.empty() && (low[0] == L'@' || low[0] == L'\\'))) n = L"\\" + n;
    return n;
}

std::wstring ColorText(COLORREF c) {
    wchar_t b[8];
    swprintf(b, 8, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

}  // namespace

bool ExeMatch(const std::wstring& exePath, const std::wstring& masks) {
    if (exePath.empty()) return false;
    const wchar_t* name = PathFindFileNameW(exePath.c_str());
    for (const std::wstring& m : Split(masks, L'|'))
        if (!m.empty() && PathMatchSpecW(name, m.c_str())) return true;
    return false;
}

void Resolve(Item& it) {
    it.full = ExpandPath(it.path);
    it.iconFull = ResolveIcon(it.icon);
    if (!it.iconFull.empty() && GetFileAttributesW(it.iconFull.c_str()) == INVALID_FILE_ATTRIBUTES) it.iconFull.clear();
    it.dir = IsFolder(it.full);
    for (Item& c : it.children) Resolve(c);
}

// Ключи [settings], которые читаются одной строкой: имя в radial.cfg -> поле Config. Ключи со своей логикой
// (цвета, углы, списки, перечисления) разбираются в LoadConfig ниже.
struct BoolKey {
    const wchar_t* key;
    bool Config::* field;
};
struct TextKey {
    const wchar_t* key;
    std::wstring Config::* field;
};
struct IntKey {
    const wchar_t* key;
    int Config::* field;
    int lo, hi;
};
const BoolKey kBoolKeys[] = {
    {L"desktop_only", &Config::desktopOnly},
    {L"skip_fullscreen", &Config::skipFullscreen},
    {L"inner_edges", &Config::innerEdges},
    {L"window_frame", &Config::windowFrame},
    {L"window_captions", &Config::windowCaptions},
    {L"game_auto", &Config::gameAuto},
    {L"game_fullscreen", &Config::gameFullscreen},
    {L"share_auto", &Config::shareAuto},
    {L"share_hide", &Config::shareHide},
    {L"game_learn", &Config::gameLearn},
    {L"game_back", &Config::gameBack},
    {L"dev_mem", &Config::devMem},
    {L"share_mask", &Config::shareMask},
    {L"game_win_anim", &Config::gameWinAnim},
    {L"game_transparency", &Config::gameTransparency},
    {L"game_wallpaper", &Config::gameWallpaper},
    {L"dev_auto", &Config::devAuto},
    {L"dev_wallpaper", &Config::devWallpaper},
    {L"dev_boost", &Config::devBoost},
    {L"lock_reveal", &Config::lockReveal},
    {L"boost_offer", &Config::boostOffer},
    {L"boost_restore", &Config::boostRestore},
    {L"boost_after", &Config::boostAfter},
    {L"boost_eco", &Config::boostEco},
    {L"boost_quiet", &Config::boostQuiet},
    {L"boost_islc", &Config::boostIslc},
    {L"boost_gpu", &Config::boostGpu},
    {L"boost_summary", &Config::boostSummary},
    {L"dev_watch", &Config::devWatch},
    {L"speed_auto", &Config::speedAuto},
    {L"net_watch", &Config::netWatch},
    {L"net_security", &Config::netSecurity},
    {L"net_changes", &Config::netChanges},
    {L"net_vpn_watch", &Config::netVpnWatch},
    {L"net_vpn_block", &Config::netVpnBlock},
    {L"transfer_gentle", &Config::transferGentle},
    {L"net_exit", &Config::netExit},
    {L"net_game", &Config::netGame},
    {L"auto_clean", &Config::autoClean},
    {L"crash_restart", &Config::crashRestart},
    {L"digest", &Config::digest},
    {L"auto_downloads", &Config::autoDownloads},
    {L"auto_power", &Config::autoPower},
    {L"phone_input", &Config::phoneInput},
    {L"phone_always", &Config::phoneAlways},
    {L"phone_screen", &Config::phoneScreen},
    {L"phone_files", &Config::phoneFiles},
    {L"bright_by_app", &Config::brightByApp},
    {L"winget_night", &Config::wingetNight},
    {L"winget_night_accept", &Config::wingetAccept},
    {L"auto_repair", &Config::autoRepair},
    {L"auto_report", &Config::autoReport},
    {L"auto_backup", &Config::autoBackup},
    {L"tools_disk_warn", &Config::toolsDiskWarn},
    {L"day_tint", &Config::dayTint},
    {L"hex_glass", &Config::hexGlass},
    {L"boot_portal", &Config::bootPortal},
    {L"wake_portal", &Config::wakePortal},
    {L"stroke", &Config::stroke},
    {L"palette", &Config::palette},
    {L"gamepad", &Config::gamepad},
    {L"clip_history", &Config::clipHistory},
    {L"clip_secrets", &Config::clipSecrets},
    {L"translate_google", &Config::translateGoogle},
    {L"win_drag", &Config::winDrag},
    {L"cpu_balance", &Config::cpuBalance},
    {L"no_sleep", &Config::noSleep},
    {L"long_watch", &Config::longWatch},
    {L"app_volume", &Config::appVolume},
    {L"blur", &Config::blur},
    {L"sounds", &Config::sounds},
    {L"tour_done", &Config::tourDone},
    {L"media", &Config::media},
    {L"media_toast", &Config::mediaToast},
    {L"window_fx", &Config::winFx},
    {L"window_ring", &Config::winRing},
    {L"selection_ring", &Config::selectionRing},
    {L"desk_double", &Config::deskDouble},
    {L"battery_glow", &Config::batteryGlow},
    {L"snip_card", &Config::snipCard},
    {L"night", &Config::night},
};
const TextKey kTextKeys[] = {
    {L"share_apps", &Config::shareApps},
    {L"games", &Config::games},
    {L"games_skip", &Config::gamesSkip},
    {L"games_light", &Config::gamesLight},
    {L"dev_busy", &Config::devBusy},
    {L"share_titles", &Config::shareTitles},
    {L"dev_apps", &Config::devApps},
    {L"security_dir", &Config::securityDir},
    {L"lock_source", &Config::lockSource},
    {L"boost_dir", &Config::boostDir},
    {L"boost_keep_fast", &Config::boostKeepFast},
    {L"net_vpn_route", &Config::netVpnRoute},
    {L"net_vpn_apps", &Config::netVpnApps},
    {L"phone_share", &Config::phoneShare},
    {L"bright_apps", &Config::brightApps},
    {L"winget_skip", &Config::wingetSkip},
    {L"avatar_source", &Config::avatarSource},
    {L"proc_rules", &Config::procRules},
    {L"no_sleep_apps", &Config::noSleepApps},
    {L"long_apps", &Config::longApps},
    {L"focus_apps", &Config::focusApps},
    {L"focus_auto", &Config::focusAuto},
    {L"folder_icon", &Config::folderIcon},
    {L"vscode", &Config::vscode},
    {L"terminal", &Config::terminal},
    {L"profile", &Config::profile},
    {L"library", &Config::library},
};
const IntKey kIntKeys[] = {
    {L"digest_hour", &Config::digestHour, 0, 23},      {L"downloads_days", &Config::downloadsDays, 3, 180},
    {L"net_interval", &Config::netInterval, 5, 600},   {L"clip_secret_sec", &Config::clipSecretSec, 10, 600},
    {L"long_min", &Config::longMin, 1, 120},           {L"focus_minutes", &Config::focusMinutes, 5, 240},
    {L"max_children", &Config::maxChildren, 4, 24},    {L"volume", &Config::volume, 0, 100},
    {L"corner_delay", &Config::cornerDelay, 50, 2000}, {L"battery_low", &Config::batteryLow, 5, 50},
    {L"night_from", &Config::nightFrom, 0, 23},        {L"night_to", &Config::nightTo, 0, 23},
    {L"cursor_size", &Config::cursorSize, 0, 128},     {L"profile_ask", &Config::profileAsk, 0, 1},
};

bool ApplySimpleKey(Config& cfg, const std::wstring& key, const std::wstring& val) {
    for (const BoolKey& k : kBoolKeys)
        if (key == k.key) return cfg.*k.field = ParseBool(val), true;
    for (const TextKey& k : kTextKeys)
        if (key == k.key) return cfg.*k.field = val, true;
    for (const IntKey& k : kIntKeys)
        if (key == k.key) return cfg.*k.field = std::clamp(_wtoi(val.c_str()), k.lo, k.hi), true;
    return false;
}

bool LoadConfig(const std::wstring& file, Config& out) {
    PERF_SCOPE("config.Load");
    HANDLE f =
        CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string text(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    ReadFile(f, text.data(), (DWORD)text.size(), &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text.erase(0, 3);  // BOM

    Config cfg;
    enum { kLeft = 1, kRight = 2, kBoth = 3, kApps = 4, kSets = 8 };
    std::vector<std::pair<int, Item>> items;
    int side = kBoth;
    Item* cur = nullptr;
    bool inSettings = false;
    std::wstringstream ws(Widen(text));
    std::wstring line;
    while (std::getline(ws, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        if (line.front() == L'[' && line.back() == L']') {
            std::wstring name = Trim(line.substr(1, line.size() - 2));
            std::wstring low = Lower(name);
            cur = nullptr;
            bool escaped = !name.empty() && name[0] == L'\\';  // \Settings - группа с таким именем (SectionName)
            if (escaped) {
                name.erase(0, 1);
                low = L"";
            }
            inSettings = low == L"settings";
            if (inSettings) continue;
            if (low == L"@left")
                side = kLeft;
            else if (low == L"@right")
                side = kRight;
            else if (low == L"@both")
                side = kBoth;
            else if (low == L"@apps")
                side = kApps;
            else if (low == L"@sets")
                side = kSets;
            else {
                items.push_back({side, {}});
                cur = &items.back().second;
                cur->name = name;
            }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq)));
        std::wstring val = Trim(line.substr(eq + 1));

        if (inSettings) {
            if (ApplySimpleKey(cfg, key, val)) continue;  // однострочные ключи - из таблиц выше
            if (key == L"motion")
                cfg.motion = Lower(val) == L"calm" ? 0 : Lower(val) == L"lively" ? 2 : 1;
            else if (key == L"lock_pattern")
                cfg.lockPattern = Lower(val) == L"none" ? 0 : Lower(val) == L"shards" ? 2 : 1;
            else if (key == L"lock_anim")
                cfg.lockPortal = Lower(val) != L"fade";
            else if (key == L"palette_key")
                cfg.paletteKey = Lower(val);
            else if (key == L"rule")
                cfg.rules.push_back(val);
            else if (key == L"scale")
                cfg.scale = std::clamp(_wtof(val.c_str()), 0.5, 3.0);
            else if (key == L"accent") {
                cfg.accentAuto = Lower(val) == L"auto";
                if (!cfg.accentAuto) ParseColor(val, cfg.accentManual);
            } else if (key == L"accent_color")
                ParseColor(val, cfg.accentManual);
            else if (key == L"theme")
                cfg.theme = Lower(val);
            else if (key == L"custom_bg")
                ParseColor(val, cfg.customBg);
            else if (key == L"custom_accent")
                ParseColor(val, cfg.customAccent);
            else if (key == L"custom_accent2")
                ParseColor(val, cfg.customAccent2);
            else if (key == L"custom_gold")
                ParseColor(val, cfg.customGold);
            else if (key.size() > 3 && (key.rfind(L"corner_", 0) == 0 || key.rfind(L"corner2_", 0) == 0 ||
                                        key.rfind(L"wheel_", 0) == 0)) {
                std::wstring side = key.substr(key.size() - 2), kind = key.substr(0, key.size() - 2);
                for (int i = 0; i < 4; ++i) {
                    if (side != kCornerKeys[i]) continue;
                    if (kind == L"corner_") cfg.corners[i] = val;
                    if (kind == L"corner2_") cfg.corners2[i] = val;
                    if (kind == L"wheel_") cfg.cornerWheel[i] = val;
                }
            } else if (key == L"cursor_color") {
                COLORREF tmp;
                cfg.cursorColor = ParseColor(val, tmp) ? val : L"";
            }
            continue;
        }
        if (!cur) continue;
        if ((key == L"exe" && side == kApps) || (key == L"layout" && side == kSets))
            cur->note = val;  // маски exe кольца программы или раскладка набора; до конца разбора лежат в note
        else if (key == L"icon")
            cur->icon = val;
        else if (key == L"path")
            cur->path = val;
        else if (key == L"expand")
            cur->expand = ParseBool(val);
        else if (key == L"files")
            cur->files = ParseBool(val);
        else if (key == L"item") {
            auto parts = SplitItem(val);
            if (parts.size() < 2 || parts[1].empty()) continue;
            Item child;
            child.path = parts[1];
            child.name = parts[0].empty() ? PathFindFileNameW(ExpandPath(parts[1]).c_str()) : parts[0];
            if (parts.size() > 2) child.icon = parts[2];
            cur->children.push_back(std::move(child));
        }
    }
    for (auto& [s, it] : items) {
        Resolve(it);
        if (s == kSets) {
            WorkSet w;
            w.layout = it.note;
            it.note.clear();
            w.ring = std::move(it);
            cfg.sets.push_back(std::move(w));
            continue;
        }
        if (s == kApps) {
            AppRing a;
            a.exes = it.note;
            it.note.clear();
            a.ring = std::move(it);
            cfg.apps.push_back(std::move(a));
            continue;
        }
        if (s & kLeft) cfg.left.push_back(it);
        if (s & kRight) cfg.right.push_back(it);
    }
    cfg.accent = cfg.accentManual;
    cfg.folderIconFull = ResolveIcon(cfg.folderIcon);
    if (GetFileAttributesW(cfg.folderIconFull.c_str()) == INVALID_FILE_ATTRIBUTES) cfg.folderIconFull.clear();
    out = std::move(cfg);
    return true;
}

bool SaveConfig(const std::wstring& file, const Config& c) {
    PERF_SCOPE("config.Save");
    std::wstringstream o;
    o << L"# Radial Menu: настройки и состав колец. Файл в UTF-8.\n"
         L"# Удобнее править через редактор (значок в трее -> Редактор колец).\n"
         L"#\n"
         L"# [@left] / [@right] - дальше идут секторы левого или правого края.\n"
         L"# Каждая секция [Имя] - сектор первого кольца (сверху вниз):\n"
         L"#   icon   = PNG/WebP (путь от папки программы или полный), пусто - системный значок\n"
         L"#   path   = папка, файл, ярлык, программа, ссылка; можно %VAR% и {desktop} {downloads}\n"
         L"#            {documents} {music} {pictures} {videos} {home}\n"
         L"#   expand = 1 - второе кольцо из содержимого path;  files = 1 - и обычные файлы тоже\n"
         L"#   item   = Подпись | путь | значок   - ручной список второго кольца\n\n";
    o << L"[settings]\n";
    o << L"desktop_only = " << (c.desktopOnly ? 1 : 0) << L"\n";
    o << L"skip_fullscreen = " << (c.skipFullscreen ? 1 : 0) << L"\n";
    o << L"inner_edges = " << (c.innerEdges ? 1 : 0) << L"\n";
    o << L"scale = " << c.scale << L"\n";
    o << L"# auto - цвет берётся из обоев\n";
    o << L"accent = " << (c.accentAuto ? L"auto" : ColorText(c.accentManual)) << L"\n";
    o << L"accent_color = " << ColorText(c.accentManual) << L"\n";
    o << L"max_children = " << c.maxChildren << L"\n";
    o << L"# sunset - закатная палитра, wallpaper - тёмные кольца с акцентом (accent)\n";
    o << L"theme = " << c.theme << L"\n";
    o << L"blur = " << (c.blur ? 1 : 0) << L"\n";
    o << L"sounds = " << (c.sounds ? 1 : 0) << L"\n";
    o << L"volume = " << c.volume << L"\n";
    o << L"folder_icon = " << c.folderIcon << L"\n";
    o << L"vscode = " << c.vscode << L"\n";
    o << L"terminal = " << c.terminal << L"\n";
    o << L"library = " << c.library << L"\n";
    o << L"# своя палитра (theme = custom): фон, акцент, второй акцент, кромка\n";
    o << L"custom_bg = " << ColorText(c.customBg) << L"\n";
    o << L"custom_accent = " << ColorText(c.customAccent) << L"\n";
    o << L"custom_accent2 = " << ColorText(c.customAccent2) << L"\n";
    o << L"custom_gold = " << ColorText(c.customGold) << L"\n";
    o << L"tour_done = " << (c.tourDone ? 1 : 0) << L"\n";
    o << L"profile = " << c.profile << L"\n";
    o << L"profile_ask = " << c.profileAsk << L"\n";
    o << L"# горячие углы (tl, tr, bl, br): desktop, tasks, start, screen_off, lock, screenshot, play, next, mute,\n"
         L"# keys:ctrl+shift+esc, путь к программе или пусто; corner2_ - двойной толчок в угол;\n"
         L"# wheel_ - колесо в углу: volume, brightness, track; corner_delay - сколько держать курсор в углу, мс\n";
    for (int i = 0; i < 4; ++i) o << L"corner_" << kCornerKeys[i] << L" = " << c.corners[i] << L"\n";
    for (int i = 0; i < 4; ++i) o << L"corner2_" << kCornerKeys[i] << L" = " << c.corners2[i] << L"\n";
    for (int i = 0; i < 4; ++i) o << L"wheel_" << kCornerKeys[i] << L" = " << c.cornerWheel[i] << L"\n";
    o << L"corner_delay = " << c.cornerDelay << L"\n";
    o << L"# панель «Сейчас играет»: выезжает, когда курсор у нижнего края; media_toast - карточка при смене трека\n";
    o << L"media = " << (c.media ? 1 : 0) << L"\n";
    o << L"media_toast = " << (c.mediaToast ? 1 : 0) << L"\n";
    o << L"# Alt + колесо над верхней полосой окна - прозрачность, Alt + средняя кнопка - поверх всех\n";
    o << L"window_fx = " << (c.winFx ? 1 : 0) << L"\n";
    o << L"# Alt + ` (клавиша Ё) - кольцо открытых окон с миниатюрой, средняя кнопка закрывает окно\n";
    o << L"window_ring = " << (c.winRing ? 1 : 0) << L"\n";
    o << L"# Ctrl + Alt + Пробел - кольцо выделенного текста: найти, перевести, раскладка, регистр, посчитать\n";
    o << L"selection_ring = " << (c.selectionRing ? 1 : 0) << L"\n";
    o << L"# двойной клик по пустому столу - целое кольцо вокруг курсора\n";
    o << L"desk_double = " << (c.deskDouble ? 1 : 0) << L"\n";
    o << L"# свечение у нижнего края: заряд ниже battery_low процентов, подключили зарядку\n";
    o << L"battery_glow = " << (c.batteryGlow ? 1 : 0) << L"\n";
    o << L"battery_low = " << c.batteryLow << L"\n";
    o << L"# карточка после снимка области: сохранить, распознать текст\n";
    o << L"snip_card = " << (c.snipCard ? 1 : 0) << L"\n";
    o << L"# ночной режим с night_from до night_to часов: стекло темнее, звуки тише\n";
    o << L"night = " << (c.night ? 1 : 0) << L"\n";
    o << L"night_from = " << c.nightFrom << L"\n";
    o << L"night_to = " << c.nightTo << L"\n";
    o << L"# цвет курсора #RRGGBB (пусто - системный), размер в пикселях (0 - как в системе)\n";
    o << L"cursor_color = " << c.cursorColor << L"\n";
    o << L"cursor_size = " << c.cursorSize << L"\n";
    o << L"# скорость всех анимаций: calm, normal, lively\n";
    o << L"motion = " << (c.motion == 0 ? L"calm" : c.motion == 2 ? L"lively" : L"normal") << L"\n";
    o << L"# рамка активного окна загорается цветом темы; заголовки окон в цветах темы\n";
    o << L"window_frame = " << (c.windowFrame ? 1 : 0) << L"\n";
    o << L"window_captions = " << (c.windowCaptions ? 1 : 0) << L"\n";
    o << L"# игровой режим: пока запущена игра (games - маски путей через |), эффекты выключены;\n"
         L"# game_win_anim, game_transparency - заодно выключить анимации и прозрачность Windows,\n"
         L"# game_wallpaper - пауза Wallpaper Engine\n";
    o << L"game_auto = " << (c.gameAuto ? 1 : 0) << L"\n";
    o << L"game_fullscreen = " << (c.gameFullscreen ? 1 : 0) << L"\n";
    o << L"# показ экрана: share_apps - программы трансляции (exe через |), share_hide - окна мимо захвата\n";
    o << L"share_auto = " << (c.shareAuto ? 1 : 0) << L"\n";
    o << L"share_apps = " << c.shareApps << L"\n";
    o << L"share_hide = " << (c.shareHide ? 1 : 0) << L"\n";
    o << L"# share_titles - окно-полоска показа из браузера и звонков (слова заголовка через |), share_mask - прятать\n"
         L"# личное: буфер, адреса и сети, пути с именем пользователя\n";
    o << L"share_titles = " << c.shareTitles << L"\n";
    o << L"share_mask = " << (c.shareMask ? 1 : 0) << L"\n";
    o << L"games = " << c.games << L"\n";
    o << L"games_skip = " << c.gamesSkip << L"\n";
    o << L"game_win_anim = " << (c.gameWinAnim ? 1 : 0) << L"\n";
    o << L"game_transparency = " << (c.gameTransparency ? 1 : 0) << L"\n";
    o << L"game_wallpaper = " << (c.gameWallpaper ? 1 : 0) << L"\n";
    o << L"# games_light - лёгкие игры: режим идёт, но обои, анимации и красота остаются; game_learn - игру,\n"
         L"# узнанную по окну на весь экран, предложить запомнить; game_back - игру свернули - углы и кольца "
         L"работают\n";
    o << L"games_light = " << c.gamesLight << L"\n";
    o << L"game_learn = " << (c.gameLearn ? 1 : 0) << L"\n";
    o << L"game_back = " << (c.gameBack ? 1 : 0) << L"\n";
    o << L"# режим разработки: пока открыт редактор (dev_apps - маски через |), Wallpaper Engine на паузе\n"
         L"# (dev_wallpaper) и при запуске - карточка «освободить память» (dev_boost); углы и кольца работают\n";
    o << L"dev_auto = " << (c.devAuto ? 1 : 0) << L"\n";
    o << L"dev_apps = " << c.devApps << L"\n";
    o << L"dev_wallpaper = " << (c.devWallpaper ? 1 : 0) << L"\n";
    o << L"dev_boost = " << (c.devBoost ? 1 : 0) << L"\n";
    o << L"# dev_busy - редакторы, которые считаются работой, только пока грузят процессор (рендер); dev_mem -\n"
         L"# память почти кончилась - сказать, кто её занимает\n";
    o << L"dev_busy = " << c.devBusy << L"\n";
    o << L"dev_mem = " << (c.devMem ? 1 : 0) << L"\n";
    o << L"# «Защита» в кольце утилит: программы из этой папки (и её подпапок на уровень) - пунктами кольца\n";
    o << L"security_dir = " << c.securityDir << L"\n";
    o << L"# экран блокировки: lock_source - картинка (пусто - обои), lock_reveal - после входа картинка\n"
         L"# плавно растворяется в рабочий стол\n";
    o << L"lock_source = " << c.lockSource << L"\n";
    o << L"lock_reveal = " << (c.lockReveal ? 1 : 0) << L"\n";
    o << L"# как открывается стол после входа: portal - кольцо раскрывается в стол, fade - картинка тает\n";
    o << L"lock_anim = " << (c.lockPortal ? L"portal" : L"fade") << L"\n";
    o << L"# Boost (списки и очистка - в boost.config.json): boost_dir - папка (пусто - ..\\Boost),\n"
         L"# boost_offer - предлагать освободить память при запуске игры, boost_restore - после игры вернуть\n"
         L"# закрытые программы, boost_after - после игры вернуть приоритеты и почистить память\n";
    o << L"boost_dir = " << c.boostDir << L"\n";
    o << L"boost_offer = " << (c.boostOffer ? 1 : 0) << L"\n";
    o << L"boost_restore = " << (c.boostRestore ? 1 : 0) << L"\n";
    o << L"boost_after = " << (c.boostAfter ? 1 : 0) << L"\n";
    o << L"# в игре: boost_eco - игре высокий приоритет, окнам в фоне экономия (кроме boost_keep_fast),\n"
         L"# boost_quiet - уведомления молчат, boost_islc - чистить раздутый кэш ожидания, boost_gpu - проверить,\n"
         L"# на какой видеокарте игра, boost_summary - итог после игры; dev_watch - в Unreal следить за\n"
         L"# видеопамятью, шейдерами и сборками\n";
    o << L"boost_eco = " << (c.boostEco ? 1 : 0) << L"\n";
    o << L"boost_keep_fast = " << c.boostKeepFast << L"\n";
    o << L"boost_quiet = " << (c.boostQuiet ? 1 : 0) << L"\n";
    o << L"boost_islc = " << (c.boostIslc ? 1 : 0) << L"\n";
    o << L"boost_gpu = " << (c.boostGpu ? 1 : 0) << L"\n";
    o << L"boost_summary = " << (c.boostSummary ? 1 : 0) << L"\n";
    o << L"dev_watch = " << (c.devWatch ? 1 : 0) << L"\n";
    o << L"# сеть: net_watch - следить за интернетом (при обрыве карточка «Починить?»), net_exit - сообщать о смене\n"
         L"# выходного узла, net_game - при запуске игры проверить маршрут и пинг, net_interval - период проверки, с\n";
    o << L"net_watch = " << (c.netWatch ? 1 : 0) << L"\n";
    o << L"net_security = " << (c.netSecurity ? 1 : 0) << L"\n";
    o << L"net_changes = " << (c.netChanges ? 1 : 0) << L"\n";
    o << L"net_vpn_watch = " << (c.netVpnWatch ? 1 : 0) << L"\n";
    o << L"net_vpn_block = " << (c.netVpnBlock ? 1 : 0) << L"\n";
    o << L"net_vpn_route = " << c.netVpnRoute << L"\n";
    o << L"net_vpn_apps = " << c.netVpnApps << L"\n";
    o << L"transfer_gentle = " << (c.transferGentle ? 1 : 0) << L"\n";
    o << L"net_exit = " << (c.netExit ? 1 : 0) << L"\n";
    o << L"net_game = " << (c.netGame ? 1 : 0) << L"\n";
    o << L"net_interval = " << c.netInterval << L"\n";
    o << L"# раз в сутки, пока компьютер простаивает (не в игре), тихо замерить скорость: 50-130 МБ трафика\n";
    o << L"speed_auto = " << (c.speedAuto ? 1 : 0) << L"\n";
    o << L"# утилиты: карточка, когда на диске мало места\n";
    o << L"tools_disk_warn = " << (c.toolsDiskWarn ? 1 : 0) << L"\n";
    o << L"# автоматика: тихая чистка, разбор Загрузок (с вопросом, старше downloads_days), режим питания,\n";
    o << L"# самовосстановление автозапуска и настроек\n";
    o << L"auto_clean = " << (c.autoClean ? 1 : 0) << L"\n";
    o << L"crash_restart = " << (c.crashRestart ? 1 : 0) << L"\n";
    o << L"digest = " << (c.digest ? 1 : 0) << L"\n";
    o << L"digest_hour = " << c.digestHour << L"\n";
    o << L"auto_downloads = " << (c.autoDownloads ? 1 : 0) << L"\n";
    o << L"downloads_days = " << c.downloadsDays << L"\n";
    o << L"auto_power = " << (c.autoPower ? 1 : 0) << L"\n";
    o << L"# обновление программ ночью (winget): исключения - ИД или имена через |, маски *\n";
    o << L"# телефон рядом: папки ПК для телефона (через |, пусто - Музыка, Видео, Загрузки, Изображения),\n";
    o << L"# тачпад и клавиатура с телефона, ждать запомненный телефон постоянно\n";
    o << L"phone_share = " << c.phoneShare << L"\n";
    o << L"phone_input = " << (c.phoneInput ? 1 : 0) << L"\n";
    o << L"phone_always = " << (c.phoneAlways ? 1 : 0) << L"\n";
    o << L"phone_screen = " << (c.phoneScreen ? 1 : 0) << L"\n";
    o << L"phone_files = " << (c.phoneFiles ? 1 : 0) << L"\n";
    o << L"# яркость по программе: exe=+N/-N (от нынешней) или exe=N, через |\n";
    o << L"bright_by_app = " << (c.brightByApp ? 1 : 0) << L"\n";
    o << L"bright_apps = " << c.brightApps << L"\n";
    o << L"winget_night = " << (c.wingetNight ? 1 : 0) << L"\n";
    o << L"winget_skip = " << c.wingetSkip << L"\n";
    o << L"winget_night_accept = " << (c.wingetAccept ? 1 : 0) << L"\n";
    o << L"auto_repair = " << (c.autoRepair ? 1 : 0) << L"\n";
    o << L"# раз в неделю отчёт картинкой, раз в сутки копия radial.cfg, если он поменялся\n";
    o << L"auto_report = " << (c.autoReport ? 1 : 0) << L"\n";
    o << L"auto_backup = " << (c.autoBackup ? 1 : 0) << L"\n";
    o << L"# цвета по времени суток: утром свежее, вечером теплее, ночью тёплые и приглушённые; кромка - в цвет "
         L"обоев\n";
    o << L"day_tint = " << (c.dayTint ? 1 : 0) << L"\n";
    o << L"# сотовое стекло в углах, кольце, свечении заряда и волне игрового режима\n";
    o << L"hex_glass = " << (c.hexGlass ? 1 : 0) << L"\n";
    o << L"# портал при входе после включения и после сна / выключения экрана из угла\n";
    o << L"boot_portal = " << (c.bootPortal ? 1 : 0) << L"\n";
    o << L"wake_portal = " << (c.wakePortal ? 1 : 0) << L"\n";
    o << L"# узор стекла при переходе: hex - соты, shards - осколки, none - круглый вырез\n";
    o << L"lock_pattern = " << (c.lockPattern == 0 ? L"none" : c.lockPattern == 2 ? L"shards" : L"hex") << L"\n";
    o << L"# аватар для экрана входа: картинка в середине (пусто - полумесяц), вокруг кольцо секторов\n";
    o << L"avatar_source = " << c.avatarSource << L"\n";
    o << L"# помощники: stroke - росчерк (махнули с Alt и отпустили над пунктом), palette - командная палитра\n"
         L"# (palette_key - сочетание), clip_history - история буфера, clip_secrets - секреты не запоминать и через\n"
         L"# clip_secret_sec секунд стирать из буфера, win_drag - Alt + левая тащит окно, Alt + правая - размер\n";
    o << L"stroke = " << (c.stroke ? 1 : 0) << L"\n";
    o << L"palette = " << (c.palette ? 1 : 0) << L"\n";
    o << L"palette_key = " << c.paletteKey << L"\n";
    o << L"clip_history = " << (c.clipHistory ? 1 : 0) << L"\n";
    o << L"gamepad = " << (c.gamepad ? 1 : 0) << L"\n";
    o << L"# proc_rules - постоянные правила процессов: маска=idle|below|normal|above|high, eco, noeco, cores:0-3+6;\n"
         L"# правила через | (chrome.exe=below,eco | blender.exe=high)\n";
    o << L"proc_rules = " << c.procRules << L"\n";
    o << L"# rule = условие -> действия: app:blender.exe, ac, battery, battery<30, time:23:00-07:00, weekend (через "
         L"&)\n"
         L"# -> power:eco|balanced|perf, volume:30, mute, theme:<тема>, run:<действие угла>\n";
    for (const std::wstring& r : c.rules) o << L"rule = " << r << L"\n";
    o << L"clip_secrets = " << (c.clipSecrets ? 1 : 0) << L"\n";
    o << L"clip_secret_sec = " << c.clipSecretSec << L"\n";
    o << L"# translate_google - перевод: разрешить Google там, где локально нельзя (текст уходит на его серверы)\n";
    o << L"translate_google = " << (c.translateGoogle ? 1 : 0) << L"\n";
    o << L"win_drag = " << (c.winDrag ? 1 : 0) << L"\n";
    o << L"# cpu_balance - фоновому процессу, что долго ест процессор, пока вы работаете, временно ниже приоритет;\n"
         L"# no_sleep - не уходить в сон, пока идёт что-то из no_sleep_apps; long_watch - карточка, когда команда из\n"
         L"# long_apps, работавшая дольше long_min минут, закончилась (если её окно не впереди)\n";
    o << L"cpu_balance = " << (c.cpuBalance ? 1 : 0) << L"\n";
    o << L"no_sleep = " << (c.noSleep ? 1 : 0) << L"\n";
    o << L"no_sleep_apps = " << c.noSleepApps << L"\n";
    o << L"long_watch = " << (c.longWatch ? 1 : 0) << L"\n";
    o << L"long_apps = " << c.longApps << L"\n";
    o << L"long_min = " << c.longMin << L"\n";
    o << L"# громкость по программам: уровень из микшера помнится по exe и возвращается при новом запуске\n";
    o << L"app_volume = " << (c.appVolume ? 1 : 0) << L"\n";
    o << L"# фокус: на сколько минут, что свернуть и усыпить; focus_auto - окно с этим в заголовке включает тишину "
         L"само\n";
    o << L"focus_minutes = " << c.focusMinutes << L"\n";
    o << L"focus_apps = " << c.focusApps << L"\n";
    o << L"focus_auto = " << c.focusAuto << L"\n";

    auto ring = [&](const wchar_t* tag, const std::vector<Item>& v) {
        o << L"\n" << tag << L"\n";
        for (const Item& it : v) {
            o << L"\n[" << SectionName(it.name) << L"]\n";
            if (!it.icon.empty()) o << L"icon = " << it.icon << L"\n";
            if (!it.path.empty()) o << L"path = " << it.path << L"\n";
            if (it.expand) o << L"expand = 1\n";
            if (it.files) o << L"files = 1\n";
            for (const Item& ch : it.children) {
                o << L"item = " << Field(ch.name) << L" | " << Field(ch.path);
                if (!ch.icon.empty()) o << L" | " << Field(ch.icon);
                o << L"\n";
            }
        }
    };
    ring(L"[@left]", c.left);
    ring(L"[@right]", c.right);
    if (!c.apps.empty()) {
        o << L"\n# кольца программ: первым сектором, когда впереди окно этой программы (exe - имена через |);\n"
             L"# item = Подпись | keys:ctrl+shift+p   или   macro:ctrl+l; wait 200; text адрес; enter\n";
        o << L"\n[@apps]\n";
        for (const AppRing& a : c.apps) {
            o << L"\n[" << SectionName(a.ring.name) << L"]\n";
            o << L"exe = " << a.exes << L"\n";
            if (!a.ring.icon.empty()) o << L"icon = " << a.ring.icon << L"\n";
            for (const Item& ch : a.ring.children) {
                o << L"item = " << Field(ch.name) << L" | " << Field(ch.path);
                if (!ch.icon.empty()) o << L" | " << Field(ch.icon);
                o << L"\n";
            }
        }
    }

    if (!c.sets.empty()) {
        o << L"\n# рабочие наборы: item = Подпись | code:папка, term:папка, ссылка, файл или программа;\n"
             L"# layout = l:2 / l:3 / l:main2 / l:4 - как разложить открывшиеся окна\n";
        o << L"\n[@sets]\n";
        for (const WorkSet& w : c.sets) {
            o << L"\n[" << SectionName(w.ring.name) << L"]\n";
            if (!w.layout.empty()) o << L"layout = " << w.layout << L"\n";
            if (!w.ring.icon.empty()) o << L"icon = " << w.ring.icon << L"\n";
            for (const Item& ch : w.ring.children)
                o << L"item = " << Field(ch.name) << L" | " << Field(ch.path) << L"\n";
        }
    }

    std::string bytes = Narrow(o.str());
    std::wstring tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    // всё записано и сброшено на диск до переименования: иначе после внезапного выключения (села батарея) на месте
    // radial.cfg мог остаться пустой файл
    BOOL ok =
        WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
