#pragma once
// Следы падения: необработанное исключение -> минидамп (dbghelp) и строка «что и где» рядом, в
// %APPDATA%\MoonDesk\crash, не больше 5 дампов. При следующем запуске программа видит новые дампы и говорит о них
// (журнал автоматики и карточка). Exe собран без символов (-s): адрес «модуль+смещение» в .txt разбирается сборкой
// с -DRADIAL_STRIP=OFF той же версии.
#include <windows.h>

#include <string>
#include <vector>

namespace crash {

constexpr int kKeep = 5;  // столько дампов храним

std::wstring Dir();  // %APPDATA%\MoonDesk\crash (создаётся)
// Поставить обработчик как можно раньше (первая строка wWinMain); dir - куда класть (пусто - Dir()).
void Install(const std::wstring& dir = L"");

struct Report {
    std::wstring dump;  // полный путь .dmp
    std::wstring when;  // «18.10 14:05»
    std::wstring what;  // «C0000005 (обращение к памяти) в RadialMenu.exe+0x1a2b3», из .txt; пусто - не записалось
};
// Дампы, о которых ещё не говорили (новее отметки reported в той же папке), старые первыми; mark - сдвинуть отметку.
std::vector<Report> Unreported(const std::wstring& dir, bool mark);
std::vector<Report> All(const std::wstring& dir);  // все дампы папки, старые первыми
// Перезапуск после падения: процесс жил дольше минуты и прошлый перезапуск был больше 10 минут назад - после дампа
// запускается новая копия с ключом --after-crash (она дождётся, пока старая отпустит мьютекс). Командная строка
// готовится заранее: в обработчике падения кучу не трогаем.
void SetRestart(bool on);
// Оставить keep самых новых дампов (и их .txt).
void Prune(const std::wstring& dir, int keep = kKeep);

// Разбор адреса (Н10): сборка пишет карту символов (nm) в %APPDATA%\MoonDesk\integrity\maps\<exe>-<метка>.map
// (cmake/symbols.cmake), строка падения несёт метку сборки. ParseWhere достаёт из «... в RadialMenu.exe+0x1a2b3,
// сборка 68F9A3C6» модуль, смещение и метку; Symbolize - «функция+0xNN» по тексту карты; Decode - всё вместе
// (пусто - карты этой сборки нет). mapsDir пусто - своя папка.
bool ParseWhere(const std::wstring& what, std::wstring& module, ULONGLONG& offset, DWORD& stamp);
std::wstring Symbolize(const std::string& map, ULONGLONG addr);
std::wstring Decode(const std::wstring& what, const std::wstring& mapsDir = L"");

}  // namespace crash
