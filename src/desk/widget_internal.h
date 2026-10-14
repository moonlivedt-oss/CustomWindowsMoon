#pragma once
// Общее для логики панели «Сейчас играет» (widget.cpp) и её отрисовки (widget_draw.cpp).
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <string>

namespace widget_detail {

constexpr wchar_t kClass[] = L"MoonRadialMedia";
constexpr UINT WM_TRACK = WM_APP + 1, WM_LYRICS = WM_APP + 2;
constexpr float kW = 452, kH = 164;   // карточка, до масштаба
constexpr float kLyr = 260;           // на сколько карточка вырастает вверх под текст песни
constexpr float kTW = 340, kTH = 78;  // маленькая карточка при смене трека
constexpr float kM = 30;              // поля под тень
constexpr float kPad = 24;            // запас картинки тени вокруг карточки
constexpr float kRadius = 22;
constexpr float kLift = 18;     // насколько карточка поднимается над нижним краем
constexpr float kEdgeGap = 80;  // у самых углов край не считается: там горячие углы
constexpr ULONGLONG kDwell = 160, kLinger = 380, kToast = 3200, kVolShow = 1400;

std::wstring Clock(int64_t t);
void RoundRect(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float rad);
void Triangle(Gdiplus::Graphics& g, Gdiplus::Brush* b, float x, float y, float s, bool right);
Gdiplus::StringFormat* LineFormat(Gdiplus::StringAlignment h = Gdiplus::StringAlignmentNear);

}  // namespace widget_detail
