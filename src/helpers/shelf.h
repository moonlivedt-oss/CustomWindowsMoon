#pragma once
// Полка у края экрана (Ф7): набросать файлы из разных папок, потом одним броском перенести все или отправить на
// телефон. Файлы попадают на полку броском на саму полку или на пункт кольца action:shelf; действие shelf - показать
// или спрятать. Полка - узкая панель у правого края рабочей области: список файлов, сверху - «Тащить все» (потянуть
// мышью в любую папку: Проводник перенесёт, с Ctrl - скопирует), «На телефон» (Ф1) и «Очистить». Щелчок по файлу -
// убрать его с полки (сам файл не трогается). На полке только ссылки на файлы, список живёт до выхода или очистки.
// shelf_logic.cpp - список (под тестами).
#include <windows.h>

#include <string>
#include <vector>

struct Config;

namespace shelf {

void Init(HINSTANCE inst, const Config* cfg);
void Toggle();
void Add(const std::vector<std::wstring>& files);  // добавить и показать
void Shutdown();
inline void Tick() {}

// --- чистое ---
// добавить без повторов (регистр не важен), не больше max; вернёт, сколько добавилось
int Merge(std::vector<std::wstring>& shelf, const std::vector<std::wstring>& add, size_t max = 200);
std::wstring Title(const std::vector<std::wstring>& shelf);  // «Полка: 3 файла»

}  // namespace shelf
