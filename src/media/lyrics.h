#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

// Текст песни с lrclib.net: бесплатно, без ключей. Синхронизированный (с временем у строк)
// или просто текст. Ищется в своём потоке, окну приходит сообщение, когда ответ готов.
namespace lyrics {

enum State { kNone, kLoading, kSynced, kPlain, kMissing, kInstrumental, kOffline };

struct Line {
    int64_t at = -1;  // начало строки в сотнях наносекунд, -1 - без времени
    std::wstring text;
};

struct Lyrics {
    State state = kNone;
    std::vector<Line> lines;
    // строка, которая поётся в момент pos (для синхронизированного текста), -1 - ещё не началось
    int LineAt(int64_t pos) const;
};

class Fetcher {
public:
    // начать поиск, если для этого трека ещё не искали; durSec - длина трека, 0 - неизвестна
    void Request(const std::wstring& artist, const std::wstring& title, int durSec, HWND notify, UINT msg);
    Lyrics Get(const std::wstring& artist, const std::wstring& title);

private:
    struct Job;
    static void Fetch(const Job& j);  // в фоне (bg::Run)
    SRWLOCK mu_ = SRWLOCK_INIT;
    std::map<std::wstring, Lyrics> cache_;
};

}  // namespace lyrics
