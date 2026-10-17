#pragma once
#include <string>

// Действия с правами администратора без повторных запросов UAC: задачи планировщика «с наивысшими правами».
// Задача запускает не RadialMenu.exe из папки пользователя (его и DLL рядом с ним может подменить любая
// программа пользователя - и получить права молча), а копию-помощника в Program Files\MoonRadial, куда пишут
// только администраторы. Запрос передаётся строкой в HKCU\Software\MoonRadial, журнал и бэкапы процесс с
// правами пишет в Program Files\MoonRadial\data: в папках пользователя он ничего не создаёт, не пишет и не
// удаляет (ссылка, подложенная туда, не перенаправит его запись).
namespace elevate {

enum Task { TASK_NET, TASK_PURGE };  // задачи «Сети» (--net-job) и очистки памяти (--purge)

std::wstring HelperDir();  // Program Files\MoonRadial - путь из реестра системы, не из переменных окружения
std::wstring HelperExe();  // HelperDir()\RadialMenu-<хэш exe>.exe: у каждой сборки свой файл
std::wstring DataDir();    // HelperDir()\data: журнал и бэкапы процесса с правами (он и создаёт папку)

bool TaskExists(Task t);
// Задача есть, запускает помощника, и помощник - копия этого exe (после пересборки его надо обновить).
bool TaskReady(Task t);
// Один запрос UAC (системный cmd): скопировать этот exe помощником и (пере)создать задачу t; заодно переводятся
// на помощника и другие уже созданные задачи. Возвращает TaskReady(t).
bool Setup(Task t);
bool Run(Task t);  // schtasks /Run - без запросов
// Запасной путь: то же задание одним запросом UAC, без задачи. Касперский не даёт планировщику поднимать права
// неподписанной программе (задача есть, а запуск отвечает «параметр задан неверно»), а под UAC - даёт.
bool RunOnce(Task t);
// Задачу уже не пустил антивирус: дальше сразу RunOnce (или пропуск), без новой установки после каждой сборки.
// Setup снимает отметку.
bool Blocked(Task t);
void SetBlocked(Task t, bool on);

// Запрос для процесса с правами: строка в HKCU. Take читает и стирает (задание одноразовое).
void Put(const wchar_t* name, const std::wstring& value);
std::wstring Take(const wchar_t* name);

}  // namespace elevate
