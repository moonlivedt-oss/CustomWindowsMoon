#pragma once
#include <string>
#include <windows.h>

// Bundled, on-demand player. Uses the existing Windows media-session controls.
namespace playermoon {
inline constexpr int kOpenControl = 20220, kSectorControl = 20221;
std::wstring Path();
bool Installed();
bool Open(HWND owner);
}
