#pragma once
#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "core/text.h"

namespace view {
struct HomeTrack {
    std::filesystem::path path;
    std::wstring title, artist, album;
    double duration = 0;
    int64_t lastPlayed = 0;
    uint32_t plays = 0;
};
inline std::wstring HomeLower(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'А' && c <= L'Я')
            c += L'а' - L'А';
        else
            c = (wchar_t)std::towlower(c);
        if (c == L'Ё' || c == L'ё') c = L'е';
    }
    return s;
}
inline std::wstring HomePathKey(const std::filesystem::path& path) {
    return HomeLower(text::Utf8ToWide(text::PathToUtf8(path.lexically_normal())));
}
class HomeModel {
public:
    enum Page { Home, Library, Favorites, Queue };
    std::vector<HomeTrack> tracks;
    std::set<std::wstring> favorites;
    Page page = Home;
    std::wstring query;
    bool Match(const HomeTrack& t) const {
        const auto text = HomeLower(t.title + L" " + t.artist + L" " + t.album + L" " +
                                    text::Utf8ToWide(text::PathToUtf8(t.path.filename())));
        const auto q = HomeLower(query);
        size_t a = 0;
        while (a < q.size()) {
            auto b = q.find(L' ', a);
            auto word = q.substr(a, b == std::wstring::npos ? b : b - a);
            if (!word.empty() && text.find(word) == std::wstring::npos) return false;
            if (b == std::wstring::npos) break;
            a = b + 1;
        }
        return true;
    }
    std::vector<size_t> Visible() const {
        std::vector<size_t> out;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (Match(tracks[i]) && (page != Favorites || favorites.count(HomePathKey(tracks[i].path))))
                out.push_back(i);
        if (page == Home && query.empty())
            std::stable_sort(out.begin(), out.end(),
                             [&](size_t a, size_t b) { return tracks[a].lastPlayed > tracks[b].lastPlayed; });
        return out;
    }
    void ToggleFavorite(const std::filesystem::path& path) {
        auto key = HomePathKey(path);
        if (!favorites.erase(key)) favorites.insert(std::move(key));
    }
};
}  // namespace view
