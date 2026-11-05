#pragma once
// Снимок, закреплённый поверх окон: удобно сверять с ним работу или списывать значения. Тянется мышью, колесо меняет
// размер, правый щелчок, двойной щелчок или крестик под курсором закрывают. Фокус не отнимает.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace pin {

void Show(Gdiplus::Bitmap& image);  // картинка копируется, исходную можно менять и удалять
void CloseAll();
int Count();

}  // namespace pin
