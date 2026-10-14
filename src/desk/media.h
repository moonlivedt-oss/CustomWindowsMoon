#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

// Что сейчас играет: системные медиа-сеансы Windows (тот же источник, что у плашки громкости).
// Опрос идёт в своём потоке раз в секунду, окну приходит сообщение, когда что-то поменялось.
namespace media {

// std::mutex в MinGW идёт через winpthreads, со статической сборкой он падает; берём замок Windows
struct Lock {
    SRWLOCK* l;
    explicit Lock(SRWLOCK& s) : l(&s) { AcquireSRWLockExclusive(l); }
    ~Lock() { ReleaseSRWLockExclusive(l); }
};

enum Status { kNone = 0, kStopped = 3, kPlaying = 4, kPaused = 5 };
enum Cmd { CMD_TOGGLE = 1, CMD_NEXT, CMD_PREV, CMD_SEEK };

// Ещё один играющий сеанс (браузер, плеер): чтобы переключаться между ними.
struct Source {
    std::wstring aumid, app;
    int status = kNone;
};

struct Track {
    bool has = false;  // есть ли сеанс вообще
    int status = kNone;
    std::wstring title, artist, app, aumid;
    // время в сотнях наносекунд; updated - момент, к которому относится position (FILETIME)
    int64_t position = 0, end = 0, updated = 0;
    double rate = 1;
    bool canNext = true, canPrev = true, canSeek = true;  // какие кнопки плеер сейчас разрешает
    std::vector<uint8_t> art;                             // обложка как есть (PNG/JPG), пусто - нет
    uint32_t artId = 0;                                   // меняется вместе с обложкой
    uint32_t id = 0;                                      // меняется, когда сменился трек
    std::vector<Source> sources;                          // все сеансы; показанный - тот, у кого aumid совпадает

    // позиция сейчас с учётом того, сколько прошло с последнего обновления
    int64_t Now() const;
};

class Watcher {
public:
    ~Watcher() { Stop(); }
    void Start(HWND notify, UINT msg);
    void Stop();
    void Send(Cmd c, int64_t arg = 0);
    void SetFast(bool fast);                    // false - опрос реже (панель спрятана)
    void SetSource(const std::wstring& aumid);  // показывать этот сеанс, пусто - текущий по мнению Windows
    Track Get();

private:
    static DWORD WINAPI Thread(void* self);
    void Loop();

    HANDLE thread_ = nullptr, wake_ = nullptr;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    volatile bool quit_ = false, fast_ = true;
    SRWLOCK mu_ = SRWLOCK_INIT;
    Track track_;
    std::vector<std::pair<Cmd, int64_t>> cmds_;
    std::wstring chosen_;
};

// Вывести окно программы, которая играет (по AppUserModelId сеанса).
void FocusSource(const std::wstring& aumid);
std::wstring AppName(const std::wstring& aumid);
// Имена программ (без .exe, строчными), которые стоят за сеансом: по окнам с таким AppUserModelId.
std::vector<std::wstring> SourceExes(const std::wstring& aumid);

}  // namespace media
