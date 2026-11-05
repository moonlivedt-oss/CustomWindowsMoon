// Караоке и доли такта: голос в центре уходит, басы и края остаются; удары находятся по энергии
#include <cmath>

#include "core/karaoke.h"
#include "tests/check.h"

namespace {
// Средняя мощность выхода для синуса частоты hz с панорамой: pan 0 - центр, 1 - только левый канал
double Power(float hz, float pan, bool karaoke) {
    pm::VocalReducer v;
    v.SetSampleRate(48000);
    v.SetTarget(karaoke ? 1.f : 0.f);
    double sum = 0;
    const int n = 48000;
    for (int i = 0; i < n; ++i) {
        const float s = std::sin(2 * 3.14159265f * hz * i / 48000.f);
        float l = s * (pan > 0 ? 1.f : 1.f), r = s * (1 - pan);
        v.Process(l, r);
        if (i > n / 2) sum += l * l + r * r;  // после плавного включения
    }
    return sum;
}
}  // namespace

TEST(KaraokeRemovesCenterKeepsBassAndSides) {
    const double voice = Power(1000, 0, true) / Power(1000, 0, false);
    const double bass = Power(60, 0, true) / Power(60, 0, false);
    const double side = Power(1000, 1, true) / Power(1000, 1, false);
    CHECK(voice < .05);  // голос в центре - тише в 20 с лишним раз
    CHECK(bass > .6);    // бочка и бас остаются
    CHECK(side > .5);    // инструмент слева не пропадает
    // Выключено - звук не меняется
    pm::VocalReducer off;
    off.SetSampleRate(48000);
    float l = .3f, r = -.2f;
    off.Process(l, r);
    CHECK(l == .3f && r == -.2f && off.Idle());
}

TEST(BeatDetectorFindsPulsesNotNoise) {
    pm::BeatDetector d;
    int beats = 0;
    // 120 ударов в минуту: всплеск энергии каждые 0,5 с на фоне тихого уровня
    for (int frame = 0; frame < 60 * 8; ++frame) {
        const double t = frame / 60.0;
        const float e = std::fmod(t, .5) < 1 / 30.0 ? .8f : .15f;
        if (d.Feed(e, t)) ++beats;
    }
    CHECK(beats >= 14 && beats <= 17);
    // Ровный гул без всплесков - ни одного удара
    pm::BeatDetector flat;
    int hum = 0;
    for (int frame = 0; frame < 600; ++frame)
        if (flat.Feed(.5f, frame / 60.0) && frame > 60) ++hum;
    CHECK(hum == 0);
}
