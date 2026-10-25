#pragma once
// Общее для частей «Телефона рядом»: сессия (под замком - сервер живёт в своих потоках), сервер, карточка с QR.
#include <atomic>
#include <string>
#include <vector>

#include "desk/toast.h"
#include "phone/phone.h"

namespace phone::detail {

struct Offered {
    std::wstring path, name;
    long long size = 0;
    bool done = false;  // скачан целиком хотя бы раз
};

struct Session {
    SRWLOCK lock = SRWLOCK_INIT;
    bool on = false;
    bool oneTime = false;  // только файлы (Ф1): всё скачано - сессия закрывается
    std::string token;
    std::vector<Offered> files;
    std::wstring clip;  // буфер ПК для телефона (без секретов)
    long long seq = 1;  // растёт при любой перемене, которую видит страница (файлы, буфер)
    ULONGLONG started = 0, seen = 0, allDone = 0;
    int bad = 0;       // запросы с чужим ключом
    int received = 0;  // принято файлов с телефона
    long long receivedBytes = 0;
};
Session& S();

struct Lock {
    Lock() { AcquireSRWLockExclusive(&S().lock); }
    ~Lock() { ReleaseSRWLockExclusive(&S().lock); }
};

// события сервера окну программы (WM_PHONE): wp - что, lp - new std::wstring или 0
enum Event { EV_SEEN = 1, EV_FILE_DONE, EV_RECEIVED, EV_CLIP, EV_BAD, EV_CMD };
void Post(Event e, const std::wstring& text = L"");
// команда пульта (vol+, play, sleep...) в основном потоке; вернёт громкость 0..1 или -1
float Command(const std::string& c);

bool ServerStart(int& port, bool loopback);
void ServerStop();
bool ServerOn();
const char* PageHtml();

std::wstring PhotosDir();     // Изображения\Телефон
std::wstring DownloadsDir();  // Загрузки

// Карточка справа снизу: QR со ссылкой, что передаётся и что с телефоном; «Другая сеть» - пути без общей сети
class PhoneCard : public Toast {
public:
    void Create(HINSTANCE inst);
    struct View {
        std::wstring title, what, status, url, hint;  // hint - брандмауэр, общественная сеть
        bool lan = false, bluetooth = false, pageMode = false, connected = false;
        long long bytes = 0;
    };
    void Show(const View& v, const ThemeInfo& theme);
    void Update(const View& v);  // поменять текст, не въезжая заново
    bool Ways() const { return ways_; }
    std::atomic<bool> stopAsked{false};

private:
    enum { H_A = H_USER, H_B, H_ROW0 = H_USER + 4 };
    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    bool Busy() const override;
    void Closed() override;
    Gdiplus::Color Tone() const override;
    void DrawQr(Gdiplus::Graphics& g, const Gdiplus::RectF& box) const;
    std::vector<Way> Rows() const;

    View v_;
    bool ways_ = false;      // показан список путей вместо QR
    bool hideOnly_ = false;  // страница на связи: уход карточки не закрывает сессию
    std::string qrFor_;
    std::vector<bool> qr_;
    int qrSize_ = 0;
    Gdiplus::RectF a_, b_, rows_[4];
};
PhoneCard& Card();
void RunWay(Way w);  // выбрали путь в карточке
void CopyUrl();
void StopSession(const wchar_t* why);

}  // namespace phone::detail
