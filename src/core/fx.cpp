#include "core/fx.h"

#include <algorithm>

namespace fx {

namespace {
double g_mult = 1.0;  // во сколько раз длиннее обычного
bool g_game = false;
bool g_active = false;
void (*g_wake)() = nullptr;
}  // namespace

void SetMotion(int motion) { g_mult = motion == 0 ? 1.45 : motion == 2 ? 0.7 : 1.0; }
void SetGame(bool on) { g_game = on; }
bool Game() { return g_game; }

double Dur(double ms) { return g_game ? 1.0 : ms * g_mult; }

float Step(float perFrame) { return g_game ? 1.0f : std::min(1.0f, (float)(perFrame / g_mult)); }

void Active() { g_active = true; }

bool TakeActive() {
    bool a = g_active;
    g_active = false;
    return a;
}

void SetWaker(void (*wake)()) { g_wake = wake; }

void Wake() {
    g_active = true;
    if (g_wake) g_wake();
}

}  // namespace fx
