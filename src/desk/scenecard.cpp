#include "desk/scenecard.h"

#include <algorithm>

#include "gfx/color.h"

using namespace Gdiplus;

namespace {

constexpr float kPad = 16;

bool StudioInFront() {
    wchar_t cls[64] = L"";
    GetClassNameW(GetForegroundWindow(), cls, 64);
    return wcscmp(cls, L"MoonRadialStudio") == 0;
}

}  // namespace

SceneCard& SceneCard::Get() {
    static SceneCard card;
    if (!card.created_) {
        card.created_ = true;
        card.CreateWnd(GetModuleHandleW(nullptr), L"MoonRadialScene", L"Scene");
    }
    return card;
}

void SceneCard::Show(Kind kind, const std::wstring& title, const ThemeInfo& theme) {
    if (StudioInFront()) return;
    kind_ = kind;
    title_ = title;
    theme_ = theme;
    doneGen_ = -1;
    moving_ = true;
    finds_ = 0;
    Open(0);
}

void SceneCard::Poll() {
    if (!Visible()) return;
    if (kind_ == CLEAN) {
        scene::CleanFeed f = scene::Clean();
        if (f.done && doneGen_ != f.gen) {
            doneGen_ = f.gen;
            Open(9000);
        }
    } else {
        scene::GuardFeed f = scene::Guard();
        if (f.done && doneGen_ != f.gen) {
            doneGen_ = f.gen;
            finds_ = f.finds.size();
            Open(f.finds.empty() ? 9000 : 30000);  // высота поменялась - Open перерисует с новой
        }
    }
}

float SceneCard::BodyHeight(Graphics&) {
    float sh = kind_ == CLEAN ? scene::CleanScene::Height(scene::Clean().items.size(), 1)
                              : scene::GuardScene::Height(finds_, 1);
    return kPad + 22 + 10 + sh + kPad - 4;
}

RectF SceneCard::SceneRect(const RectF& body) const {
    float K = k();
    return RectF(body.X + (kPad + 4) * K, body.Y + (kPad + 30) * K, body.Width - (kPad * 2 + 4) * K,
                 body.Height - (kPad + 30 + kPad - 4) * K);
}

void SceneCard::Paint(Graphics& g, const RectF& body) {
    float K = k();
    scene::Palette p;
    p.text = ToColor(theme_.fg);
    p.muted = ToColor(theme_.muted);
    p.faint = MixColor(p.muted, ToColor(theme_.bg), 0.45);
    p.accent = ToColor(theme_.ac);
    p.accent2 = ToColor(theme_.glow);
    p.gold = ToColor(theme_.gold);
    p.card = ToColor(theme_.panel);
    scene::Label(g, title_, RectF(body.X + (kPad + 4) * K, body.Y + kPad * K, body.Width - 60 * K, 22 * K), p.text,
                 14.5f * K);
    POINT m;
    GetCursorPos(&m);
    ScreenToClient(Wnd(), &m);
    RectF sr = SceneRect(body);
    if (kind_ == CLEAN)
        moving_ = clean_.Draw(g, sr, K, p, m, click_);
    else
        moving_ = guard_.Draw(g, sr, K, p, m);
    click_ = false;
}

int SceneCard::HitContent(const PointF& p) const { return SceneRect(Body()).Contains(p) ? H_SCENE : H_NONE; }

void SceneCard::Click(int id) {
    if (id != H_SCENE) return;
    click_ = true;
    moving_ = true;
}
