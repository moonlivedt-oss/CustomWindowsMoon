#pragma once
// Пипетка и линейка (Е3): действие picker - экран замирает, у курсора лупа с сеткой пикселей и цветом. Щелчок -
// цвет в буфер (#RRGGBB), Ctrl+щелчок - rgb(r, g, b); протянуть с зажатой кнопкой - замер: ширина, высота и
// расстояние в пикселях экрана, при отпускании «312 x 48» в буфер; Shift - только по горизонтали или вертикали.
// Стрелки двигают курсор на пиксель, Esc или правый щелчок - выйти. picker_logic.cpp - тексты (под тестами).
#include <windows.h>

#include <string>

struct Config;

namespace picker {

void Init(HINSTANCE inst, const Config* cfg);
void Show();
void Tick();
void Shutdown();
// снимок окна без окна и мыши: курсор в cur, замер от from (--life-snap picker)
bool Snap(POINT cur, POINT from, bool measure, const std::wstring& out);

// --- чистое ---
std::wstring Hex(BYTE r, BYTE g, BYTE b);      // #C8323F
std::wstring RgbText(BYTE r, BYTE g, BYTE b);  // rgb(200, 50, 63)
std::wstring Measure(int dx, int dy);          // «312 x 48, 315.7 px»
POINT Constrain(POINT from, POINT to);         // Shift: ближняя ось

}  // namespace picker
