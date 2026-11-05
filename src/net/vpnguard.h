#pragma once
#include "net/net.h"

namespace net {
inline constexpr wchar_t kVpnGuardPrefix[] = L"MoonRadial: VPN guard - ";
bool VpnGuardJob(int mode, const Log& log);  // 0 — проверить, 1 — настроить после UAC, 2 — снять свои правила
bool VpnGuardConfigured();
bool VpnGuardMatches(const std::wstring& route, const std::wstring& apps);
}
