#pragma once
// Запись области экрана (Ф9): действие record - экран темнеет, рамкой выделяете область (Esc - отмена), запись идёт
// через ffmpeg (gdigrab, 30 к/с, H.264, без звука) в «Видео\Screen Recordings\Запись <дата время>.mp4». Вокруг области
// - тонкая рамка, под ней плашка с временем и «Стоп» (то же действие ещё раз или Esc на плашке тоже останавливают);
// рамка и плашка снаружи области и в запись не попадают. Не дольше 10 минут. Итог - карточка «Показать» /
// «Превратить»: кольцо конвертера (до 8 или 25 МБ под Discord, в GIF). record_logic.cpp - команда и область (под
// тестами).
#include <windows.h>

#include <string>

struct Config;

namespace record {

void Init(HINSTANCE inst, const Config* cfg);
void Toggle();  // нет записи - выбрать область, идёт - остановить
void Tick();
void Shutdown();
std::wstring TestRun(RECT r, int ms, const std::wstring& snap);  // --record-live: путь записи

// --- чистое ---
RECT Normalize(POINT a, POINT b);  // из двух углов, ширина и высота чётные (H.264), не меньше 16
std::wstring GrabArgs(const RECT& r, const std::wstring& out, int maxSec);
std::wstring Clock(int sec);  // «0:07», «1:23»

}  // namespace record
