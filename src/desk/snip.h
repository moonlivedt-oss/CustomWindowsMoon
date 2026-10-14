#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/themes.h"

struct Config;

// Карточка после снимка области (Win+Shift+S или горячий угол): справа снизу выезжает превью
// с кнопками «Сохранить» и «Текст». «Текст» распознаёт снимок встроенным OCR Windows
// и кладёт текст в буфер обмена. Снимок замечается по буферу обмена: картинку туда
// положил «Ножницы» (ScreenClippingHost / SnippingTool).
class SnipCard {
public:
    void Create(HINSTANCE inst);
    void Apply(const Config& cfg);
    void Tick();  // из общего опроса раз в 20 мс
    void Destroy();

private:
    enum Hit { H_NONE, H_PREVIEW, H_SAVE, H_TEXT, H_CLOSE, H_BODY, kHits };

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnClipboard();
    void Show();
    void Render();
    Gdiplus::RectF Rect(Hit h) const;
    Hit HitAt(POINT p) const;
    void Save(bool thenOpen);
    std::wstring SaveFolder() const;

    HWND wnd_ = nullptr;
    bool enabled_ = true;
    ThemeInfo theme_{};
    std::unique_ptr<Gdiplus::Bitmap> shot_, thumb_;
    std::vector<uint8_t> png_;
    int shotW_ = 0, shotH_ = 0;
    std::wstring status_, savedPath_;
    bool busy_ = false, statusGood_ = true;
    bool open_ = false, tracking_ = false;
    float slide_ = 0, hover_[kHits] = {};
    Hit hot_ = H_NONE, down_ = H_NONE;
    ULONGLONG until_ = 0;
    double k_ = 1;
    RECT work_{};
    bool dirty_ = false;
};
