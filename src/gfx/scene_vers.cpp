// Табло версий (см. scene.h, VersScene): строки перещёлкиваются на свежую версию, поезд релизов, конец поддержки.
#include <algorithm>
#include <cmath>

#include "gfx/color.h"
#include "gfx/scene.h"

using namespace Gdiplus;

namespace scene {

bool VersScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, const Input& in,
                     const std::vector<VerRow>& rows) {
    Step(r, k, in);
    float W = r.Width, ox = r.X, oy = r.Y;
    std::wstring key;
    for (const VerRow& v : rows) key += v.name + L"|" + v.from + L"|" + v.to + L";";
    if (key != key_) {  // новый итог проверки - табло перещёлкивается заново
        key_ = key;
        at_.clear();
        for (size_t i = 0; i < rows.size(); ++i) at_.push_back(t_ + 0.3 + i * 0.3);
        Wake(4 + rows.size() * 0.3);
    }
    hot_ = -1;
    if (over_)
        for (size_t i = 0; i < rows.size(); ++i)
            if (std::fabs(my_ - (34 * k + i * 56 * k)) < 24 * k) hot_ = (int)i;
    if (click_ && hot_ >= 0) at_[hot_] = t_, Wake(2.5);
    const wchar_t* CH = L"0123456789.абвгдежзиклмнопрст";
    const int nCH = (int)wcslen(CH);
    int cells = 10;  // табло шире, если строка длиннее («111 пакетов»)
    for (const VerRow& v : rows) cells = std::max(cells, (int)std::max(v.from.size(), v.to.size()));
    cells = std::min(cells, 13);
    float bx0 = 100 * k, cell = 15 * k;
    bool moving = false;
    Color pale = Pale(p);

    GraphicsState gs = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    for (size_t i = 0; i < rows.size(); ++i) {
        const VerRow& v = rows[i];
        bool up = !v.to.empty() && v.to != v.from;
        std::wstring to = v.to.empty() ? v.from : v.to;
        float y = 34 * k + i * 56 * k;
        float pr = game_ ? 1.0f : std::clamp((float)(t_ - at_[i]) / 0.8f, 0.0f, 1.0f);
        float a = game_ ? 1.0f : Ease((float)(t_ - at_[i] - 0.6) / 0.6f);
        if (pr < 1 || a < 1) moving = true;
        if (hot_ == (int)i)
            FillRound(g, RectF(ox + 8 * k, oy + y - 22 * k, W - 16 * k, 46 * k), 8 * k, MixColor(p.card, p.text, 0.05));
        Label(g, v.name, RectF(ox + 20 * k, oy + y - 16 * k, bx0 - 24 * k, 18 * k), p.text, 13 * k, true);
        Label(g, !up ? std::wstring(L"свежая") : iswdigit(v.from[0]) ? L"было " + v.from : std::wstring(L"есть новее"),
              RectF(ox + 20 * k, oy + y + 2 * k, bx0 - 24 * k, 15 * k), p.faint, 10 * k);
        // табло: ячейки переворачиваются по очереди, посередине поворота - случайные знаки
        for (int c = 0; c < cells; ++c) {
            float kk = std::clamp(pr * 1.7f - c * 0.07f, 0.0f, 1.0f);
            wchar_t ch = kk >= 1   ? (c < (int)to.size() ? to[c] : L' ')
                         : kk <= 0 ? (c < (int)v.from.size() ? v.from[c] : L' ')
                                   : CH[rand() % nCH];
            float x = bx0 + c * (cell + 1 * k);
            float fl = kk > 0 && kk < 1 ? std::fabs(std::cos(kk * 3.14159265f * 3)) : 1;
            FillRound(g, RectF(ox + x, oy + y - 14 * k + 14 * k * (1 - fl), cell, 28 * k * fl), 3 * k, Deep(p));
            SolidBrush seam(Color(100, 0, 0, 0));
            g.FillRectangle(&seam, ox + x, oy + y - 0.5f, cell, 1.0f);
            if (fl > 0.3f)
                Label(g, std::wstring(1, ch), RectF(ox + x - 2 * k, oy + y - 9 * k, cell + 4 * k, 18 * k),
                      kk >= 1 ? (up ? p.text : p.muted) : p.muted, 12 * k, true, 1);
        }
        // поезд релизов: от своей ветки до последней
        float tx = bx0 + cells * (cell + 1 * k) + 18 * k, tw = W - tx - 26 * k;
        if (v.train.size() > 1) {
            Pen line(WithAlpha(p.faint, 153), 1);
            g.DrawLine(&line, ox + tx, oy + y, ox + tx + tw * a, oy + y);
            size_t n = v.train.size();
            for (size_t q = 0; q < n; ++q) {
                float x = tx + tw * q / (n - 1) * a;
                bool last = q == n - 1, first = q == 0;
                float rr = (last || first ? 5 : 3.5f) * k;
                SolidBrush db(last ? p.accent : first ? pale : p.faint);
                g.FillEllipse(&db, ox + x - rr, oy + y - rr, rr * 2, rr * 2);
                if (a > 0.9f)
                    Label(g, v.train[q], RectF(ox + x - 30 * k, oy + y - 21 * k, 60 * k, 14 * k),
                          last    ? p.accent
                          : first ? pale
                                  : p.faint,
                          9.5f * k, last || first, 1);
            }
            if (a > 0.9f) {
                int behind = (int)n - 1;
                Label(g,
                      std::to_wstring(behind) + (behind == 1  ? L" ветка позади"
                                                 : behind < 5 ? L" ветки позади"
                                                              : L" веток позади"),
                      RectF(ox + tx, oy + y + 6 * k, tw, 14 * k), p.muted, 10 * k, false, 1);
            }
        } else if (!up) {
            SolidBrush db(p.accent2);
            g.FillEllipse(&db, ox + tx + 3 * k, oy + y - 5 * k, 10 * k, 10 * k);
            Label(g, L"последняя", RectF(ox + tx + 20 * k, oy + y - 8 * k, 120 * k, 16 * k), p.accent2, 10.5f * k);
        } else {
            RectF pill(ox + tx, oy + y - 11 * k, 92 * k, 22 * k);
            FillRound(g, pill, 11 * k, WithAlpha(p.accent, 50));
            Label(g, L"обновить", pill, p.accent, 11 * k, true, 1);
        }
        if (!v.eol.empty() && a > 0.9f) {
            float pu = Awake() ? 0.6f + 0.4f * std::sin((float)t_ * 4) : 1;
            RectF pill(ox + 20 * k, oy + y + 17 * k, std::max(140 * k, Measure(g, v.eol, 9.5f * k, true) + 16 * k),
                       15 * k);
            FillRound(g, pill, 7 * k, WithAlpha(p.accent, (BYTE)(255 * (0.25f * pu + 0.1f))));
            Label(g, v.eol, pill, p.accent, 9.5f * k, true, 1);
            if (Awake()) moving = true;
        }
    }
    Label(g, L"щёлкни по строке - перещёлкнуть заново", RectF(ox, oy + r.Height - 16 * k, W - 8 * k, 15 * k), p.faint,
          11 * k, false, 2);
    g.Restore(gs);
    return moving && !game_;
}

}  // namespace scene
