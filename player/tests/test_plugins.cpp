// Плагины: поиск по plugin.json, включение и выключение, события, пункты меню, ошибки.
// Используется настоящий пример plugins/history, собранный рядом с тестами.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <thread>

#include "core/plugins.h"
#include "core/text.h"
#include "tests/check.h"
#include "tests/wav_util.h"

namespace {

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(PluginsDiscoverEnableNotify) {
    const auto data = tests::TempDir("plugins_data");
    const auto extra = tests::TempDir("plugins_extra");
    // Битый манифест и плагин без библиотеки показываются с причиной, но не загружаются
    std::filesystem::create_directories(extra / "broken");
    tests::WriteBytes(extra / "broken" / "plugin.json", {'{', 'x'});
    std::filesystem::create_directories(extra / "nolib");
    const std::string manifest = R"({"id":"nolib","name":"Без DLL","library":"nolib"})";
    tests::WriteBytes(extra / "nolib" / "plugin.json", {manifest.begin(), manifest.end()});

    pm::PluginManager pm;
    pm.SetDataRoot(data);
    int commands = 0;
    pm::PluginHostFunctions host;
    host.playback = [] { return PmPlayback{1, 0, 1, 0, 0, 1}; };
    host.command = [&](int) { ++commands; };
    pm.SetHost(host);
    pm.Discover({PM_PLUGINS_DIR, extra});
    // В папке сборки могут лежать и другие плагины (на Windows - downloader): ищем по id
    CHECK(pm.Plugins().size() >= 3);
    size_t history = 99, nolib = 99, broken = 99;
    for (size_t i = 0; i < pm.Plugins().size(); ++i) {
        const auto& p = pm.Plugins()[i];
        if (p.id == "history") history = i;
        if (p.id == "nolib") nolib = i;
        if (p.id == "broken") broken = i;
    }
    const size_t n = pm.Plugins().size();
    CHECK(history < n && nolib < n && broken < n);
    if (history >= n || nolib >= n || broken >= n) return;
    CHECK(pm.Plugins()[history].name == "Журнал прослушиваний");
    // По умолчанию всё выключено
    CHECK(pm.EnabledIds().empty() && !pm.Plugins()[history].loaded);
    CHECK(!pm.Plugins()[broken].error.empty());
    CHECK(!pm.SetEnabled(nolib, true) && !pm.Plugins()[nolib].error.empty() && !pm.Plugins()[nolib].enabled);

    CHECK(pm.SetEnabled(history, true));
    CHECK(pm.Plugins()[history].loaded && pm.EnabledIds() == std::set<std::string>{"history"});
    // Плагин добавил свой пункт меню
    CHECK(pm.MenuItems().size() == 1 && pm.MenuItems()[0].title == "Открыть журнал прослушиваний");
    pm.InvokeMenuItem(0);  // на Linux ничего не открывает, но не должен падать

    PmTrack t{"/music/a.mp3", "Лунный свет", "Северный Ветер", "", 200};
    pm.NotifyTrack(&t);
    pm.NotifyTrack(nullptr);  // пустой плейлист - запись не появляется
    PmTrack u{"/music/b.mp3", "Без исполнителя", "", "", 0};
    pm.NotifyTrack(&u);
    pm.NotifyPlayState(true);
    const std::string log = ReadAll(data / "history" / "history.txt");
    CHECK(log.find("| Северный Ветер - Лунный свет\n") != std::string::npos);
    CHECK(log.find("| Без исполнителя\n") != std::string::npos);
    CHECK(std::count(log.begin(), log.end(), '\n') == 2);

    // Выключение выгружает плагин и убирает его пункты меню; события больше не доходят
    CHECK(pm.SetEnabled(history, false));
    CHECK(!pm.Plugins()[history].loaded && pm.MenuItems().empty());
    pm.NotifyTrack(&t);
    CHECK(ReadAll(data / "history" / "history.txt") == log);

    // Повторный поиск сохраняет выбор пользователя
    pm.SetEnabled(history, true);
    pm.Discover({PM_PLUGINS_DIR, extra});
    CHECK(pm.EnabledIds() == std::set<std::string>{"history"});
    CHECK(ReadAll(data / "plugins.log").find("history: загружен") != std::string::npos);
    pm.UnloadAll();
    CHECK(pm.MenuItems().empty());
    (void)commands;
}

TEST(PluginsPagesAndPost) {
    const auto data = tests::TempDir("plugins_pages");
    pm::PluginManager pm;
    pm.SetDataRoot(data);
    std::atomic<int> wakes{0};
    std::vector<std::string> toasts;
    size_t shown = 99;
    pm::PluginHostFunctions host;
    host.wake = [&] { ++wakes; };
    host.toast = [&](const std::string& t) { toasts.push_back(t); };
    host.clipboardText = [] { return std::string("https://music.youtube.com/watch?v=abc"); };
    host.pageShow = [&](size_t i) { shown = i; };
    pm.SetHost(host);
    pm.Discover({PM_TEST_PLUGINS_DIR});
    CHECK(pm.Plugins().size() == 1);
    if (pm.Plugins().empty()) return;
    CHECK(pm.SetEnabled(0, true));
    // Страница создана при загрузке: шапка из двух кнопок, строка с текстом из буфера
    CHECK(pm.Pages().size() == 1);
    if (pm.Pages().empty()) return;
    const pm::PluginPage& page = pm.Pages()[0];
    CHECK(page.title == "Тест" && page.emptyText == "Пусто" && page.actions.size() == 2);
    CHECK(page.rows.size() == 1 && page.rows[0].title == "https://music.youtube.com/watch?v=abc");
    CHECK(page.rows[0].state == PM_ROW_DONE && page.rows[0].progress < 0);

    // Кнопка шапки: плагин работает в своём потоке и присылает строку через post
    const uint64_t rev = pm.PagesRevision();
    pm.PageAction(0, 0, 1);
    for (int i = 0; i < 500 && wakes.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(wakes.load() == 1);
    CHECK(pm.Pages()[0].rows.size() == 1);  // до RunPosted ничего не меняется
    pm.RunPosted();
    CHECK(pm.PagesRevision() > rev);
    CHECK(pm.Pages()[0].rows.size() == 2 && pm.Pages()[0].rows[0].id == 42);  // новые сверху
    CHECK(std::abs(pm.Pages()[0].rows[0].progress - 0.5f) < 1e-6f && pm.Pages()[0].rows[0].actions.size() == 1);
    // Кнопка строки: плагин убирает строку и показывает уведомление
    pm.PageAction(0, 42, 7);
    CHECK(pm.Pages()[0].rows.size() == 1 && toasts == std::vector<std::string>{"убрано"});

    // Выключение: страница исчезает, отложенные вызовы отменяются
    pm.PageAction(0, 0, 1);
    for (int i = 0; i < 500 && wakes.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(pm.SetEnabled(0, false));
    CHECK(pm.Pages().empty());
    pm.RunPosted();  // код плагина уже выгружен - вызов не должен случиться
    (void)shown;
}

#ifdef _WIN32
TEST(DownloaderQueueArchiveAndPartialPlaylist) {
    const auto root = tests::TempDir("downloader_archive");
    const auto plugin = root / "extensions/downloader";
    const auto data = root / "data";
    const auto output = root / "music";
    std::filesystem::create_directories(plugin / "bin");
    std::filesystem::create_directories(data / "downloader");
    std::filesystem::create_directories(output);
    for (const auto* name : {"downloader.dll", "plugin.json"})
        std::filesystem::copy_file(std::filesystem::path(PM_PLUGINS_DIR) / "downloader" / name, plugin / name);
    // Always use this fixture's executable, never a real downloader or browser account.
    std::filesystem::copy_file(PM_FAKE_YTDLP_PATH, plugin / "bin/yt-dlp.exe");
    const std::string config = "outDir=" + text::PathToUtf8(output) + "\nbrowser=\n";
    tests::WriteBytes(data / "downloader/settings.txt", {config.begin(), config.end()});
    std::string url;
    int added = 0;
    std::vector<std::string> toasts;
    pm::PluginHostFunctions host;
    host.clipboardText = [&] { return url; };
    host.addFiles = [&](const std::vector<std::string>& files, bool) {
        for (const auto& path : files) CHECK(std::filesystem::exists(text::Utf8ToPath(path)));
        added += (int)files.size();
    };
    host.toast = [&](const std::string& message) { toasts.push_back(message); };
    pm::PluginManager manager;
    manager.SetDataRoot(data); manager.SetHost(host); manager.Discover({root / "extensions"});
    CHECK(manager.Plugins().size() == 1 && manager.SetEnabled(0, true));
    if (manager.Pages().empty()) return;
    auto jobs = [&] {
        return std::count_if(manager.Pages()[0].rows.begin(), manager.Pages()[0].rows.end(),
            [](const auto& row) { return row.id >= 100; });
    };
    auto wait = [&](int id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            manager.RunPosted();
            for (const auto& row : manager.Pages()[0].rows)
                if (row.id == id && (row.state == PM_ROW_DONE || row.state == PM_ROW_ERROR)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    url = "https://music.youtube.com/playlist?list=LM";
    manager.PageAction(0, 0, 1);
    CHECK(jobs() == 0 && !toasts.empty() && toasts.back().find("Выберите браузер") != std::string::npos);
    manager.PageAction(0, 0, 23); // Explicit Chrome choice saves a preference; it does not read cookies.
    CHECK(ReadAll(data / "downloader/settings.txt").find("browser=chrome") != std::string::npos);
    manager.PageAction(0, 0, 21);
    url = "https://youtube.com/playlist?list=fixture-fast";
    manager.PageAction(0, 0, 1);
    url = "https://music.youtube.com/playlist?list=fixture-fast&si=tracking";
    manager.PageAction(0, 0, 1);
    CHECK(jobs() == 1); // Same list across two domains is queued once.
    CHECK(wait(100) && added == 2);
    CHECK(std::filesystem::exists(output / ".playermoon-archive.txt"));
    manager.PageAction(0, 0, 1); // A finished playlist can be synced again.
    CHECK(wait(101) && jobs() == 2 && added == 2);
    bool skipped = false;
    for (const auto& row : manager.Pages()[0].rows)
        if (row.id == 101) skipped = row.state == PM_ROW_DONE && row.subtitle.find("Повторы пропущены: 2") != std::string::npos;
    CHECK(skipped);
    CHECK(manager.SetEnabled(0, false) && manager.SetEnabled(0, true));
    manager.PageAction(0, 0, 1); // Reopening the plugin retains the archive.
    CHECK(wait(100) && added == 2);
    url = "https://youtube.com/playlist?list=partial-fixture-fast";
    manager.PageAction(0, 0, 1);
    CHECK(wait(101) && added == 3);
    bool partial = false, retry = false;
    for (const auto& row : manager.Pages()[0].rows) if (row.id == 101) {
        partial = row.state == PM_ROW_DONE && row.subtitle.find("Часть треков недоступна") != std::string::npos;
        for (const auto& action : row.actions) if (action.id == 5) retry = true;
    }
    CHECK(partial && retry);
    manager.UnloadAll();
}
#endif
