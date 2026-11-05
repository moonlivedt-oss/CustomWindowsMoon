#pragma once
// Кольцо выделенного: выделили текст в любой программе, Ctrl+Alt+Пробел (selection_ring) или действие угла
// «Кольцо выделенного» - кольцо того, что с ним сделать: найти, перевести, исправить раскладку, регистр, посчитать,
// открыть ссылку или путь, QR. Текст берётся копированием: буфер обмена сохраняется целиком и возвращается, в
// историю буфера служебное копирование не попадает. Ничего не выделено - берётся текст из буфера.
#include <windows.h>

#include <string>
#include <vector>

struct Config;
struct Item;
class RadialMenu;

namespace selring {

void Start(HINSTANCE inst, const Config* cfg, RadialMenu* menu);
void Trigger();  // взять выделенное и показать кольцо (из горячей клавиши и угла)
void Stop();
// пункты кольца для текста (и для снимка без мыши: --sel-snap)
std::vector<Item> RingItems(const std::wstring& text, bool fromClipboard);

// ---- чистое (selring_logic.cpp, под тестами) ----
// «ghbdtn» -> «привет», «руддщ» -> «hello»: набрано не в той раскладке. Направление - по тому, каких букв больше.
std::wstring FixLayout(const std::wstring& s);
bool HasCyrillic(const std::wstring& s);
// Выражение: + - * / % ^ скобки, числа с точкой или запятой, × ÷. false - не выражение (или просто число).
bool Calc(const std::wstring& s, double& out);
std::wstring FormatNumber(double v, bool comma);  // до 10 значащих цифр, без хвостовых нулей
std::wstring Sentence(const std::wstring& s);     // «пРИВЕТ МИР. ЕЩЁ» -> «Привет мир. Ещё»
std::wstring Squeeze(const std::wstring& s);      // лишние пробелы и пустые строки прочь, края обрезаны
std::wstring UrlEncode(const std::wstring& s);    // UTF-8 с %XX, пробел - %20
int Words(const std::wstring& s);

}  // namespace selring
