// Второе кольцо: что в нём показать, вход в папки, страницы, поиск.
#include "menu/menu.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

#include "core/files.h"
#include "core/launch.h"
#include "core/perf.h"
#include "core/text.h"
#include "core/themes.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/sound.h"
#include "menu/menu_internal.h"

using namespace Gdiplus;
using namespace menu_detail;

namespace {

// Запрос, набранный не в той раскладке: "ghjtrns" -> "проекты" и обратно.
std::wstring OtherLayout(const std::wstring& q) {
    static const wchar_t en[] = L"qwertyuiop[]asdfghjkl;'zxcvbnm,.`";
    static const wchar_t ru[] =
        L"йцукенгшщзхъфывап"
        L"ролджэячсмитьбюё";
    std::wstring out = q;
    for (wchar_t& c : out) {
        if (const wchar_t* p = wcschr(en, c); p && c)
            c = ru[p - en];
        else if (const wchar_t* r = wcschr(ru, c); r && c)
            c = en[r - ru];
    }
    return out;
}

// 0 - совпало начало имени, 1 - начало слова, 2 - где-то внутри, -1 - нет
int MatchScore(const std::wstring& name, const std::wstring& q) {
    if (q.empty()) return -1;
    size_t at = name.find(q);
    if (at == std::wstring::npos) return -1;
    if (at == 0) return 0;
    for (; at != std::wstring::npos; at = name.find(q, at + 1)) {
        wchar_t p = name[at - 1];
        if (p == L' ' || p == L'-' || p == L'_' || p == L'.' || p == L'(') return 1;
    }
    return 2;
}

}  // namespace

double RadialMenu::Span() const { return full_ ? 180.0 : kHalfSpan; }

double RadialMenu::SubStep() const {
    int m = std::max<int>(1, (int)subItems_.size());
    return m * kSubItemDeg > 2 * Span() ? 2 * Span() / m : kSubItemDeg;
}

double RadialMenu::SubFrom() const {
    double total = SubStep() * subItems_.size();
    return std::clamp(subMid_ - total / 2, -Span(), Span() - total);
}

const std::wstring& RadialMenu::IconPng(const Item& it) const {
    static const std::wstring kNone;
    if (!it.iconFull.empty()) return it.iconFull;
    if (it.dir && !cfg_->folderIconFull.empty()) return cfg_->folderIconFull;
    return kNone;
}

int RadialMenu::HotSub() const {
    if (hoverSub_ >= 0) return hoverSub_;
    if (mode_ == Mode::Search && kbSel_ < (int)subItems_.size()) return kbSel_;
    return -1;
}

const Item* RadialMenu::HoveredItem() const {
    int s = HotSub();
    if (s >= 0 && s < (int)subItems_.size()) return subItems_[s];
    if (hoverRoot_ >= 0) return &(*ring_)[hoverRoot_];
    return nullptr;
}

void RadialMenu::SwitchRoot(int root) {
    navStack_.clear();
    page_ = 0;
    pendingRoot_ = -1;
    const Item& it = (*ring_)[root];
    if (it.HasRing()) {
        mode_ = Mode::Root;
        subParent_ = root;
        RebuildSub();
        tSub_ = GetTickCount64();
        subSlide_ = 0;
    } else {
        mode_ = Mode::None;
        subParent_ = -1;
        subItems_.clear();
        subOwned_.clear();
        hlSub_.clear();
    }
}

void RadialMenu::RebuildSub() {
    PERF_SCOPE("menu.RebuildSub");
    std::vector<Item> list;
    int cap = cfg_->maxChildren;
    if (mode_ == Mode::Search) {
        subMid_ = 0;
        std::wstring q = Lower(query_), q2 = OtherLayout(q);
        std::vector<std::pair<int, const Item*>> found;
        for (const Item& it : searchPool_) {
            std::wstring n = Lower(it.name);
            int s = MatchScore(n, q);
            int s2 = MatchScore(n, q2);
            if (s < 0 || (s2 >= 0 && s2 < s)) s = s2;
            if (s >= 0) found.push_back({s * 1000 + (int)std::min<size_t>(n.size(), 999), &it});
        }
        std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < found.size() && (int)i < cap; ++i) list.push_back(*found[i].second);
        pageCount_ = 1;
        page_ = 0;
        kbSel_ = std::clamp(kbSel_, 0, std::max(0, (int)list.size() - 1));
    } else if (mode_ == Mode::Root && subParent_ >= 0) {
        double step = 2 * Span() / ring_->size();
        subMid_ = -Span() + step * (subParent_ + 0.5);
        if (!navStack_.empty()) {
            list = ListFolder(navStack_.back().full, true);
        } else {
            const Item& root = (*ring_)[subParent_];
            list = root.children;
            if (root.expand)
                for (Item& c : ListFolder(root.full, root.files)) list.push_back(std::move(c));
        }
        // страницы: на каждой cap-1 пунктов и сектор перехода дальше
        if ((int)list.size() > cap) {
            int per = cap - 1;
            pageCount_ = ((int)list.size() + per - 1) / per;
            page_ = std::clamp(page_, 0, pageCount_ - 1);
            std::vector<Item> part(list.begin() + page_ * per,
                                   list.begin() + std::min<size_t>(list.size(), (size_t)(page_ + 1) * per));
            Item more;
            more.special = 1;
            more.name = page_ + 1 < pageCount_ ? L"Ещё…" : L"В начало";
            more.iconFull = Exists(ExeDir() + L"\\icons\\more.png") ? ExeDir() + L"\\icons\\more.png"
                                                                    : ExeDir() + L"\\icons\\folder.png";
            part.push_back(std::move(more));
            list = std::move(part);
        } else {
            pageCount_ = 1;
            page_ = 0;
        }
    }
    subOwned_ = std::move(list);
    subItems_.clear();
    for (const Item& c : subOwned_) subItems_.push_back(&c);
    hlSub_.assign(subItems_.size(), 0.0);
    hoverSub_ = -1;
}

void RadialMenu::Drill(Item folder) {
    if (!folder.dir) return;
    if (mode_ != Mode::Root) {
        if (hoverRoot_ < 0) return;
        mode_ = Mode::Root;
        subParent_ = hoverRoot_;
    }
    query_.clear();
    navStack_.push_back(std::move(folder));
    page_ = 0;
    RebuildSub();
    tSub_ = GetTickCount64();
    subSlide_ = 0;
    if (cfg_->sounds) Sound::Play(Sound::Select);
}

bool RadialMenu::Back() {
    if (mode_ == Mode::Search) {
        query_.clear();
        mode_ = Mode::None;
        subParent_ = -1;
        subItems_.clear();
        subOwned_.clear();
        hlSub_.clear();
        if (cfg_->sounds) Sound::Play(Sound::Back);
        return true;
    }
    if (navStack_.empty()) return false;
    navStack_.pop_back();
    page_ = 0;
    if (navStack_.empty() && !(*ring_)[subParent_].HasRing()) {
        mode_ = Mode::None;
        subParent_ = -1;
        subItems_.clear();
        subOwned_.clear();
        hlSub_.clear();
    } else {
        RebuildSub();
        tSub_ = GetTickCount64();
        subSlide_ = 0;
    }
    if (cfg_->sounds) Sound::Play(Sound::Back);
    return true;
}

void RadialMenu::TurnPage(int dir) {
    if (pageCount_ <= 1) return;
    page_ = (page_ + dir + pageCount_) % pageCount_;
    RebuildSub();
    tSub_ = GetTickCount64();
    subSlide_ = dir;
    if (cfg_->sounds) Sound::Play(Sound::Hover);
}

// Всё, что можно найти набором: секторы, их пункты и папки на два уровня вглубь.
void RadialMenu::BuildSearchPool() {
    PERF_SCOPE("menu.BuildSearchPool");
    poolBuilt_ = true;
    searchPool_.clear();
    std::set<std::wstring> seen;
    constexpr size_t kMax = 5000;
    auto add = [&](const Item& it) {
        if (searchPool_.size() >= kMax || it.full.empty()) return false;
        if (!seen.insert(Lower(it.full)).second) return false;
        searchPool_.push_back(it);
        return true;
    };
    auto addFolder = [&](const std::wstring& full, bool files, int depth) {
        auto list = ListFolder(full, files);
        for (const Item& c : list) {
            add(c);
            if (depth > 1 && c.dir)
                for (const Item& g : ListFolder(c.full, false)) add(g);
        }
    };
    for (const Item& root : *ring_) {
        add(root);
        for (const Item& c : root.children) {
            add(c);
            if (c.dir) addFolder(c.full, false, 1);
        }
        if (root.expand)
            addFolder(root.full, root.files, 2);
        else if (root.dir)
            addFolder(root.full, false, 1);
    }
}
