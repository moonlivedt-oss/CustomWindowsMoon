#pragma once
// «Продолжить, где остановился» (Л4): что было открыто - папки проектов в VS Code, папки Проводника, проект Unreal,
// программы с окнами - запоминается (раз в 10 минут, пока вы за компьютером, и при блокировке, сне и выключении) в
// %APPDATA%\MoonDesk\lastsession.tsv. После перерыва (включили компьютер, разблокировали или разбудили через час и
// больше) - карточка «вчера в 23:40 были открыты ... - открыть всё?»: открывается только то, чего сейчас нет.
// resume_logic.cpp - снимок строками, разбор VS Code, разница и текст (под тестами); resume.cpp - сбор и карточка.
#include <windows.h>

#include <string>
#include <vector>

struct Config;

namespace resume {

inline constexpr UINT WM_RESUME = WM_APP + 74;  // итог проверки после перерыва (wp - когда, lp - чего нет)

struct Thing {
    enum Kind { CODE, FOLDER, UNREAL, APP } kind = APP;
    std::wstring path;   // папка, .uproject или exe
    std::wstring label;  // как показать: имя папки, проекта, программы
};
struct Snapshot {
    LONGLONG when = 0;  // секунды unix
    std::vector<Thing> things;
};

// --- без окон и файлов (под тестами) ---
std::wstring Save(const Snapshot& s);
bool Parse(const std::wstring& text, Snapshot& s);
std::wstring FromFileUri(const std::wstring& uri);  // file:///d%3A/Web/C%2B%2B -> D:\Web\C++
// Открытые папки VS Code из storage.json: backupWorkspaces.folders (окна, что открыты сейчас) и папки из заголовков
// окон («файл - имя - Visual Studio Code»), найденные среди известных VS Code папок (profileAssociations)
std::vector<std::wstring> CodeFolders(const std::wstring& storageJson, const std::vector<std::wstring>& titles);
std::vector<Thing> Missing(const Snapshot& was, const std::vector<Thing>& now);  // чего из прошлого сейчас нет
// «VS Code: RadialMenu, image; папки: Загрузки; Unreal: MyGame; программы: Blender» (до maxLen знаков)
std::wstring Describe(const std::vector<Thing>& things, size_t maxLen = 220);
std::wstring When(LONGLONG then, LONGLONG now);  // «сегодня в 14:20», «вчера в 23:40», «20.10 в 18:05»

// --- в программе ---
void Init(HINSTANCE inst, HWND app, const Config* cfg);
void Tick();               // из общего опроса: снимок раз в 10 минут, карточка после перерыва
void OnLock();             // блокировка, сон, выключение - запомнить сейчас
void OnUnlock();           // разблокировали или разбудили - если прошёл час, спросить
void SaveNow();            // запомнить сейчас (Л39 «ухожу»)
std::vector<Thing> Now();  // что открыто сейчас
void OpenAll(const std::vector<Thing>& things);
void OnMessage(WPARAM wp, LPARAM lp);  // WM_RESUME: карточка «Продолжить, где остановились?»
void Shutdown();

}  // namespace resume
