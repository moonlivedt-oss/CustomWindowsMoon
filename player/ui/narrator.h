#pragma once
// Экранный диктор (Экранный диктор Windows, NVDA, JAWS) через UI Automation.
//
// Интерфейс рисуется Direct2D, у него нет системных кнопок, которые диктор прочитал бы сам.
// Поэтому окно отдаёт один элемент-провайдер: его имя - что сейчас в фокусе, а важные события
// (смена трека, элемент под клавиатурным фокусом, сообщения) проговариваются уведомлениями UIA.
#include <windows.h>

#include <string>

namespace ui {

class Narrator {
public:
    ~Narrator();
    void Attach(HWND hwnd) { hwnd_ = hwnd; }
    void Detach();
    // Ответ на WM_GETOBJECT; 0 - не наш запрос
    LRESULT OnGetObject(WPARAM wp, LPARAM lp, bool& handled);
    // Проговорить фразу; имя элемента тоже меняется, чтобы диктор мог перечитать её
    void Announce(const std::wstring& text);

private:
    HWND hwnd_ = nullptr;
    struct Provider;
    Provider* provider_ = nullptr;
};

}  // namespace ui
