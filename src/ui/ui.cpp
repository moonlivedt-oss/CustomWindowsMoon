#include "ui/ui.h"

#include <commctrl.h>

#include "core/fx.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

using namespace Gdiplus;

namespace ui {

namespace {
Color Alpha(Color c, BYTE a) { return Color(a, c.GetR(), c.GetG(), c.GetB()); }
// свои ключи Anim у элемента рядом с его id: нажатие, наведение, положение ручки
constexpr int kPressKey = 0x20000000, kHoverKey = 0x30000000, kKnobKey = 0x40000000;
}  // namespace

// ---------- ввод ----------

int Ui::HitAt(int x, int y) const {
    for (auto it = prevHits_.rbegin(); it != prevHits_.rend(); ++it)
        if (it->r.Contains((REAL)x, (REAL)y)) return it->id;
    return 0;
}

void Ui::OnMouseMove(int x, int y) {
    mouse_ = {x, y};
    int h = HitAt(x, y);
    if (h != hot_) {
        hotSince_ = GetTickCount64();
        hotChanged_ = true;
    }
    if (h != hot_ || (down_ && active_)) {
        hot_ = h;
        repaint_ = true;
    }
}

void Ui::OnRightDown(int x, int y) {
    mouse_ = {x, y};
    rdown_ = HitAt(x, y);
}

void Ui::OnRightUp(int x, int y) {
    mouse_ = {x, y};
    int h = HitAt(x, y);
    if (h && h == rdown_) rclicked_ = h;
    rdown_ = 0;
    repaint_ = true;
}

void Ui::OnDouble(int x, int y) {
    mouse_ = {x, y};
    dbl_ = HitAt(x, y);
    repaint_ = true;
}

bool Ui::RightClick(int id, const RectF& r) {
    Hover(id, r);
    if (interactive && rclicked_ == id) {
        rclicked_ = 0;
        return true;
    }
    return false;
}

bool Ui::DoubleClick(int id) {
    if (interactive && dbl_ == id && id) {
        dbl_ = 0;
        return true;
    }
    return false;
}

bool Ui::HotChanged() {
    bool r = hotChanged_;
    hotChanged_ = false;
    return r;
}

float Ui::Anim(int id, bool on, float speed) {
    auto it = anims_.find(id);
    float v = it == anims_.end() ? (on ? 1.0f : 0.0f) : it->second;
    float target = on ? 1.0f : 0.0f;
    float step = std::min(1.0f, dt_ * speed);
    v += (target - v) * step;
    if (std::fabs(target - v) < 0.01f)
        v = target;
    else
        repaint_ = true;
    anims_[id] = v;
    return v;
}

RectF Ui::Glide(int key, const RectF& t) {
    float target[4] = {t.X, t.Y, t.X + t.Width, t.Y + t.Height};
    auto it = glides_.find(key);
    if (it == glides_.end() || fx::Game()) {
        Spring s{};
        for (int i = 0; i < 4; ++i) s.e[i] = target[i];
        glides_[key] = s;
        return t;
    }
    Spring& s = it->second;
    // куда едет середина: по этой оси передний край жёстче, задний отстаёт и догоняет
    float dx = (target[0] + target[2]) - (s.e[0] + s.e[2]);
    float dy = (target[1] + target[3]) - (s.e[1] + s.e[3]);
    float speed = (float)(180.0 / std::max(1.0, fx::Dur(180)));  // «спокойно» мягче, «живо» резче
    bool moving = false;
    float left = dt_;
    while (left > 0) {  // мелкими шагами: пружина устойчива и при редких кадрах
        float h = std::min(left, 1 / 240.0f);
        left -= h;
        for (int i = 0; i < 4; ++i) {
            float d = (i % 2 == 0) ? dx : dy;
            bool front = (i < 2) ? d < 0 : d > 0;
            float k = (std::fabs(d) < 0.5f ? 300 : front ? 520 : 150) * speed * speed;
            float c = 2 * 0.72f * std::sqrt(k);
            float a = k * (target[i] - s.e[i]) - c * s.v[i];
            s.v[i] += a * h;
            s.e[i] += s.v[i] * h;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(target[i] - s.e[i]) > 0.3f || std::fabs(s.v[i]) > 4) { moving = true; }
    }
    if (!moving) {
        for (int i = 0; i < 4; ++i) {
            s.e[i] = target[i];
            s.v[i] = 0;
        }
    } else {
        repaint_ = true;
    }
    return RectF(s.e[0], s.e[1], std::max(0.0f, s.e[2] - s.e[0]), std::max(0.0f, s.e[3] - s.e[1]));
}

void Ui::Ripple(int id, const RectF& r, float radius) {
    auto it = ripples_.find(id);
    if (it == ripples_.end()) return;
    float t = (float)((GetTickCount64() - it->second.t) / fx::Dur(560));
    if (t >= 1) {
        ripples_.erase(it);
        return;
    }
    repaint_ = true;
    float px = (float)it->second.p.x, py = (float)it->second.p.y;
    // до дальнего угла рамки, чтобы круг успел залить её целиком
    float fx_ = std::max(std::fabs(px - r.X), std::fabs(px - r.X - r.Width));
    float fy = std::max(std::fabs(py - r.Y), std::fabs(py - r.Y - r.Height));
    float maxR = std::sqrt(fx_ * fx_ + fy * fy);
    float e = 1 - (1 - t) * (1 - t) * (1 - t);
    float rr = std::max(S(6), maxR * e);
    GraphicsState st = g_->Save();
    g_->SetClip(Round(r, radius), CombineModeIntersect);
    SolidBrush b(Color((BYTE)(52 * (1 - t)), 255, 255, 255));
    g_->FillEllipse(&b, px - rr, py - rr, rr * 2, rr * 2);
    g_->Restore(st);
}

namespace {
// время для ожидания: «спокойно» медленнее, «живо» быстрее
double LoopT(double periodMs) { return GetTickCount64() / std::max(1.0, fx::Dur(periodMs)); }

void AddHex(GraphicsPath& p, float cx, float cy, float rad) {
    PointF pts[6];
    for (int i = 0; i < 6; ++i) {
        float a = (60.0f * i - 90) * 3.14159265f / 180;
        pts[i] = PointF(cx + rad * std::cos(a), cy + rad * std::sin(a));
    }
    p.AddPolygon(pts, 6);
}
}  // namespace

void Ui::HexSpinner(const RectF& r, Color c) {
    Tick();
    float side = std::min(r.Width, r.Height);
    float R = side / 5.6f;  // радиус соты: семь сот укладываются в рамку
    float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    double t = LoopT(1100);
    double turn = t - std::floor(t);
    for (int i = 0; i < 7; ++i) {
        float px = cx, py = cy, v;
        if (i < 6) {
            float a = (60.0f * i - 60) * 3.14159265f / 180;
            px += std::cos(a) * R * 1.78f;
            py += std::sin(a) * R * 1.78f;
            // волна обходит круг: сота загорается, когда до неё доходит гребень, и медленно гаснет
            double ph = turn - i / 6.0;
            ph -= std::floor(ph);
            v = (float)std::pow(1 - ph, 3.0);
        } else {
            v = 0.35f + 0.25f * (float)std::sin(t * 6.2831853 * 0.5);
        }
        GraphicsPath hp;
        AddHex(hp, px, py, R * (0.72f + 0.2f * v));
        SolidBrush b(MixColor(MixColor(c_.field, c, 0.25f), MixColor(c, Color(255, 255, 255), 0.25f * v), v));
        g_->FillPath(&b, &hp);
    }
}

void Ui::Shimmer(const RectF& r, float radius, BYTE strength) {
    Tick();
    double t = LoopT(1500);
    float ph = (float)(t - std::floor(t));
    float bw = std::max(S(40), r.Width * 0.35f);
    float x = r.X - bw + (r.Width + bw * 2) * ph;
    GraphicsState st = g_->Save();
    g_->SetClip(Round(r, radius), CombineModeIntersect);
    // косая полоса: прозрачный край, светлая середина, прозрачный край
    LinearGradientBrush lb(PointF(x - bw / 2, r.Y), PointF(x + bw / 2, r.Y + bw * 0.2f), Color(0, 255, 255, 255),
                           Color(0, 255, 255, 255));
    Color cols[3] = {Color(0, 255, 255, 255), Color(strength, 255, 255, 255), Color(0, 255, 255, 255)};
    REAL pos[3] = {0, 0.5f, 1};
    lb.SetInterpolationColors(cols, pos, 3);
    g_->FillRectangle(&lb, x - bw / 2 - 1, r.Y, bw + 2, r.Height);
    g_->Restore(st);
}

float Ui::BounceDots(float x, float cy, Color c, float d) {
    Tick();
    double t = LoopT(900);
    float gap = S(3);
    for (int i = 0; i < 3; ++i) {
        double ph = t - i * 0.14;
        ph -= std::floor(ph);
        float up = ph < 0.4 ? (float)std::sin(ph / 0.4 * 3.14159265) : 0;  // подскок, потом пауза
        SolidBrush b(Alpha(c, (BYTE)(150 + 105 * up)));
        g_->FillEllipse(&b, x + i * (d + gap), cy - d / 2 - up * S(4), d, d);
    }
    return d * 3 + gap * 2;
}

void Ui::Skeleton(const RectF& r, int rows, float rowH) {
    // ширины «строк» разные, как у настоящего списка; блик бежит только по заготовкам
    static const float kW[] = {0.62f, 0.48f, 0.7f, 0.4f, 0.56f, 0.66f, 0.44f};
    GraphicsPath shapes;
    auto add = [&](const RectF& q, float rad) { shapes.AddPath(Round(q, rad), FALSE); };
    for (int i = 0; i < rows; ++i) {
        float y = r.Y + rowH * i + (rowH - S(12)) / 2;
        RectF box(r.X + S(10), y - S(1), S(16), S(14));
        add(box, S(4));
        add(RectF(box.GetRight() + S(12), y, (r.Width - S(140)) * kW[i % 7], S(12)), S(6));
        add(RectF(r.GetRight() - S(80), y, S(70), S(12)), S(6));
    }
    SolidBrush base(MixColor(c_.card, c_.text, 0.07f));
    g_->FillPath(&base, &shapes);
    GraphicsState st = g_->Save();
    g_->SetClip(&shapes, CombineModeIntersect);
    Shimmer(RectF(r.X, r.Y, r.Width, rowH * rows), 0, 40);
    g_->Restore(st);
}

void Ui::Tip(int id, const std::wstring& text) {
    if (interactive) tips_[id] = text;
}

void Ui::DrawTip(float w, float h) {
    auto it = tips_.find(hot_);
    if (!hot_ || it == tips_.end() || down_) return;
    if (GetTickCount64() - hotSince_ < 600) return;
    const std::wstring& t = it->second;
    float maxW = S(280);
    float tw = std::min(maxW, Measure(t, 11.5f) + S(4));
    float th = MeasureWrapped(t, 11.5f, false, tw) + S(14);
    RectF r(mouse_.x + S(14), mouse_.y + S(20), tw + S(20), th);
    if (r.X + r.Width > w - S(6)) r.X = w - r.Width - S(6);
    if (r.Y + r.Height > h - S(6)) r.Y = mouse_.y - r.Height - S(8);
    Fill(r, MixColor(c_.win, Color(0, 0, 0), 0.25f, 245), S(8));
    Stroke(r, c_.border, S(8));
    Text(t, RectF(r.X + S(10), r.Y + S(7), tw + S(2), th), c_.text, 11.5f, false, Left, true);
}

void Ui::OnLeave() {
    mouse_ = {-1, -1};
    if (hot_) {
        hot_ = 0;
        repaint_ = true;
    }
}

void Ui::OnDown(int x, int y) {
    mouse_ = press_ = {x, y};
    hot_ = HitAt(x, y);
    active_ = hot_;
    down_ = true;
    if (hot_ && interactive) {
        if (ripples_.size() > 16) ripples_.clear();  // круги элементов, которые больше не рисуются
        ripples_[hot_] = {press_, GetTickCount64()};
    }
    if (EditFocused()) DropFocus();
    SetCapture(hwnd_);
    repaint_ = true;
}

void Ui::OnUp(int x, int y) {
    mouse_ = {x, y};
    hot_ = HitAt(x, y);
    if (active_ && active_ == hot_) clicked_ = active_;
    released_ = active_;
    active_ = 0;
    down_ = false;
    ReleaseCapture();
    repaint_ = true;
}

void Ui::OnWheel(int x, int y, int delta) {
    for (auto it = prevScrollViews_.rbegin(); it != prevScrollViews_.rend(); ++it) {
        if (it->second.Contains((REAL)x, (REAL)y)) {
            scroll_[it->first] -= delta / 120.0f * S(64);
            repaint_ = true;
            return;
        }
    }
}

bool Ui::OnSetCursor() {
    if (hot_) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return true;
    }
    return false;
}

bool Ui::OnEditChange(HWND edit) {
    if (suppress_) return true;
    for (auto& [id, e] : edits_) {
        if (e.hwnd != edit) continue;
        int n = GetWindowTextLengthW(edit);
        std::wstring s(n, L'\0');
        GetWindowTextW(edit, s.data(), n + 1);
        if (e.onChange) e.onChange(s);
        repaint_ = true;
        return true;
    }
    return false;
}

bool Ui::EditFocused() const {
    HWND f = GetFocus();
    for (auto& [id, e] : edits_)
        if (e.hwnd == f) return true;
    return false;
}

void Ui::DropFocus() { SetFocus(hwnd_); }

void Ui::FocusField(int id, bool selectAll) {
    auto it = edits_.find(id);
    if (it == edits_.end() || !it->second.hwnd) return;
    SetFocus(it->second.hwnd);
    if (selectAll) SendMessageW(it->second.hwnd, EM_SETSEL, 0, -1);
}

bool Ui::FieldFocused(int id) const {
    auto it = edits_.find(id);
    return it != edits_.end() && it->second.hwnd && GetFocus() == it->second.hwnd;
}

// ---------- кадр ----------

void Ui::SetScale(double k) {
    k_ = k;
    fonts_.clear();
    if (editFont_) DeleteObject(editFont_);
    editFont_ = nullptr;
    icons_.Clear();
}

void Ui::Begin(Graphics& g, const UiColors& c, double k) {
    g_ = &g;
    c_ = c;
    if (k != k_) SetScale(k);
    hits_.clear();
    cards_.clear();
    scrollViews_.clear();
    tips_.clear();
    ULONGLONG now = GetTickCount64();
    dt_ = lastFrame_ ? std::min(0.1f, (now - lastFrame_) / 1000.0f) : 0.016f;
    lastFrame_ = now;
    for (auto& [id, e] : edits_) e.used = false;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    interactive = true;
}

void Ui::End() {
    for (auto& [id, e] : edits_)
        if (!e.used && IsWindowVisible(e.hwnd)) {
            if (GetFocus() == e.hwnd) DropFocus();
            ShowWindow(e.hwnd, SW_HIDE);
        }
    prevHits_ = hits_;
    prevScrollViews_ = scrollViews_;
    int h = HitAt(mouse_.x, mouse_.y);
    if (h != hot_) {
        hot_ = h;
        repaint_ = true;
    }
    clicked_ = 0;
    released_ = 0;
    rclicked_ = 0;
    dbl_ = 0;
    g_ = nullptr;
}

bool Ui::Hover(int id, const RectF& r) {
    if (!interactive) return false;
    hits_.push_back({id, r});
    return hot_ == id;
}

bool Ui::Click(int id, const RectF& r) {
    Hover(id, r);
    if (interactive && clicked_ == id) {
        clicked_ = 0;
        repaint_ = true;
        return true;
    }
    return false;
}

bool Ui::Released(int id) {
    if (released_ == id && id) {
        released_ = 0;
        return true;
    }
    return false;
}

bool Ui::Dragging(int id) const {
    if (active_ != id || !down_) return false;
    return std::abs(mouse_.x - press_.x) + std::abs(mouse_.y - press_.y) > S(6);
}

float Ui::Scroll(int id, const RectF& view, float content) {
    if (interactive) scrollViews_.push_back({id, view});
    float maxOff = std::max(0.0f, content - view.Height);
    float& target = scroll_[id];
    target = std::clamp(target, 0.0f, maxOff);
    // колесо задаёт цель, страница доезжает до неё плавно
    auto it = scrollShown_.find(id);
    float shown = it == scrollShown_.end() ? target : it->second;
    shown += (target - shown) * std::min(1.0f, dt_ * 16);
    if (std::fabs(target - shown) < 0.5f)
        shown = target;
    else
        repaint_ = true;
    scrollShown_[id] = shown;
    return shown;
}

// ---------- рисование ----------

GraphicsPath* Ui::Round(const RectF& r, float rad, bool top, bool bottom) {
    path_ = std::make_unique<GraphicsPath>();
    GraphicsPath& p = *path_;
    rad = std::min(rad, std::min(r.Width, r.Height) / 2);
    float d = rad * 2;
    if (rad <= 0.5f) {
        p.AddRectangle(r);
        return &p;
    }
    if (top) {
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    } else {
        p.AddLine(r.X, r.Y, r.X + r.Width, r.Y);
    }
    if (bottom) {
        p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    } else {
        p.AddLine(r.X + r.Width, r.Y + r.Height, r.X, r.Y + r.Height);
    }
    p.CloseFigure();
    return &p;
}

void Ui::Fill(const RectF& r, Color c, float radius) {
    SolidBrush b(c);
    if (radius <= 0)
        g_->FillRectangle(&b, r);
    else
        g_->FillPath(&b, Round(r, radius));
}

void Ui::Stroke(const RectF& r, Color c, float radius, float width, bool dashed) {
    Pen p(c, width);
    if (dashed) p.SetDashStyle(DashStyleDash);
    RectF in(r.X + width / 2, r.Y + width / 2, r.Width - width, r.Height - width);
    if (radius <= 0)
        g_->DrawRectangle(&p, in);
    else
        g_->DrawPath(&p, Round(in, radius));
}

Gdiplus::Font* Ui::Font(float size, bool bold) {
    long long key = (long long)(size * 100) * 2 + (bold ? 1 : 0);
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second.get();
    if (!fam_) {
        fam_ = std::make_unique<FontFamily>(L"Segoe UI");
        famBold_ = std::make_unique<FontFamily>(L"Segoe UI Semibold");
        if (famBold_->GetLastStatus() != Ok) famBold_ = std::make_unique<FontFamily>(L"Segoe UI");
    }
    auto f = std::make_unique<Gdiplus::Font>(bold ? famBold_.get() : fam_.get(), S(size), FontStyleRegular, UnitPixel);
    auto* raw = f.get();
    fonts_[key] = std::move(f);
    return raw;
}

void Ui::Text(const std::wstring& s, const RectF& r, Color c, float size, bool bold, Align h, bool wrap) {
    if (s.empty()) return;
    StringFormat sf(wrap ? 0 : StringFormatFlagsNoWrap);
    sf.SetAlignment(h == Left ? StringAlignmentNear : h == Center ? StringAlignmentCenter : StringAlignmentFar);
    sf.SetLineAlignment(wrap ? StringAlignmentNear : StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g_->DrawString(s.c_str(), -1, Font(size, bold), r, &sf, &b);
}

float Ui::Measure(const std::wstring& s, float size, bool bold) {
    if (s.empty()) return 0;
    RectF out;
    StringFormat sf(StringFormatFlagsNoWrap);  // тот же формат, что и при рисовании, иначе подпись режется
    g_->MeasureString(s.c_str(), -1, Font(size, bold), RectF(0, 0, 100000, 1000), &sf, &out);
    return out.Width;
}

float Ui::MeasureWrapped(const std::wstring& s, float size, bool bold, float width) {
    if (s.empty()) return 0;
    RectF out;
    g_->MeasureString(s.c_str(), -1, Font(size, bold), RectF(0, 0, width, 10000), nullptr, &out);
    return out.Height;
}

void Ui::Icon(const std::wstring& file, const RectF& r, const std::wstring& shellPath) {
    int px = (int)std::lround(std::min(r.Width, r.Height));
    if (px <= 0) return;
    Bitmap* b = icons_.Get(file, shellPath, px);
    if (!b) return;
    // значок уже нужного размера: рисуем один к одному по целым пикселям, без пересчёта сглаживания
    int x = (int)std::lround(r.X + (r.Width - px) / 2), y = (int)std::lround(r.Y + (r.Height - px) / 2);
    InterpolationMode im = g_->GetInterpolationMode();
    g_->SetInterpolationMode(InterpolationModeNearestNeighbor);
    g_->DrawImage(b, x, y, px, px);
    g_->SetInterpolationMode(im);
}

// Картинка «cover»: заполняет рамку, лишнее обрезается, углы скругляются.
void Ui::Picture(Bitmap* b, const RectF& r, float radius, bool roundBottom, BYTE alpha) {
    if (!b || !alpha) return;
    int w = (int)std::lround(r.Width), h = (int)std::lround(r.Height);
    if (w <= 0 || h <= 0) return;
    // Уменьшение с обрезкой и скругление дорогие, а карточки одни и те же: готовая картинка кэшируется.
    wchar_t key[96];
    swprintf(key, 96, L"%p|%d|%d|%d|%d", (void*)b, w, h, (int)std::lround(radius * 10), roundBottom ? 1 : 0);
    auto it = pictures_.find(key);
    if (it == pictures_.end()) {
        if (pictures_.size() > 64) pictures_.clear();  // после смены размера окна старые не нужны
        auto out = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        Graphics pg(out.get());
        pg.SetSmoothingMode(SmoothingModeAntiAlias);
        pg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        pg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        float iw = (float)b->GetWidth(), ih = (float)b->GetHeight();
        float k = std::max(w / iw, h / ih);
        float sw = w / k, sh = h / k;
        RectF dst(0, 0, (REAL)w, (REAL)h);
        Graphics* saved = g_;
        g_ = &pg;
        pg.SetClip(Round(dst, radius, true, roundBottom));
        g_ = saved;
        pg.DrawImage(b, dst, (iw - sw) / 2, (ih - sh) / 2, sw, sh, UnitPixel);
        it = pictures_.emplace(key, std::move(out)).first;
    }
    InterpolationMode im = g_->GetInterpolationMode();
    g_->SetInterpolationMode(InterpolationModeNearestNeighbor);
    Rect dst((int)std::lround(r.X), (int)std::lround(r.Y), w, h);
    if (alpha == 255) {
        g_->DrawImage(it->second.get(), dst);
    } else {
        ColorMatrix cm = {
            {{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha / 255.0f, 0}, {0, 0, 0, 0, 1}}};
        ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        g_->DrawImage(it->second.get(), dst, 0, 0, w, h, UnitPixel, &ia);
    }
    g_->SetInterpolationMode(im);
}

// ---------- элементы ----------

bool Ui::Button(int id, const RectF& r, const std::wstring& text, Kind kind, const std::wstring& icon, bool busy) {
    bool clicked = Click(id, r);
    bool pressed = active_ == id && down_;
    float ht = Anim(id, Hot(id));
    bool hot = ht > 0.5f;
    Color fill, fg, border(0, 0, 0, 0);
    switch (kind) {
        case Kind::Primary:
            fill = MixColor(c_.accent, Color(255, 255, 255), 0.12 * ht);
            fg = c_.onAccent;
            break;
        case Kind::Secondary:
            fill = MixColor(c_.card, c_.cardHi, ht);
            fg = c_.text;
            border = c_.border;
            break;
        case Kind::Ghost:
            fill = Color((BYTE)(c_.cardHi.GetA() * ht), c_.cardHi.GetR(), c_.cardHi.GetG(), c_.cardHi.GetB());
            fg = hot ? c_.text : c_.muted;
            break;
        case Kind::Danger:
            fill = hot ? MixColor(c_.card, c_.warn, 0.18) : c_.card;
            fg = c_.warn;
            border = c_.border;
            break;
    }
    // нажатая кнопка вдавливается и отпружинивает обратно, от точки нажатия расходится круг
    float pr = Anim(id + kPressKey, pressed, pressed ? 40 : 14);
    RectF rr = r;
    rr.Inflate(-r.Width * 0.025f * pr, -r.Height * 0.06f * pr);
    Fill(rr, fill, S(9));
    Ripple(id, rr, S(9));
    if (border.GetA()) Stroke(rr, border, S(9));
    float iw = icon.empty() ? 0 : S(18);
    float tw = Measure(text, 12.5f, true);
    float dotsW = busy ? S(4) * 3 + S(3) * 2 + S(6) : 0;
    float total = iw + (iw && !text.empty() ? S(7) : 0) + tw + dotsW;
    float x = rr.X + (rr.Width - total) / 2;
    if (iw) {
        Icon(icon, RectF(x, rr.Y + (rr.Height - iw) / 2, iw, iw));
        x += iw + S(7);
    }
    Text(text, RectF(x, rr.Y, rr.X + rr.Width - x, rr.Height), fg, 12.5f, true);
    if (busy) {
        BounceDots(x + tw + S(6), rr.Y + rr.Height / 2 + S(1), fg, S(4));
        Shimmer(rr, S(9), kind == Kind::Primary ? 46 : 30);
    }
    return clicked;
}

bool Ui::Toggle(int id, const RectF& r, bool& v) {
    bool clicked = Click(id, r);
    if (clicked) {
        v = !v;
        bursts_[id] = GetTickCount64();
    }
    float hot = Anim(id + kHoverKey, Hot(id), 16);
    float on = Anim(id + kKnobKey, v, 13);  // ручка едет, цвет дорожки перетекает
    RectF t(r.X, r.Y + (r.Height - S(22)) / 2, S(40), S(22));
    Color off = MixColor(c_.field, c_.cardHi, hot);
    Fill(t, MixColor(off, MixColor(c_.accent, Color(255, 255, 255), 0.08f * hot), on), t.Height / 2);
    if (on < 0.99f) Stroke(t, Alpha(c_.border, (BYTE)(c_.border.GetA() * (1 - on))), t.Height / 2);
    float d = S(16);
    float travel = t.Width - d - S(6);
    // в пути ручка вытягивается каплей назад, на месте снова круглая
    float stretch = std::sin(on * 3.14159f) * S(7);
    float kx = t.X + S(3) + travel * on - (v ? stretch : 0);
    float ky = t.Y + (t.Height - d) / 2;
    float cx = t.X + S(3) + travel * on + d / 2, cy = ky + d / 2;
    if (hot > 0.01f) {  // мягкий ореол вокруг ручки при наведении
        SolidBrush halo(Alpha(v ? c_.accent : c_.muted, (BYTE)(46 * hot)));
        float hd = d + S(12) * hot;
        g_->FillEllipse(&halo, cx - hd / 2, cy - hd / 2, hd, hd);
    }
    Fill(RectF(kx, ky, d + stretch, d), MixColor(c_.muted, c_.onAccent, on), d / 2);
    // щелчок: от ручки расходится кольцо, при включении ещё и разлетаются искры
    auto it = bursts_.find(id);
    if (it != bursts_.end()) {
        float bt = (float)((GetTickCount64() - it->second) / fx::Dur(620));
        if (bt >= 1) {
            bursts_.erase(it);
        } else {
            repaint_ = true;
            float e = 1 - (1 - bt) * (1 - bt) * (1 - bt);
            float fade = 1 - bt;
            float tx = v ? t.X + t.Width - S(3) - d / 2 : t.X + S(3) + d / 2;  // где ручка остановится
            float rr = d / 2 + S(15) * e;
            Pen ring(Alpha(v ? c_.accent : c_.muted, (BYTE)(210 * fade)), S(2.2f) * fade + S(0.4f));
            g_->DrawEllipse(&ring, tx - rr, cy - rr, rr * 2, rr * 2);
            if (v) {
                for (int i = 0; i < 8; ++i) {
                    float ang = i * 0.785398f + 0.39f;
                    float dist = d / 2 + S(6) + S(17) * e;
                    float pd = S(3.2f) * fade + S(0.6f);
                    SolidBrush sp(Alpha(i % 2 ? c_.gold : c_.accent, (BYTE)(255 * fade)));
                    g_->FillEllipse(&sp, tx + std::cos(ang) * dist - pd / 2, cy + std::sin(ang) * dist - pd / 2, pd,
                                    pd);
                }
            }
        }
    }
    return clicked;
}

bool Ui::Slider(int id, const RectF& r, int& v, int lo, int hi, int step) {
    Hover(id, r);
    bool changed = false;
    float x0 = r.X + S(8), x1 = r.X + r.Width - S(8);
    if (active_ == id && down_) {
        float t = std::clamp((mouse_.x - x0) / (x1 - x0), 0.0f, 1.0f);
        int nv = lo + (int)std::lround(t * (hi - lo) / step) * step;
        if (nv != v) {
            v = nv;
            changed = true;
        }
    }
    float t = hi > lo ? (float)(v - lo) / (hi - lo) : 0;
    float cy = r.Y + r.Height / 2;
    Fill(RectF(x0, cy - S(2), x1 - x0, S(4)), c_.field, S(2));
    Fill(RectF(x0, cy - S(2), (x1 - x0) * t, S(4)), c_.accent, S(2));
    // ручка подрастает при наведении, пока её тянут - вокруг ореол
    float grow = Anim(id + kKnobKey, Hot(id) || active_ == id, 18);
    float held = Anim(id + kPressKey, active_ == id && down_, 16);
    float kx = x0 + (x1 - x0) * t;
    if (held > 0.01f) {
        float hd = S(16) + S(14) * held;
        SolidBrush halo(Alpha(c_.gold, (BYTE)(50 * held)));
        g_->FillEllipse(&halo, kx - hd / 2, cy - hd / 2, hd, hd);
    }
    float d = S(14) + S(3) * grow;
    SolidBrush knob(c_.gold);
    g_->FillEllipse(&knob, kx - d / 2, cy - d / 2, d, d);
    return changed;
}

bool Ui::Segmented(int id, const RectF& r, const std::vector<std::wstring>& items, int& sel) {
    Fill(r, c_.field, S(9));
    Stroke(r, c_.border, S(9));
    bool changed = false;
    float w = (r.Width - S(6)) / items.size();
    auto item = [&](size_t i) { return RectF(r.X + S(3) + w * i, r.Y + S(3), w, r.Height - S(6)); };
    for (size_t i = 0; i < items.size(); ++i) {
        int iid = id * 16 + (int)i;
        if (Click(iid, item(i)) && sel != (int)i) {
            sel = (int)i;
            changed = true;
        }
        float hov = Anim(iid + kHoverKey, Hot(iid) && sel != (int)i, 18);
        if (hov > 0.01f) Fill(item(i), Alpha(c_.cardHi, (BYTE)(c_.cardHi.GetA() * hov)), S(7));
    }
    // выбранная плашка одна на всех и переезжает к новому пункту
    if (sel >= 0 && sel < (int)items.size()) {
        RectF pill = Glide(id, item(sel));
        Fill(pill, c_.accent, S(7));
        Ripple(id * 16 + sel, pill, S(7));
    }
    for (size_t i = 0; i < items.size(); ++i) {
        float on = Anim(id * 16 + (int)i + kKnobKey, sel == (int)i, 16);
        Text(items[i], item(i), MixColor(c_.muted, c_.onAccent, on), 12, on > 0.5f, Center);
    }
    return changed;
}

void Ui::Chip(const RectF& r, const std::wstring& t, Color bg, Color fg) {
    Fill(r, bg, S(5));
    Text(t, r, fg, 10.5f, true, Center);
}

float Ui::KeycapWidth(const std::wstring& t) { return std::max(S(38), Measure(t, 12, true) + S(22)); }

void Ui::Keycap(const RectF& r, const std::wstring& t) {
    RectF base(r.X, r.Y + S(3), r.Width, r.Height - S(3));
    Fill(base, MixColor(c_.win, Color(0, 0, 0), 0.4), S(8));
    RectF top(r.X, r.Y, r.Width, r.Height - S(4));
    Fill(top, c_.cardHi, S(8));
    Stroke(top, Alpha(c_.accent, 150), S(8), S(1.2f));
    Text(t, top, c_.text, 12, true, Center);
}

void Ui::Dots(const RectF& r, Color c) {
    SolidBrush b(c);
    float d = S(4), gap = S(4);
    float x = r.X + (r.Width - 3 * d - 2 * gap) / 2, y = r.Y + (r.Height - d) / 2;
    for (int i = 0; i < 3; ++i) g_->FillEllipse(&b, x + i * (d + gap), y, d, d);
}

HBRUSH Ui::FieldBrush() {
    COLORREF want = FieldBack();
    if (!fieldBrush_ || fieldBrushColor_ != want) {
        if (fieldBrush_) DeleteObject(fieldBrush_);
        fieldBrush_ = CreateSolidBrush(want);
        fieldBrushColor_ = want;
    }
    return fieldBrush_;
}

void Ui::Field(int id, const RectF& r, const std::wstring& value, std::function<void(const std::wstring&)> onChange,
               const std::wstring& placeholder) {
    Edit& e = edits_[id];
    if (!e.hwnd) {
        e.hwnd = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, nullptr,
                                 (HINSTANCE)GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE), nullptr);
    }
    if (!editFont_)
        editFont_ = CreateFontW(-(int)std::lround(S(13)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(e.hwnd, WM_SETFONT, (WPARAM)editFont_, FALSE);
    e.used = true;
    e.onChange = std::move(onChange);
    bool focused = GetFocus() == e.hwnd;
    Fill(r, c_.field, S(9));
    Stroke(r, focused ? c_.accent : c_.border, S(9), focused ? S(1.5f) : 1);

    float lineH = S(19);
    RECT pos{(LONG)(r.X + S(11)), (LONG)(r.Y + (r.Height - lineH) / 2), (LONG)(r.X + r.Width - S(10)),
             (LONG)(r.Y + (r.Height - lineH) / 2 + lineH)};
    if (memcmp(&pos, &e.pos, sizeof(RECT)) != 0 || !IsWindowVisible(e.hwnd)) {
        e.pos = pos;
        SetWindowPos(e.hwnd, nullptr, pos.left, pos.top, pos.right - pos.left, pos.bottom - pos.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (!interactive)
        EnableWindow(e.hwnd, FALSE);
    else
        EnableWindow(e.hwnd, TRUE);
    if (!focused) {
        int n = GetWindowTextLengthW(e.hwnd);
        std::wstring cur(n, L'\0');
        GetWindowTextW(e.hwnd, cur.data(), n + 1);
        if (cur != value) {
            suppress_ = true;
            SetWindowTextW(e.hwnd, value.c_str());
            suppress_ = false;
        }
    }
    if (!placeholder.empty()) SendMessageW(e.hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)placeholder.c_str());
}

}  // namespace ui
