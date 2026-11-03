// Manual Windows integration check. Run only with a disposable silent WAV in PlayerMoon.
#include "media/media.h"
#include <cstdio>
#include <string>
#include <cstring>
#include <windows.h>

template<class Predicate>
bool Until(media::Watcher& watcher, Predicate predicate, DWORD ms = 10000) {
    const auto end = GetTickCount64() + ms;
    do {
        if (predicate(watcher.Get())) return true;
        Sleep(100);
    } while (GetTickCount64() < end);
    return false;
}

int main(int argc, char** argv) {
    media::Watcher watcher;
    watcher.Start(nullptr, 0);
    watcher.SetFast(true);
    watcher.SetSource(L"MoonDesk.PlayerMoon");
    if (!Until(watcher, [](const media::Track& t) {
            return t.has && t.aumid == L"MoonDesk.PlayerMoon" && !t.title.empty() && t.end > 0;
        })) {
        std::puts("PlayerMoon media session/metadata not found");
        auto t = watcher.Get();
        for (const auto& s : t.sources) std::wprintf(L"source: %ls status=%d\n", s.aumid.c_str(), s.status);
        return 1;
    }
    auto track = watcher.Get();
    std::wprintf(L"PlayerMoon: %ls duration=%lld status=%d\n", track.title.c_str(), (long long)track.end, track.status);
    if (argc != 2 || std::strcmp(argv[1], "--exercise") != 0) return 0;
    if (track.status != media::kPlaying) {
        watcher.Send(media::CMD_TOGGLE);
        if (!Until(watcher, [](const media::Track& t) { return t.status == media::kPlaying; })) return 2;
    }
    watcher.Send(media::CMD_TOGGLE);
    if (!Until(watcher, [](const media::Track& t) { return t.status == media::kPaused; })) return 3;
    watcher.Send(media::CMD_SEEK, 3 * 10000000LL);
    if (!Until(watcher, [](const media::Track& t) { return t.Now() >= 2 * 10000000LL; })) return 4;
    auto firstTitle = watcher.Get().title;
    watcher.Send(media::CMD_NEXT);
    if (!Until(watcher, [&](const media::Track& t) { return t.title != firstTitle; })) return 5;
    watcher.Send(media::CMD_PREV);
    if (!Until(watcher, [&](const media::Track& t) { return t.title == firstTitle; })) return 6;
    if (watcher.Get().status == media::kPlaying) watcher.Send(media::CMD_TOGGLE);
    std::puts("Metadata, play, pause, seek, next and previous through RadialMenu's actual media watcher passed");
}
