#include "ui/intro.h"

#include <d2d1helper.h>
#include <algorithm>
#include <cmath>

#include "motion/easing.h"

namespace ui {
using Microsoft::WRL::ComPtr;
namespace {
float Clamp01(double u) { return (float)std::clamp(u, 0.0, 1.0); }
float Phase(double t, double from, double to) { return Clamp01((t - from) / (to - from)); }
float EaseOut(float u) { return 1 - (1 - u) * (1 - u) * (1 - u); }
float EaseInOut(float u) { return u < .5f ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3.f) / 2; }
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, a}; }
}  // namespace

void Intro::Paint(ID2D1DeviceContext* dc, ID2D1Factory* factory, IDWriteFactory* dwrite, D2D1_SIZE_F size, double now,
                  D2D1_COLOR_F bg, D2D1_COLOR_F accent, ID2D1Bitmap* backdrop) {
    if (!Active(now)) {
        active_ = false;
        return;
    }
    if (factory != factoryUsed_) {
        // Полумесяц: круг минус сдвинутый круг; радиус 100, чтобы допуск сглаживания не рубил кончики
        moon_.Reset();
        ComPtr<ID2D1EllipseGeometry> outer, inner;
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(0, 0), 100, 100), &outer)) &&
            SUCCEEDED(factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(36, -24), 80, 80), &inner)) &&
            SUCCEEDED(factory->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            outer->CombineWithGeometry(inner.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
            sink->Close();
            moon_ = path;
        }
        factoryUsed_ = factory;
    }
    if (dc != dcUsed_ || !brush_) {
        brush_.Reset();
        dc->CreateSolidColorBrush(accent, &brush_);
        dcUsed_ = dc;
    }
    if (!title_ && dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 30, L"ru-ru", &title_);
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 11, L"ru-ru", &caption_);
        for (auto* format : {title_.Get(), caption_.Get()})
            if (format) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (!moon_ || !brush_) return;
    const double t = now - start_;
    const float w = size.width, h = size.height, cx = w / 2;
    // В маленьком окне (мини-полоска) всё меньше и без надписи
    const float unit = std::clamp(std::min(w, h) / 9.f, 9.f, 46.f);
    const bool words = h > 220;
    const float cy = h / 2 - (words ? unit * .6f : 0);
    const float cover = std::hypot(w, h) / 2 + 4;

    float moonScale, moonAlpha, rotate, hole, textAlpha, ringU;
    if (!closing_) {
        static const auto spring = motion::Easing::Spring({300, 16});
        const float in = Phase(t, 0, .62);
        moonScale = .35f + .65f * (float)spring.Eval(in);
        rotate = -38 * (1 - EaseOut(in));
        const float reveal = EaseInOut(Phase(t, .78, kOpen));
        moonAlpha = EaseOut(Phase(t, 0, .25)) * (1 - reveal);
        moonScale *= 1 + reveal * .7f;
        textAlpha = EaseOut(Phase(t, .3, .62)) * (1 - EaseOut(Phase(t, .78, 1.05)));
        hole = cover * reveal;
        ringU = Phase(t, .14, .9);
    } else {
        const float shut = EaseInOut(Phase(t, 0, .4));
        hole = cover * (1 - shut);
        moonAlpha = EaseOut(Phase(t, .12, .34)) * (1 - EaseOut(Phase(t, .4, kClose)));
        moonScale = 1.4f - .4f * EaseOut(Phase(t, .1, .4)) - .35f * EaseOut(Phase(t, .4, kClose));
        rotate = 24 * EaseOut(Phase(t, .38, kClose));
        textAlpha = 0;
        ringU = Phase(t, .3, kClose);
    }

    // Заставка - фон с дыркой-кругом: сквозь дырку виден плеер
    ComPtr<ID2D1RectangleGeometry> full;
    ComPtr<ID2D1EllipseGeometry> circle;
    ComPtr<ID2D1PathGeometry> veil;
    ComPtr<ID2D1GeometrySink> sink;
    factory->CreateRectangleGeometry(D2D1::RectF(-2, -2, w + 2, h + 2), &full);
    if (hole > .5f && full &&
        SUCCEEDED(factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), hole, hole), &circle)) &&
        SUCCEEDED(factory->CreatePathGeometry(&veil)) && SUCCEEDED(veil->Open(&sink))) {
        full->CombineWithGeometry(circle.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
        sink->Close();
    } else {
        veil.Reset();
    }
    D2D1_MATRIX_3X2_F previous;
    dc->GetTransform(&previous);
    dc->SetTransform(D2D1::IdentityMatrix());
    brush_->SetColor(WithAlpha(bg, 1));
    if (hole < cover - 1) {
        if (veil)
            dc->FillGeometry(veil.Get(), brush_.Get());
        else if (full)
            dc->FillGeometry(full.Get(), brush_.Get());
        if (backdrop) {
            // Картина заставки - в той же маске, что и фон: сквозь круг всё равно виден плеер
            ID2D1Geometry* mask = veil ? static_cast<ID2D1Geometry*>(veil.Get()) : static_cast<ID2D1Geometry*>(full.Get());
            const auto bs = backdrop->GetSize();
            const float k = std::max(w / bs.width, h / bs.height);
            const float dw = bs.width * k, dh = bs.height * k;
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask), nullptr);
            dc->DrawBitmap(backdrop, D2D1::RectF((w - dw) / 2, (h - dh) / 2, (w + dw) / 2, (h + dh) / 2), .9f);
            dc->PopLayer();
        }
        // Светящаяся кромка круга - граница между заставкой и плеером
        if (hole > .5f) {
            for (int i = 0; i < 4; ++i) {
                brush_->SetColor(WithAlpha(accent, .16f - i * .035f));
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), hole + i * 2.5f, hole + i * 2.5f), brush_.Get(),
                                2.5f);
            }
        }
    }
    // Мягкое свечение за луной
    if (moonAlpha > .01f) {
        D2D1_GRADIENT_STOP stops[] = {{0, WithAlpha(accent, .34f * moonAlpha)}, {1, WithAlpha(accent, 0)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> halo;
        dc->CreateGradientStopCollection(stops, 2, &collection);
        const float r = unit * 4.2f * moonScale;
        if (collection)
            dc->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r, r), collection.Get(),
                &halo);
        if (halo) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), halo.Get());
    }
    // Кольцо расходится волной от луны
    if (ringU > 0 && ringU < 1) {
        const float r = unit * (1.3f + ringU * 4.5f);
        brush_->SetColor(WithAlpha(accent, (1 - ringU) * .55f));
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_.Get(), 2 * (1 - ringU) + .5f);
    }
    if (moonAlpha > .01f) {
        const float r = unit * moonScale;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(r / 100, r / 100) * D2D1::Matrix3x2F::Rotation(rotate) *
                         D2D1::Matrix3x2F::Translation(cx, cy));
        brush_->SetColor(WithAlpha(accent, moonAlpha));
        dc->FillGeometry(moon_.Get(), brush_.Get());
        dc->SetTransform(D2D1::IdentityMatrix());
    }
    if (words && textAlpha > .01f && title_ && caption_) {
        const float rise = (1 - textAlpha) * 10;
        const float ty = cy + unit * 1.7f + rise;
        brush_->SetColor({.96f, .95f, .96f, textAlpha});
        dc->DrawText(L"PlayerMoon", 10, title_.Get(), D2D1::RectF(0, ty, w, ty + 44), brush_.Get());
        brush_->SetColor(WithAlpha(accent, textAlpha * .9f));
        dc->DrawText(L"М У З Ы К А   Р Я Д О М", 23, caption_.Get(), D2D1::RectF(0, ty + 44, w, ty + 64), brush_.Get());
    }
    dc->SetTransform(previous);
}

}  // namespace ui
