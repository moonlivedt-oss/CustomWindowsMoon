#pragma once
// Рабочие наборы (radial.cfg, [@sets]): «Открыть» запускает всё из набора, следит 10 с, какие окна появились, и
// раскладывает их (desk/zones); «Закрыть» вежливо (WM_CLOSE, как крестиком) закрывает именно эти окна - браузер
// и редактор спросят про несохранённое сами. Кольцо «Наборы» (действие угла sets, пункт action:sets), строки
// «Набор:» и «Закрыть набор:» в палитре.
#include <string>
#include <vector>

#include <windows.h>

struct Config;
struct Item;
class RadialMenu;

namespace worksets {

void Init(HWND app, const Config* cfg);
void Open(int set);
void Close(int set);
void Run(const std::wstring& key);  // open:N / close:N
std::vector<Item> Items(const std::wstring& tag);
void ShowRing(RadialMenu& menu);
int OpenedCount(int set);  // сколько окон набора сейчас открыто (0 - не открыт или уже закрыли)
// что откроет пункт: «VS Code», «терминал», «ссылка», «программа»...
std::wstring KindOf(const std::wstring& path);

}  // namespace worksets
