#pragma once
#include <map>
#include <string>
#include <vector>

#include <windows.h>

// Boost во время игры и работы в Unreal: всё, что не закрывает программы, а перераспределяет силы.
// - Игре высокий приоритет, фоновым окнам - режим экономии (EcoQoS: Windows держит их на экономных ядрах
//   и низкой частоте). «Усыпить» программу: её память уходит в подкачку, процессор - в экономию.
//   Всё изменённое записывается в %APPDATA%\MoonDesk\gameboost.ini: упадёт программа - при запуске вернётся.
// - Тишина: уведомления Windows молчат, пока идёт игра.
// - Слежка в своём потоке раз в 5 секунд: кэш ожидания (как ISLC) - чистка через задачу Purge, видеопамять,
//   на какой видеокарте игра, компиляция шейдеров Unreal, итог сборки UBT. Новости - сообщением окну.
// - Сессия: сколько длилась, пик памяти, рост подкачки, тормоза из-за подкачки (жёсткие ошибки страниц).
namespace gboost {

inline constexpr UINT WM_GBOOST = WM_APP + 69;  // wParam - Event, lParam - Note* (удалить получателю)

enum Event {
    EV_GPU_WRONG = 1,  // игра на встроенной видеокарте: text - путь exe, sub - имена видеокарт
    EV_VRAM_HIGH,      // видеопамять почти кончилась: a - занято МБ, b - всего МБ
    EV_SHADERS_START,  // a - рабочих
    EV_SHADERS_DONE,   // a - секунд, b - больше всего рабочих
    EV_BUILD_DONE,     // a - секунд, b - 1 успех / 0 ошибка, text - первая ошибка, sub - цель сборки
    EV_PURGED,         // кэш ожидания почищен: a - сколько стало свободно МБ
};

struct Note {
    Event ev;
    double a = 0, b = 0;
    std::wstring text, sub;
    std::wstring go;  // EV_BUILD_DONE с ошибкой: «файл:строка:столбец» для VS Code (code -g); пусто - места нет
};

// ---------------- процессы ----------------

// Игре HIGH, окнам в фоне - EcoQoS. keepFast - кого не трогать (маски имён без .exe), guarded - защищённые Boost.
// Возвращает, сколько программ ушло в экономию.
int GameFirst(const std::vector<DWORD>& game, const std::vector<std::wstring>& keepFast,
              const std::vector<std::wstring>& guarded);
// Только фон в экономию (компиляция шейдеров): кроме exclude по имени.
int BackgroundEco(const std::vector<std::wstring>& exclude, const std::vector<std::wstring>& guarded);
// «Усыпить»: память в подкачку, EcoQoS, приоритет ниже обычного. Возвращает, сколько МБ ушло из памяти.
double Soften(const std::vector<DWORD>& pids, wchar_t tag = L's');  // L'f' - усыплённые фокусом
void Restore(wchar_t tag);  // только свои: L'g' игра и фон, L's' усыплённые, L'f' фокусом, L'c' компиляция
void RestoreAll();          // вернуть приоритеты и экономию всем, кого трогали
int Tamed();                // сколько процессов сейчас изменено

// ---------------- тишина ----------------
// Уведомления Windows: «только будильники». Тишину держат несколько режимов сразу (L'g' игра, L'f' фокус,
// L'p' показ экрана) - вернутся уведомления, только когда отпустят все. Quiet(false) - снять тишину совсем.
void QuietFor(wchar_t who, bool on);
void Quiet(bool on);
bool QuietOn();

void Recover();  // при запуске: вернуть то, что осталось от упавшего прошлого раза

// ---------------- видеокарта ----------------
struct Adapter {
    LUID luid{};
    std::wstring name;
    double vramMb = 0;
    bool integrated = false;
};
std::vector<Adapter> Adapters();
bool PreferDiscrete(const std::wstring& exe);  // «Высокая производительность» для exe (HKCU, без прав)

// ---------------- профили ----------------
// Что делать с программой перед игрой или редактором: 0 - закрыть, 1 - усыпить, 2 - оставить.
enum Act { ACT_CLOSE = 0, ACT_SLEEP = 1, ACT_KEEP = 2 };
std::map<std::wstring, int> Profile(const std::wstring& app);  // ключ - имя программы в нижнем регистре
void SaveProfile(const std::wstring& app, const std::map<std::wstring, int>& acts);

// ---------------- слежка и сессия ----------------
struct Session {
    std::wstring name;  // игра или редактор
    bool game = true;
    ULONGLONG ms = 0;                    // длительность
    double peakUsedMb = 0, totalMb = 0;  // пик занятой памяти и сколько её всего
    double peakCommitMb = 0, limitMb = 0;
    double pageGrowMb = 0;  // насколько выросла подкачка
    double maxFaults = 0;   // пик чтений из подкачки, страниц/с
    int stallSamples = 0;   // замеров, где подкачка мешала (больше 800 страниц/с)
    int samples = 0, purges = 0;
    double peakVramMb = 0, vramMb = 0;
};

void Start(HWND notify);  // поток слежки
void Stop();
// Режимы от главного окна: процессы игры, путь exe игры (для проверки видеокарты), имя для сессии.
void SetGame(bool on, const std::vector<DWORD>& pids, const std::wstring& exe, const std::wstring& name, bool islc);
void SetDev(bool on, const std::wstring& name);
Session TakeSession(bool game);      // закончить сессию и забрать итог
void SaveSession(const Session& s);  // строка в boost_sessions.tsv
std::vector<Session> History(size_t max);

// ---------------- Unreal ----------------
// Список для исключений антивируса: папки движка, проектов, кэша и процессы сборки - текстом для людей.
std::wstring UnrealExclusions(const std::wstring& editorExe);

}  // namespace gboost
