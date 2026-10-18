#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "desk/toast.h"

struct Config;

// Картинка из буфера обмена (CF_DIB) и в PNG - общее со историей буфера (desk/cliphist).
namespace snipimg {
std::unique_ptr<Gdiplus::Bitmap> FromDib(const BYTE* dib, SIZE_T size);
std::vector<uint8_t> ToPng(Gdiplus::Bitmap* b);
}  // namespace snipimg

// Карточка после снимка области (Win+Shift+S или горячий угол): справа снизу выезжает превью
// с кнопками «Сохранить» и «Текст»; клик по превью открывает снимок. «Текст» распознаёт снимок встроенным OCR
// Windows и кладёт текст в буфер обмена. Снимок замечается по буферу обмена: картинку туда
// положил «Ножницы» (ScreenClippingHost / SnippingTool). Стопка и движение - в Toast.
class SnipCard : public Toast {
public:
    void Create(HINSTANCE inst);
    void Apply(const Config& cfg);
    void Destroy();
    void Demo();  // для проверки: карточка со снимком кусочка экрана, как после «Ножниц»

private:
    enum { H_PREVIEW = H_USER, H_SAVE, H_TEXT };

    bool Message(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnClipboard();
    void Show();
    void Save(bool thenOpen);
    std::wstring SaveFolder() const;

    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    bool Busy() const override { return busy_; }
    Gdiplus::RectF R(int id) const;

    bool enabled_ = true;
    std::unique_ptr<Gdiplus::Bitmap> shot_, thumb_;
    std::vector<uint8_t> png_;
    int shotW_ = 0, shotH_ = 0;
    float pw_ = 150, ph_ = 104;  // превью, dip
    std::wstring status_, savedPath_;
    bool busy_ = false, statusGood_ = true;
    ULONGLONG busySince_ = 0;
};
