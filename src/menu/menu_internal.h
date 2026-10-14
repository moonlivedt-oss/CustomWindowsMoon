#pragma once
// Общее для файлов меню: постоянные, сообщения окна, палитра.
#include <windows.h>

#include "core/config.h"
#include "core/fx.h"
#include "core/themes.h"

namespace menu_detail {

inline constexpr wchar_t kClass[] = L"MoonRadialMenu";
inline constexpr UINT_PTR kTimer = 1;
inline constexpr UINT WM_OUTSIDE_CLICK = WM_APP + 5;
inline constexpr UINT WM_MENU_KEY = WM_APP + 6;
inline constexpr UINT WM_MENU_CHAR = WM_APP + 7;
inline constexpr UINT WM_MENU_WHEEL = WM_APP + 8;

inline constexpr double kHalfSpan = 84.0;  // полураскрыв колец в градусах от направления «внутрь»
inline constexpr double kSubItemDeg = 17.0;
inline constexpr double kOpenMs = 230, kSubMs = 190, kCloseMs = 120;
inline constexpr double kLaunchMs = 300;   // волна от сектора при запуске
inline constexpr double kStaggerMs = 24;   // секторы первого кольца появляются по очереди
inline constexpr double kSubWaveMs = 14;   // второе кольцо - волной от середины к краям
inline constexpr ULONGLONG kSwitchDelay = 260;  // столько держать курсор на другом секторе, чтобы уйти из папки
inline constexpr ULONGLONG kDwellMs = 750;      // столько держать файл над папкой, чтобы в неё зайти

// Приёмник перетаскивания (menu_drop.cpp).
IDropTarget* CreateMenuDrop(class RadialMenu* m);

// Цвета колец берутся из общей темы (core/themes.cpp).
using Palette = RingColors;
inline Palette Pal(const Config* c) { return RingPalette(*c); }

}  // namespace menu_detail
