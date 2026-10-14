#include "gfx/sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Sound {
namespace {

constexpr int kRate = 22050;
std::vector<BYTE> g_wav[Count];
bool g_ready = false;

std::vector<BYTE> MakeWav(const std::vector<float>& s, double gain) {
    std::vector<BYTE> out(44 + s.size() * 2);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&out[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&out[at], &v, 2); };
    memcpy(&out[0], "RIFF", 4);
    put32(4, (uint32_t)(36 + s.size() * 2));
    memcpy(&out[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&out[36], "data", 4);
    put32(40, (uint32_t)(s.size() * 2));
    for (size_t i = 0; i < s.size(); ++i) {
        int16_t v = (int16_t)std::clamp(s[i] * gain * 32767.0, -32767.0, 32767.0);
        memcpy(&out[44 + i * 2], &v, 2);
    }
    return out;
}

// Тон с плавной сменой частоты f0 -> f1 и затуханием.
std::vector<float> Tone(double ms, double f0, double f1, double decay, double attackMs = 2) {
    size_t n = (size_t)(kRate * ms / 1000);
    std::vector<float> s(n);
    double phase = 0;
    for (size_t i = 0; i < n; ++i) {
        double t = (double)i / n;
        double f = f0 + (f1 - f0) * t;
        phase += 2 * 3.14159265358979 * f / kRate;
        double env = std::exp(-decay * t) * std::min(1.0, i / (kRate * attackMs / 1000));
        env *= std::min(1.0, (n - i) / (kRate * 0.004));  // без щелчка в конце
        s[i] = (float)(std::sin(phase) * 0.8 + std::sin(phase * 2) * 0.12) * (float)env;
    }
    return s;
}

std::vector<float> Concat(std::vector<float> a, const std::vector<float>& b, double gapMs = 0) {
    a.resize(a.size() + (size_t)(kRate * gapMs / 1000), 0.f);
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Наложить b на a со сдвигом (для аккордов).
std::vector<float> Mix(std::vector<float> a, const std::vector<float>& b, double atMs, double gain) {
    size_t at = (size_t)(kRate * atMs / 1000);
    if (a.size() < at + b.size()) a.resize(at + b.size(), 0.f);
    for (size_t i = 0; i < b.size(); ++i) a[at + i] += (float)(b[i] * gain);
    return a;
}

}  // namespace

void Init(int volume) {
    double v = std::clamp(volume, 0, 100) / 100.0;
    g_wav[Open] = MakeWav(Tone(110, 330, 560, 4.5, 6), 0.55 * v);
    g_wav[Hover] = MakeWav(Tone(22, 1900, 1700, 7), 0.16 * v);
    g_wav[Select] = MakeWav(Concat(Tone(55, 660, 680, 5), Tone(90, 990, 1000, 5), 8), 0.45 * v);
    g_wav[Back] = MakeWav(Tone(80, 560, 360, 5), 0.4 * v);
    // вход в систему: мягкий подъём и два тихих колокольчика, пока раскрывается портал
    g_wav[Portal] = MakeWav(Mix(Mix(Tone(820, 196, 392, 3.2, 240), Tone(520, 1175, 1180, 6, 4), 260, 0.32),
                                Tone(480, 1568, 1572, 7, 4), 420, 0.22),
                            0.42 * v);
    g_wav[Drop] = MakeWav(Concat(Tone(70, 220, 120, 4), Tone(60, 880, 900, 6), 4), 0.55 * v);
    g_ready = v > 0;
}

void Play(Kind k) {
    if (!g_ready || g_wav[k].empty()) return;
    PlaySoundW((LPCWSTR)g_wav[k].data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}  // namespace Sound
