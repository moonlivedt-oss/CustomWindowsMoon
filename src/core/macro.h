#pragma once
// Пункт кольца «нажать сочетание» и короткие макросы (SendInput).
//   keys:ctrl+shift+p                              - одно сочетание
//   macro:ctrl+l; wait 300; text github.com; enter - цепочка шагов через «;»
// Шаг макроса: сочетание (ctrl+shift+p, win+shift+s, f5, enter), «wait N» - пауза в мс (до 10 с), «text ...» -
// набрать текст как есть (любая раскладка: идёт символами Юникода). Разбор отдельно от отправки - под тестами.
#include <string>
#include <vector>

#include <windows.h>

namespace macro {

struct Step {
    enum Kind { KEYS, WAIT, TEXT } kind = KEYS;
    std::vector<WORD> vk;  // KEYS: клавиши по порядку нажатия (модификаторы первыми)
    int ms = 0;            // WAIT
    std::wstring text;     // TEXT
};

bool Is(const std::wstring& path);  // keys: или macro:
// Разбор значения keys:... или macro:...; пустой список - ничего не понятно (такой пункт ничего не делает).
std::vector<Step> Parse(const std::wstring& path);
UINT TokenVk(const std::wstring& token);          // «ctrl», «f5», «a», «`» -> виртуальная клавиша, 0 - не знаю
std::wstring KeyName(UINT vk);                    // обратно, для записи сочетания в студии
std::wstring Describe(const std::wstring& path);  // для карточки: «Ctrl + Shift + P», «макрос: 3 шага»
// Отправить в фоне: сперва дождаться, пока отпустят Alt, Ctrl, Shift и Win (до 1,5 с; иначе Ctrl+Shift+P
// превратится в Alt+Ctrl+Shift+P), потом шаги по очереди окну, которое сейчас впереди.
void Run(const std::wstring& path);

}  // namespace macro
