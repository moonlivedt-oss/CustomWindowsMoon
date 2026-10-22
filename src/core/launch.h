#pragma once
#include <string>

#include <windows.h>

struct Config;

// Запуск того, что выбрали в меню.
std::wstring FindVsCode(const Config* c);                  // Code.exe: из настроек, из стандартных мест или code.cmd
void OpenTerminal(const Config* c, std::wstring dir);      // Windows Terminal, PowerShell или cmd в папке
void OpenCodeAt(const Config* c, const std::wstring& at);  // VS Code на месте «файл:строка:столбец» (code -g)
void UnlockForeground(HWND self);                          // чтобы открытое окно не оказалось позади
