#pragma once
// Кольцо с геймпада (И22, XInput). Свой поток: опрос раз в 16 мс, пока геймпад подключён, иначе раз в 2 с.
// Зажать View (Back) на 0,6 с - целое кольцо в середине экрана; левый стик - сектор (дальше от центра - второе
// кольцо), A - открыть, B - назад / закрыть, LB - назад, RB - войти в папку. Всё уходит сообщением окну программы:
// поток ничего не рисует и меню не трогает. xinput1_4.dll грузится сама, нет - ничего не делает.
#include <windows.h>

namespace gamepad {

inline constexpr UINT WM_PAD = WM_APP + 72;  // wParam - Action, lParam - стик (MAKELPARAM x+32768, y+32768) для MOVE
enum Action { OPEN = 1, MOVE, ACCEPT, CANCEL, BACK, DRILL };

void Start(HWND target, bool enabled);  // повторный вызов меняет только enabled
void Stop();
bool Connected();  // для студии: есть ли геймпад сейчас

// Разбор кадра (для тестов без устройства): прошлое и нынешнее состояние кнопок, время зажатия View.
struct Edge {
    WORD was = 0, now = 0;
    bool Pressed(WORD b) const { return (now & b) && !(was & b); }
};

}  // namespace gamepad
