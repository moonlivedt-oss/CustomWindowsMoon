// Экран входа: три профиля. Выбор прячет лишнее в студии, трее и кольце утилит, но ничего не выключает.
#include <windows.h>

#include <algorithm>
#include <cmath>

#include "core/fx.h"
#include "core/profile.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {
const wchar_t* Avatar(profile::Level l) {
    return l == profile::Simple ? L"mascot-wave.webp" : l == profile::Dev ? L"mascot-point.webp" : L"mascot-read.webp";
}
}  // namespace

void Studio::ChooseProfile(int level) {
    profile::Choose((profile::Level)level);
    profile::SetPeek(false);
    Change();
    cfg.profile = profile::CurrentKey();
    Save();
    // открытая страница могла оказаться скрытой
    if (!PageFits(page_)) GoPage(PAGE_STATE);
    Log(std::wstring(L"Профиль: ") + profile::Name((profile::Level)level));
}

void Studio::LoginOverlay(float w, float h) {
    const UiColors& c = ui.C();
    ULONGLONG now = GetTickCount64();
    if (!loginAt_) loginAt_ = now;
    double openMs = (double)(now - loginAt_);
    float dim = (float)std::min(1.0, openMs / std::max(1.0, fx::Dur(180)));
    ui.Fill(RectF(0, 0, w, h), Color((BYTE)(232 * dim), 8, 6, 12));
    ui.Hover(ID_OV_BG, RectF(0, 0, w, h));

    float cw = ui.S(240), ch = ui.S(320), gap = ui.S(24);
    float total = cw * profile::kCount + gap * (profile::kCount - 1);
    if (total > w - ui.S(48)) {  // узкое окно
        cw = (w - ui.S(48) - gap * (profile::kCount - 1)) / profile::kCount;
        total = w - ui.S(48);
    }
    float x0 = (w - total) / 2, y0 = std::max(ui.S(92), (h - ch) / 2 - ui.S(18));
    ui.Text(L"Кто сегодня за компьютером?", RectF(0, y0 - ui.S(66), w, ui.S(36)), c.text, 22, true, ui::Center);

    bool first = !profile::Chosen();
    int cur = first ? -1 : (int)profile::Current();
    for (int i = 0; i < profile::kCount; ++i) {
        profile::Level l = (profile::Level)i;
        int id = ID_LOGIN + i;
        RectF base(x0 + i * (cw + gap), y0, cw, ch);
        bool click = ui.Click(id, base);
        float hov = ui.Anim(id + 10, ui.Hot(id), 16);
        RectF r = base;
        // появление по очереди с лёгким перелётом
        double t = std::clamp((openMs - i * 70.0) / std::max(1.0, fx::Dur(460)), 0.0, 1.0);
        double back = 1 + 1.7 * std::pow(t - 1, 3) + 0.7 * std::pow(t - 1, 2);
        if (t < 1) ui.Invalidate();
        r.Y += ui.S(46) * (float)(1 - back) - ui.S(8) * hov;
        for (int s = 3; s >= 1; --s)  // мягкая тень под приподнятой карточкой
            ui.Fill(RectF(r.X - s * ui.S(2), r.Y + ui.S(4) * s * hov, r.Width + s * ui.S(4), r.Height),
                    Color((BYTE)(26 * hov), 0, 0, 0), ui.S(22));
        ui.Fill(r, MixColor(c.card, c.accent, 0.02f + 0.08f * hov), ui.S(20));
        ui.Stroke(r, i == cur ? c.accent : MixColor(c.border, c.accent, hov), ui.S(20), ui.S(i == cur ? 2.0f : 1.0f));
        ui.Spotlight(r, ui.S(20));
        ui.Ripple(id, r, ui.S(20));

        float pic = std::min(ui.S(138), r.Width - ui.S(40));
        ui.Icon(ExeDir() + L"\\icons\\" + Avatar(l), RectF(r.X + (r.Width - pic) / 2, r.Y + ui.S(24), pic, pic));
        float ty = r.Y + ui.S(24) + pic + ui.S(14);
        ui.Text(profile::Name(l), RectF(r.X, ty, r.Width, ui.S(28)), c.text, 16, true, ui::Center);
        ui.Text(profile::Blurb(l), RectF(r.X + ui.S(18), ty + ui.S(32), r.Width - ui.S(36), ui.S(44)), c.muted, 12,
                false, ui::Center, true);
        // сколько открыто: три точки, как уровень
        float d = ui.S(7), dg = ui.S(7), dx = r.X + (r.Width - 3 * d - 2 * dg) / 2, dy = r.Y + r.Height - ui.S(26);
        for (int k = 0; k < 3; ++k) {
            SolidBrush b(k <= i ? c.accent : WithAlpha(c.faint, 90));
            ui.G().FillEllipse(&b, dx + k * (d + dg), dy, d, d);
        }
        ui.Detail(base, profile::Name(l),
                  std::wstring(profile::Holds(l)) + L" Скрытое продолжает работать и находится через палитру команд.");
        if (click) {
            int level = i;
            bool tour = !cfg.tourDone;
            Defer([this, level, tour] {
                ChooseProfile(level);
                ov_ = tour ? OV_TOUR : OV_NONE;
                tourStep_ = 0;
            });
        }
    }

    // под карточками: спрашивать ли каждый раз и тур
    float by = y0 + ch + ui.S(26);
    RectF ask(x0, by, std::min(total, ui.S(340)), ui.S(30));
    ui.Text(L"Спрашивать при каждом открытии", RectF(ask.X + ui.S(54), ask.Y, ask.Width, ask.Height), c.muted, 12);
    bool on = cfg.profileAsk != 0;
    if (ui.Toggle(ID_LOGIN_ASK, RectF(ask.X, ask.Y, ui.S(42), ui.S(30)), on)) {
        Change();
        cfg.profileAsk = on ? 1 : 0;
        Save();
    }
    if (ui.Button(ID_LOGIN_TOUR, RectF(x0 + total - ui.S(116), by - ui.S(2), ui.S(116), ui.S(34)), L"Тур", Kind::Ghost))
        Defer([this] {
            ov_ = OV_TOUR;
            tourStep_ = 0;
        });
    if (!first && ui.Button(ID_LOGIN_CLOSE, RectF(x0 + total - ui.S(240), by - ui.S(2), ui.S(116), ui.S(34)), L"Назад",
                            Kind::Ghost))
        Defer([this] { ov_ = OV_NONE; });
}

}  // namespace studio
