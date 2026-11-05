#pragma once
// Сравнение двух снимков «до и после»: шторка, которую тянут мышью, или рядом. Открывается из ленты карточки снимка.
#include <memory>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

struct UiColors;

namespace snipcompare {

void Show(const UiColors& colors, std::unique_ptr<Gdiplus::Bitmap> left, std::unique_ptr<Gdiplus::Bitmap> right);
void Close();

}  // namespace snipcompare
