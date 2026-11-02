#include "tests/check.h"
#include "view/home_model.h"
#include "view/home_layout.h"

TEST(HomeLayoutKeepsTransportClearOfTrackAndVolume) {
    for (float width : {900.f, 1024.f, 1179.f, 1180.f, 1440.f, 2400.f}) {
        view::HomeLayout layout({width, 640});
        CHECK(layout.trackText.Right() + 20 <= layout.seek.x - 41);
        CHECK(layout.trackText.Right() < layout.transport.x);
        CHECK(layout.seek.Right() + 47 < layout.volume.x - 32);
        CHECK(layout.transport.Right() < layout.volume.x - 32);
        CHECK(layout.searchText.x >= layout.search.x);
        CHECK(layout.searchText.Right() <= layout.search.Right());
        CHECK(layout.searchText.Bottom() <= layout.search.Bottom());
        CHECK(layout.seek.w > 100);
    }
}

TEST(HomeSearchCombinesTagsAndIgnoresCyrillicCase) {
    view::HomeModel m;
    view::HomeTrack track;
    track.path = text::Utf8ToPath("music/Новая песня.mp3");
    track.title = L"Полёт";
    track.artist = L"Луна";
    track.album = L"Ночь";
    m.tracks.push_back(track);
    m.query = L"  ЛУНА  полет ";
    CHECK(m.Visible().size() == 1);
    m.query = L"НОЧЬ песня";
    CHECK(m.Visible().size() == 1);
    m.query = L"Луна солнце";
    CHECK(m.Visible().empty());
}
TEST(HomeFavoritesAndRecentOrderAreIndependentOfQueue) {
    view::HomeModel m;
    view::HomeTrack a, b, c;
    a.path = L"a.mp3";
    b.path = L"b.mp3";
    c.path = L"c.mp3";
    b.lastPlayed = 200;
    c.lastPlayed = 100;
    m.tracks = {a, b, c};
    auto list = m.Visible();
    CHECK(list == std::vector<size_t>({1, 2, 0}));
    m.page = view::HomeModel::Library;
    CHECK(m.Visible() == std::vector<size_t>({0, 1, 2}));
    m.ToggleFavorite(b.path);
    m.page = view::HomeModel::Favorites;
    CHECK(m.Visible() == std::vector<size_t>({1}));
    m.ToggleFavorite(b.path);
    CHECK(m.Visible().empty());
    m.ToggleFavorite(L"B.mp3");
    CHECK(m.Visible() == std::vector<size_t>({1}));
    CHECK(m.tracks.size() == 3);
}
