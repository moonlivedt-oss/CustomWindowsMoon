#pragma once
// Окно разметки снимка: стрелка, рамка, маркер, подпись, размытие. Открывается щелчком по превью в карточке снимка
// (helpers/snip). Всё мышью; клавиши только привычные: Ctrl+Z, Ctrl+Y, Ctrl+C, Ctrl+S, Esc.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

struct UiColors;

namespace markup {

enum class Finish { Apply, ApplyAndSave };
// Что вернуть карточке: готовая картинка (нет правок - не зовётся) и закрытие окна в любом случае
struct Callbacks {
    std::function<void(std::unique_ptr<Gdiplus::Bitmap>, Finish)> done;
    std::function<void()> closed;
};

void Open(const UiColors& colors, const std::vector<uint8_t>& png, Callbacks callbacks);
bool IsOpen();
void Close();  // без вопросов и без применения: выход из программы

// Для разработки: кадр окна с показательными фигурами и итоговая картинка
int DemoSnap(const UiColors& colors, const std::wstring& frameFile, const std::wstring& flatFile, int imageW = 1000,
             int imageH = 600);
// Для разработки: скрытое окно на картинке из файла, перевод снимка настоящим движком; кадр окна и итог
int DemoTranslate(const UiColors& colors, const std::wstring& imageFile, const std::wstring& frameFile, const std::wstring& flatFile);
// Для разработки: настоящее окно на показательной картинке; итог - в flatFile, выход - по закрытии окна
int DemoWindow(const UiColors& colors, const std::wstring& flatFile);

}  // namespace markup
