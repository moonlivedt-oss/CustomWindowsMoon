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
std::wstring HelperExe();  // HelperDir()\RadialMenu-<начало SHA-256 exe>.exe: у каждой сборки свой файл
std::wstring DataDir();    // HelperDir()\data: журнал и бэкапы процесса с правами (он и создаёт папку)

bool TaskExists(Task t);
// Задача есть, запускает помощника, и помощник - копия этого exe (после пересборки его надо обновить).
bool TaskReady(Task t);
// То же без ожидания - для интерфейса: TaskReady запускает schtasks и сравнивает exe (сотни мс, тайм-аут 15 с), а
// в главном потоке висит перехват мыши. Последний известный ответ; устарел - пересчёт в фоне. Пока ответа не было -
// false (дальше RunAdmin в фоне проверит заново и при нужде спросит UAC).
bool TaskReadyCached(Task t);
// Один запрос UAC (системный cmd): скопировать этот exe помощником и (пере)создать задачу t; заодно переводятся
// на помощника и другие уже созданные задачи. Возвращает TaskReady(t).
bool Setup(Task t);
bool Run(Task t);  // schtasks /Run - без запросов
// Запасной путь: то же задание одним запросом UAC, без задачи. Касперский не даёт планировщику поднимать права
// неподписанной программе (задача есть, а запуск отвечает «параметр задан неверно»), а под UAC - даёт.
// consent - задание из ConsentJob: помощник выполнит его, только если запущен так (через окно UAC)
bool RunOnce(Task t, bool consent = false);
// Задачу уже не пустил антивирус: дальше сразу RunOnce (или пропуск), без новой установки после каждой сборки.
// Setup снимает отметку.
bool Blocked(Task t);
void SetBlocked(Task t, bool on);

// SHA-256 своего exe (hex строчными). Считается один раз, при первом вызове - main зовёт его сразу после старта,
// пока файл рядом с работающей программой ещё тот, что запущен (переименовать работающий exe и положить на его место
// другой может любая программа пользователя).
std::wstring SelfHash();
// Помощник в Program Files цел: его SHA-256 совпадает с SelfHash(). Перед каждым запуском задачи.
bool HelperIntact();
// Процесс с правами (--net-job, --purge) запущен из HelperDir() и его содержимое совпадает с именем
// RadialMenu-<хэш>.exe. Нет - задание не выполняется: значит, задачу или exe подменили.
bool SelfTrusted();
// Для процесса с правами, первым делом: окружение - только системное (CreateEnvironmentBlock без токена). Задача
// запускается от имени пользователя и получает его переменные из HKCU\Environment - через них (PATH,
// COR_PROFILER и т.п.) дочерний powershell или netsh с правами загрузил бы чужую DLL.
void SystemEnvironment();
// Процесс с правами: убрать прежние копии помощника рядом с собой (раньше это делала строка для cmd)
void RemoveOldHelpers();
// Задания, которые ломают или выключают что-то заметное (сброс сети, ремонт Windows, перезагрузка в проверку
// Defender, удаление точек восстановления, службы, отрезать программу от сети): запрос в HKCU и запуск задачи
// может подделать любая программа пользователя, поэтому их помощник выполняет только после окна UAC (--consented)
bool ConsentJob(const std::wstring& key);

// --- без системы (elevate_logic.cpp, под тестами) ---
std::wstring HelperName(const std::wstring& sha256);  // RadialMenu-<первые 12 знаков>.exe
// path - файл прямо в папке dir (без подпапок, «..», потоков NTFS и коротких имён 8.3); регистр не важен
bool InsideDir(const std::wstring& path, const std::wstring& dir);
// exePath - помощник этой сборки: лежит в helperDir и называется по своему SHA-256
bool TrustedHelper(const std::wstring& exePath, const std::wstring& helperDir, const std::wstring& sha256);

// Запрос для процесса с правами: строка в HKCU. Take читает и стирает (задание одноразовое).
void Put(const wchar_t* name, const std::wstring& value);
std::wstring Take(const wchar_t* name);

}  // namespace elevate
