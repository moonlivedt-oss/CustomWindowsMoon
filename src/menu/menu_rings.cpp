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
#include "core/fuzzy.h"
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

namespace {}  // namespace

double RadialMenu::Span() const { return full_ ? 180.0 : kHalfSpan; }

// Окно второго кольца: у края - весь полукруг, у целого кольца - веер вокруг своего сектора.
double RadialMenu::SubWinFrom() const { return full_ ? subMid_ - kHalfSpan : -Span(); }
double RadialMenu::SubWinTo() const { return full_ ? subMid_ + kHalfSpan : Span(); }

double RadialMenu::SubStep() const {
    int m = std::max<int>(1, (int)subItems_.size());
    double win = SubWinTo() - SubWinFrom();
    return m * kSubItemDeg <= win ? kSubItemDeg : std::max(kSubMinDeg, win / m);
}

double RadialMenu::SubMaxScroll() const {
    double win = SubWinTo() - SubWinFrom(), step = SubStep();
    return std::max(0.0, subItems_.size() - win / step);
}

double RadialMenu::SubFrom() const {
    double step = SubStep(), total = step * subItems_.size(), from = SubWinFrom(), to = SubWinTo();
    // всё влезло - держимся у своего сектора; не влезло - пункты идут от начала окна и прокручиваются
    double base = total <= to - from + 1e-6 ? std::clamp(subMid_ - total / 2, from, to - total) : from;
    return base - subScroll_ * step;
}

double RadialMenu::RootSlot(int i) const {
    int n = (int)ring_->size();
    double p = std::fmod(i - rootRot_, (double)n);
    return p < 0 ? p + n : p;
}

int RadialMenu::RootAt(double rel) const {
    int n = (int)ring_->size();
    double p = (rel + Span()) / (2 * Span() / n);
    int i = (int)std::floor(p + rootRot_) % n;
    return i < 0 ? i + n : i;
}

void RadialMenu::UpdateSubMid() {
    if (mode_ != Mode::Root || subParent_ < 0) return;
    double step = 2 * Span() / ring_->size();
    subMid_ = -Span() + step * (RootSlot(subParent_) + 0.5);
    if (full_ && subMid_ > 180) subMid_ -= 360;
}

void RadialMenu::ScrollSub(double items) {
    double to = std::clamp(std::round(subScrollTo_ + items), 0.0, SubMaxScroll());
    if (to == subScrollTo_) return;
    subScrollTo_ = to;
    if (cfg_->sounds) Sound::Play(Sound::Hover);
}

void RadialMenu::EnsureSubVisible(int idx) {
    double vis = (SubWinTo() - SubWinFrom()) / SubStep();
    if (idx < subScrollTo_) subScrollTo_ = idx;
    if (idx + 1 > subScrollTo_ + vis) subScrollTo_ = std::ceil(idx + 1 - vis);
    subScrollTo_ = std::clamp(subScrollTo_, 0.0, SubMaxScroll());
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
    constexpr int cap = 400;  // больше пунктов кольцом всё равно не пролистать
    if (mode_ == Mode::Search) {
        subMid_ = 0;
        std::wstring q = Lower(query_);
        std::vector<std::pair<int, const Item*>> found;
        for (const Item& it : searchPool_) {
            std::wstring n = Lower(it.name);
            int s = fuzzy::Score(n, q);
            if (s >= 0) found.push_back({s * 1000 + (int)std::min<size_t>(n.size(), 999), &it});
        }
        std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < found.size() && (int)i < cap; ++i) list.push_back(*found[i].second);
        kbSel_ = std::clamp(kbSel_, 0, std::max(0, (int)list.size() - 1));
    } else if (mode_ == Mode::Root && (subParent_ >= 0 || !navStack_.empty())) {
        if (!navStack_.empty()) {
            const Item& top = navStack_.back();
            list = top.dir ? ListFolder(top.full, true) : top.children;
        } else {
            const Item& root = (*ring_)[subParent_];
            list = root.children;
            if (root.expand)
                for (Item& c : ListFolder(root.full, root.files)) {
                    bool pinned = false;  // закреплённое в студии уже стоит первым среди своих пунктов
                    for (const Item& o : root.children)
                        if (!o.full.empty() && _wcsicmp(o.full.c_str(), c.full.c_str()) == 0) pinned = true;
                    if (!pinned) list.push_back(std::move(c));
                }
        }
        if ((int)list.size() > cap) list.resize(cap);
    }
    subOwned_ = std::move(list);
    subItems_.clear();
    for (const Item& c : subOwned_) subItems_.push_back(&c);
    hlSub_.assign(subItems_.size(), 0.0);
    hoverSub_ = -1;
    subScroll_ = subScrollTo_ = 0;
    if (mode_ == Mode::Search) subMid_ = 0;
    UpdateSubMid();
}

void RadialMenu::Drill(Item folder) {
    if (!folder.CanDrill()) return;
    if (mode_ == Mode::Search) {
        // папка из найденного: второе кольцо - её содержимое, «назад» до конца - к первому кольцу
        mode_ = Mode::Root;
        subParent_ = -1;
    } else if (mode_ != Mode::Root) {
        if (hoverRoot_ < 0) return;
        mode_ = Mode::Root;
        subParent_ = hoverRoot_;
    }
    query_.clear();
    navStack_.push_back(std::move(folder));
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
    if (navStack_.empty() && (subParent_ < 0 || !(*ring_)[subParent_].HasRing())) {
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
    double vis = (SubWinTo() - SubWinFrom()) / SubStep();
    ScrollSub(dir * std::max(1.0, std::floor(vis) - 1));
}

// Всё, что можно найти набором: секторы, их пункты и папки на два уровня вглубь.
void RadialMenu::BuildSearchPool() {
    PERF_SCOPE("menu.BuildSearchPool");
    poolBuilt_ = true;
    searchPool_.clear();
    std::set<std::wstring> seen;
    constexpr size_t kMax = 5000;
    auto add = [&](const Item& it) {
        // команды палитры (действия, окна, сочетания) без пути - по имени и ключу
        std::wstring key = it.full.empty() ? it.path + L"|" + it.name + (it.win ? L"|w" : L"") : it.full;
        if (searchPool_.size() >= kMax || (it.full.empty() && it.path.empty() && !it.win)) return false;
        if (!seen.insert(Lower(key)).second) return false;
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
    for (const Item& it : paletteExtra_) add(it);  // командная палитра: действия утилит, сети, студии, окна
}
