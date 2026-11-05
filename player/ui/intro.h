#pragma once
// Заставка при открытии и закрытии окна, поверх любого вида (главная, плеер, мини-полоска).
//
// Открытие: полумесяц пружинит из глубины, от него расходится кольцо, проявляется надпись,
// затем круг из центра луны раскрывает плеер. Закрытие - наоборот: окно сжимается в круг вокруг
// луны, луна гаснет. Рисуется обычным Direct2D поверх готового кадра.
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace ui {

class Intro {
public:
    static constexpr double kOpen = 1.3, kClose = .62;
    void Start(double now, bool closing) {
        start_ = now;
        closing_ = closing;
        active_ = true;
    }
    bool Active(double now) const { return active_ && now - start_ < (closing_ ? kClose : kOpen) + .05; }
    bool Closing() const { return closing_; }
    // bg - цвет заставки (фон вида), accent - цвет луны и кольца
    // backdrop - необязательная картина за луной (assets/intro/backdrop.png)
    void Paint(ID2D1DeviceContext* dc, ID2D1Factory* factory, IDWriteFactory* dwrite, D2D1_SIZE_F size, double now,
               D2D1_COLOR_F bg, D2D1_COLOR_F accent, ID2D1Bitmap* backdrop = nullptr);

private:
    double start_ = 0;
    bool closing_ = false, active_ = false;
    Microsoft::WRL::ComPtr<ID2D1Geometry> moon_;  // полумесяц радиуса 100 с центром в нуле
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_, caption_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1Factory* factoryUsed_ = nullptr;
    ID2D1DeviceContext* dcUsed_ = nullptr;
};

}  // namespace ui
