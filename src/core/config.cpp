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
            if (key == L"desktop_only")
                cfg.desktopOnly = ParseBool(val);
            else if (key == L"skip_fullscreen")
                cfg.skipFullscreen = ParseBool(val);
            else if (key == L"inner_edges")
                cfg.innerEdges = ParseBool(val);
            else if (key == L"motion")
                cfg.motion = Lower(val) == L"calm" ? 0 : Lower(val) == L"lively" ? 2 : 1;
            else if (key == L"window_frame")
                cfg.windowFrame = ParseBool(val);
            else if (key == L"window_captions")
                cfg.windowCaptions = ParseBool(val);
            else if (key == L"game_auto")
                cfg.gameAuto = ParseBool(val);
            else if (key == L"game_fullscreen")
                cfg.gameFullscreen = ParseBool(val);
            else if (key == L"share_auto")
                cfg.shareAuto = ParseBool(val);
            else if (key == L"share_apps")
                cfg.shareApps = val;
            else if (key == L"share_hide")
                cfg.shareHide = ParseBool(val);
            else if (key == L"games")
                cfg.games = val;
            else if (key == L"games_skip")
                cfg.gamesSkip = val;
            else if (key == L"game_win_anim")
                cfg.gameWinAnim = ParseBool(val);
            else if (key == L"game_transparency")
                cfg.gameTransparency = ParseBool(val);
            else if (key == L"game_wallpaper")
                cfg.gameWallpaper = ParseBool(val);
            else if (key == L"dev_auto")
                cfg.devAuto = ParseBool(val);
            else if (key == L"dev_apps")
                cfg.devApps = val;
            else if (key == L"security_dir")
                cfg.securityDir = val;
            else if (key == L"dev_wallpaper")
                cfg.devWallpaper = ParseBool(val);
            else if (key == L"dev_boost")
                cfg.devBoost = ParseBool(val);
            else if (key == L"lock_source")
                cfg.lockSource = val;
            else if (key == L"lock_reveal")
                cfg.lockReveal = ParseBool(val);
            else if (key == L"boost_dir")
                cfg.boostDir = val;
            else if (key == L"boost_offer")
                cfg.boostOffer = ParseBool(val);
            else if (key == L"boost_restore")
                cfg.boostRestore = ParseBool(val);
            else if (key == L"boost_after")
                cfg.boostAfter = ParseBool(val);
            else if (key == L"boost_eco")
                cfg.boostEco = ParseBool(val);
            else if (key == L"boost_keep_fast")
                cfg.boostKeepFast = val;
            else if (key == L"boost_quiet")
                cfg.boostQuiet = ParseBool(val);
            else if (key == L"boost_islc")
                cfg.boostIslc = ParseBool(val);
            else if (key == L"boost_gpu")
                cfg.boostGpu = ParseBool(val);
            else if (key == L"boost_summary")
                cfg.boostSummary = ParseBool(val);
            else if (key == L"dev_watch")
                cfg.devWatch = ParseBool(val);
            else if (key == L"speed_auto")
                cfg.speedAuto = ParseBool(val);
            else if (key == L"net_watch")
                cfg.netWatch = ParseBool(val);
            else if (key == L"net_exit")
                cfg.netExit = ParseBool(val);
            else if (key == L"net_game")
                cfg.netGame = ParseBool(val);
            else if (key == L"auto_clean")
                cfg.autoClean = ParseBool(val);
            else if (key == L"crash_restart")
                cfg.crashRestart = ParseBool(val);
            else if (key == L"digest")
                cfg.digest = ParseBool(val);
            else if (key == L"digest_hour")
                cfg.digestHour = std::clamp(_wtoi(val.c_str()), 0, 23);
            else if (key == L"auto_downloads")
                cfg.autoDownloads = ParseBool(val);
            else if (key == L"downloads_days")
                cfg.downloadsDays = std::clamp(_wtoi(val.c_str()), 3, 180);
            else if (key == L"auto_power")
                cfg.autoPower = ParseBool(val);
            else if (key == L"auto_repair")
                cfg.autoRepair = ParseBool(val);
            else if (key == L"auto_report")
                cfg.autoReport = ParseBool(val);
            else if (key == L"auto_backup")
                cfg.autoBackup = ParseBool(val);
            else if (key == L"tools_disk_warn")
                cfg.toolsDiskWarn = ParseBool(val);
            else if (key == L"net_interval")
                cfg.netInterval = std::clamp(_wtoi(val.c_str()), 5, 600);
            else if (key == L"day_tint")
                cfg.dayTint = ParseBool(val);
            else if (key == L"hex_glass")
                cfg.hexGlass = ParseBool(val);
            else if (key == L"boot_portal")
                cfg.bootPortal = ParseBool(val);
            else if (key == L"wake_portal")
                cfg.wakePortal = ParseBool(val);
            else if (key == L"lock_pattern")
                cfg.lockPattern = Lower(val) == L"none" ? 0 : Lower(val) == L"shards" ? 2 : 1;
            else if (key == L"lock_anim")
                cfg.lockPortal = Lower(val) != L"fade";
            else if (key == L"avatar_source")
                cfg.avatarSource = val;
            else if (key == L"stroke")
                cfg.stroke = ParseBool(val);
            else if (key == L"palette")
                cfg.palette = ParseBool(val);
            else if (key == L"palette_key")
                cfg.paletteKey = Lower(val);
            else if (key == L"rule")
                cfg.rules.push_back(val);
            else if (key == L"proc_rules")
                cfg.procRules = val;
            else if (key == L"gamepad")
                cfg.gamepad = ParseBool(val);
            else if (key == L"clip_history")
                cfg.clipHistory = ParseBool(val);
            else if (key == L"clip_secrets")
                cfg.clipSecrets = ParseBool(val);
            else if (key == L"clip_secret_sec")
                cfg.clipSecretSec = std::clamp(_wtoi(val.c_str()), 10, 600);
            else if (key == L"win_drag")
                cfg.winDrag = ParseBool(val);
            else if (key == L"cpu_balance")
                cfg.cpuBalance = ParseBool(val);
            else if (key == L"no_sleep")
                cfg.noSleep = ParseBool(val);
            else if (key == L"no_sleep_apps")
                cfg.noSleepApps = val;
            else if (key == L"long_watch")
                cfg.longWatch = ParseBool(val);
            else if (key == L"long_apps")
                cfg.longApps = val;
            else if (key == L"long_min")
                cfg.longMin = std::clamp(_wtoi(val.c_str()), 1, 120);
            else if (key == L"app_volume")
                cfg.appVolume = ParseBool(val);
            else if (key == L"focus_minutes")
                cfg.focusMinutes = std::clamp(_wtoi(val.c_str()), 5, 240);
            else if (key == L"focus_apps")
                cfg.focusApps = val;
            else if (key == L"focus_auto")
                cfg.focusAuto = val;
            else if (key == L"scale")
                cfg.scale = std::clamp(_wtof(val.c_str()), 0.5, 3.0);
            else if (key == L"accent") {
                cfg.accentAuto = Lower(val) == L"auto";
                if (!cfg.accentAuto) ParseColor(val, cfg.accentManual);
            } else if (key == L"accent_color")
                ParseColor(val, cfg.accentManual);
            else if (key == L"max_children")
                cfg.maxChildren = std::clamp(_wtoi(val.c_str()), 4, 24);
            else if (key == L"theme")
                cfg.theme = Lower(val);
            else if (key == L"blur")
                cfg.blur = ParseBool(val);
            else if (key == L"sounds")
                cfg.sounds = ParseBool(val);
            else if (key == L"volume")
                cfg.volume = std::clamp(_wtoi(val.c_str()), 0, 100);
            else if (key == L"folder_icon")
                cfg.folderIcon = val;
            else if (key == L"vscode")
                cfg.vscode = val;
            else if (key == L"terminal")
                cfg.terminal = val;
            else if (key == L"library")
                cfg.library = val;
            else if (key == L"custom_bg")
                ParseColor(val, cfg.customBg);
            else if (key == L"custom_accent")
                ParseColor(val, cfg.customAccent);
            else if (key == L"custom_accent2")
                ParseColor(val, cfg.customAccent2);
            else if (key == L"custom_gold")
                ParseColor(val, cfg.customGold);
            else if (key == L"tour_done")
                cfg.tourDone = ParseBool(val);
            else if (key.size() > 3 && (key.rfind(L"corner_", 0) == 0 || key.rfind(L"corner2_", 0) == 0 ||
                                        key.rfind(L"wheel_", 0) == 0)) {
                std::wstring side = key.substr(key.size() - 2), kind = key.substr(0, key.size() - 2);
                for (int i = 0; i < 4; ++i) {
                    if (side != kCornerKeys[i]) continue;
                    if (kind == L"corner_") cfg.corners[i] = val;
                    if (kind == L"corner2_") cfg.corners2[i] = val;
                    if (kind == L"wheel_") cfg.cornerWheel[i] = val;
                }
                if (key == L"corner_delay") cfg.cornerDelay = std::clamp(_wtoi(val.c_str()), 50, 2000);
            } else if (key == L"media")
                cfg.media = ParseBool(val);
            else if (key == L"media_toast")
                cfg.mediaToast = ParseBool(val);
            else if (key == L"window_fx")
                cfg.winFx = ParseBool(val);
            else if (key == L"window_ring")
                cfg.winRing = ParseBool(val);
            else if (key == L"desk_double")
                cfg.deskDouble = ParseBool(val);
            else if (key == L"battery_glow")
                cfg.batteryGlow = ParseBool(val);
            else if (key == L"battery_low")
                cfg.batteryLow = std::clamp(_wtoi(val.c_str()), 5, 50);
            else if (key == L"snip_card")
                cfg.snipCard = ParseBool(val);
            else if (key == L"night")
                cfg.night = ParseBool(val);
            else if (key == L"night_from")
                cfg.nightFrom = std::clamp(_wtoi(val.c_str()), 0, 23);
            else if (key == L"night_to")
                cfg.nightTo = std::clamp(_wtoi(val.c_str()), 0, 23);
            else if (key == L"cursor_color") {
                COLORREF tmp;
                cfg.cursorColor = ParseColor(val, tmp) ? val : L"";
            } else if (key == L"cursor_size")
                cfg.cursorSize = std::clamp(_wtoi(val.c_str()), 0, 128);
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
    o << L"games = " << c.games << L"\n";
    o << L"games_skip = " << c.gamesSkip << L"\n";
    o << L"game_win_anim = " << (c.gameWinAnim ? 1 : 0) << L"\n";
    o << L"game_transparency = " << (c.gameTransparency ? 1 : 0) << L"\n";
    o << L"game_wallpaper = " << (c.gameWallpaper ? 1 : 0) << L"\n";
    o << L"# режим разработки: пока открыт редактор (dev_apps - маски через |), Wallpaper Engine на паузе\n"
         L"# (dev_wallpaper) и при запуске - карточка «освободить память» (dev_boost); углы и кольца работают\n";
    o << L"dev_auto = " << (c.devAuto ? 1 : 0) << L"\n";
    o << L"dev_apps = " << c.devApps << L"\n";
    o << L"dev_wallpaper = " << (c.devWallpaper ? 1 : 0) << L"\n";
    o << L"dev_boost = " << (c.devBoost ? 1 : 0) << L"\n";
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
