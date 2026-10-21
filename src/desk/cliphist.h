#pragma once
// История буфера и буфер без секретов: последние 30 текстов и картинок (только в памяти программы; картинки -
// превью во временной папке, которая чистится при выходе и запуске). Кольцо «Буфер» (действие угла clip и пункт
// кольца action:clip) и строки «Буфер: ...» в командной палитре: выбрали - запись снова в буфере и вставлена
// (Ctrl+V) туда, что впереди. Токены, ключи, пароли и номера карт (core/secrets) в историю не идут и через
// clip_secret_sec секунд стираются из буфера, если их никто не заменил. Что программы помечают как «не для
// истории» (менеджеры паролей: ExcludeClipboardContentFromMonitorProcessing) - не запоминается.
#include <string>
#include <vector>

#include <windows.h>

struct Config;
struct Item;
class RadialMenu;

namespace cliphist {

void Start(HINSTANCE inst, const Config* cfg);
void Stop();
std::vector<Item> Items(const std::wstring& tag);  // записи для кольца и палитры: special 3, ключ tag + номер
void Use(int n);                                   // положить запись n в буфер и вставить
void Clear();                                      // забыть всё, кроме закреплённого
void TogglePin(int n);                             // закрепить / открепить запись n (на диске, DPAPI)
void ShowRing(RadialMenu& menu);
std::wstring Status();  // «запомнено 12, стёрто секретов 1»

}  // namespace cliphist
