#pragma once
// Переносимая часть плагина загрузки: какие ссылки понимаем, какие аргументы даём yt-dlp и
// как читаем его вывод. Без Win32 - проверяется тестами на Linux (tests/test_downloader.cpp).
//
// yt-dlp запускается с шаблонами вывода, которые печатают строки с метками:
//   PMITEM|<номер в плейлисте>|<всего>|<название>|<исполнитель>   перед загрузкой трека
//   PMPROG|<проценты>|<скорость>|<осталось>                        ход загрузки
//   PMFILE|<путь>                                                   готовый файл (после конвертации)
// Остальное - обычные строки yt-dlp: ERROR:, WARNING:, [ExtractAudio] и т.п.

#include <string>
#include <vector>

namespace dl {

enum class Source { YouTubeMusic, YouTube, Yandex, Other, Invalid };

Source DetectSource(const std::string& url);
bool IsYandexMusicExtractor(const std::string& line);
std::string CanonicalDownloadKey(const std::string& url);
bool IsPlaylist(const std::string& url);
bool NeedsAccount(const std::string& url);
const char* SourceName(Source s);  // «YouTube Music», «Яндекс Музыка»...

// Все ссылки http(s) из текста буфера обмена, без повторов, в порядке появления
std::vector<std::string> ExtractUrls(const std::string& text);

struct Options {
    std::string archiveFile;  // persistent successful source IDs, shared across playlists
    std::string outDir;       // куда сохранять (UTF-8)
    std::string browser;      // откуда взять вход: "", "firefox", "chrome", "edge"
    std::string ffmpegDir;    // папка с ffmpeg, если не в PATH
    bool mp3 = true;          // перекодировать в MP3 (нужен ffmpeg); иначе как есть
};

// Аргументы для yt-dlp (без имени программы)
std::vector<std::string> BuildArgs(const std::string& url, const Options& o);
// Командная строка Windows из аргументов (правила CommandLineToArgvW)
std::string QuoteArgs(const std::vector<std::string>& args);

struct Event {
    enum class Type { None, Item, Progress, File, Converting, Error, Warning, Skipped };
    Type type = Type::None;
    double percent = -1;        // 0..100
    std::string speed, eta;     // как печатает yt-dlp: «2.10MiB/s», «00:05»
    int item = 0, items = 0;    // номер трека в плейлисте и сколько всего (0 - не плейлист)
    std::string title, artist;  // для Item
    std::string text;           // путь для File, сообщение для Error/Warning
};

Event ParseLine(std::string line);

// Скорость по-русски: «2.10MiB/s» -> «2.1 МБ/с»
std::string HumanSpeed(const std::string& s);

// Установка инструментов одной кнопкой: curl.exe и tar.exe есть в Windows 10 1803 и новее.
// Официальные сборки: yt-dlp из его релизов, ffmpeg - сборка проекта yt-dlp/FFmpeg-Builds.
constexpr const char* kYtdlpUrl = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
constexpr const char* kFfmpegUrl =
    "https://github.com/yt-dlp/FFmpeg-Builds/releases/latest/download/ffmpeg-master-latest-win64-gpl.zip";
// Аргументы curl: скачать url в файл out с полосой прогресса «###  12.3%»
std::vector<std::string> CurlArgs(const std::string& url, const std::string& out);
// Аргументы tar: достать ffmpeg.exe и ffprobe.exe из архива сразу в папку dir
std::vector<std::string> FfmpegUnpackArgs(const std::string& zip, const std::string& dir);
// Проценты из строки прогресса curl (-#); -1, если строка не про прогресс
double CurlPercent(const std::string& line);

}  // namespace dl
