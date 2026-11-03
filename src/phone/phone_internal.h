#pragma once
// Общее для частей «Телефона рядом»: сессия (под замком - сервер живёт в своих потоках), сервер, карточка с QR.
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "desk/toast.h"
#include "phone/phone.h"

namespace phone::detail {

struct Offered {
    std::wstring path, name;  // name - путь внутри брошенной папки («Отпуск\a.jpg») или имя файла
    long long size = 0;
    bool done = false;  // скачан целиком хотя бы раз
};

// Загрузка с телефона кусками (докачка): id придумывает страница, кусок - PUT /up?id=&off=&total=
struct Incoming {
    std::wstring tmp, dir, name;
    long long total = 0, have = 0, mt = 0;
    bool busy = false;  // кусок пишется сейчас (параллельные куски одного файла не принимаем)
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
    int received = 0;  // принято файлов с телефона
    long long receivedBytes = 0;
    std::map<std::string, Incoming> incoming;
    std::string clipPng;               // картинка из буфера ПК (PNG) для телефона
    long long clipImg = 0;             // её номер (0 - картинки нет)
    bool standing = false;             // запомненный телефон: сервер ждёт постоянно, без карточки и QR
    std::vector<std::string> devices;  // ключи запомненных телефонов (/d/<ключ>/)
    std::map<std::string, DeviceRec> deviceAccess;
    std::string caDer;                 // свой корневой сертификат (отдаётся телефону как MoonDesk-CA.crt)
    int tlsPort = 0;                   // порт https (0 - только http)
    int findSeq = 0;                   // «найти телефон»: растёт - страница звенит
    std::vector<std::wstring> links;   // ссылки с ПК для телефона («На телефон»)
    // управление с телефона (ControlPath): кому разрешено на ПК
    std::vector<std::string> ctlKeys;    // запомненные телефоны с разрешением (из файла)
    std::vector<uint32_t> tokenCtlIps;   // телефонам по QR (адреса) этой сессии разрешили
    std::vector<std::string> ctlDenied;  // «s» (по QR) или «d:<ключ>»: на ПК нажали «Запретить» - до конца сессии
    std::string ctlAsk;                  // о ком сейчас спрашивает карточка
    ULONGLONG ctlAskAt = 0;
    // «Запомнить этот телефон» ждёт ответа на ПК: 0 - не спрашиваем, 1 - ждём, 2 - разрешили, 3 - нет
    int pairState = 0;
    std::vector<uint32_t> tokenIps;   // адреса, которым открыт ключ сессии (TokenIpAllowed)
    std::vector<uint32_t> tokenDeny;  // на ПК сказали «Нет» - до конца сессии
    uint32_t ipAsk = 0;               // о каком адресе сейчас спрашивает карточка
    ULONGLONG ipAskAt = 0;
    ULONGLONG pairAt = 0;
};
Session& S();

struct Lock {
    Lock() { AcquireSRWLockExclusive(&S().lock); }
    ~Lock() { ReleaseSRWLockExclusive(&S().lock); }
};

// события сервера окну программы (WM_PHONE): wp - что, lp - new std::wstring или 0
enum Event {
    EV_SEEN = 1,
    EV_FILE_DONE,
    EV_RECEIVED,
    EV_CLIP,
    EV_BAD,
    EV_CMD,
    EV_OPEN,
    EV_SCAN,
    EV_CLIPIMG,
    EV_PAIRED,
    EV_OCR,
    EV_NOTE,      // запись в журнал (что сделано с телефона)
    EV_CALL,      // CallMain: lp - CallReq на стеке потока сервера (SendMessage)
    EV_CAM,       // пришёл кадр камеры телефона
    EV_OUTBOX,    // вторая копия положила файлы в phone-outbox.txt («Отправить - На телефон»)
    EV_NOTICE,    // приложение на телефоне: уведомление, SMS, звонок (lp - new Notice)
    EV_ASK,       // телефон впервые берёт управление: спросить на ПК (lp - new wstring: «s» или «d:<ключ>»)
    EV_PAIR_ASK,  // телефон просит постоянный ключ: спросить на ПК (lp - new wstring: имя телефона)
    EV_IP_ASK     // ключ сессии с нового адреса: спросить на ПК (lp - адрес)
};
void Post(Event e, const std::wstring& text = L"");
void PostBytes(Event e, std::string bytes);  // lp - new std::string (картинка)
void PostNotice(const Notice& n);            // EV_NOTICE: карточка на ПК
void PostIpAsk(uint32_t ip);                 // EV_IP_ASK: пустить ли второй телефон
// счётчики для скорости на карточке
extern std::atomic<long long> g_sent, g_recv, g_sendLeft;
extern std::atomic<ULONGLONG> g_screenAt;  // когда телефон последний раз брал кадр экрана
// вызов в основном потоке (окна, открыть файл): op - «wins», «win:front:<окно>», «open:<путь UTF-8>»
bool CallMain(const std::string& op, std::string& out);
// команда пульта (vol+, play, sleep...) в основном потоке; вернёт громкость 0..1 или -1
float Command(const std::string& c);

bool ServerStart();  // поток приёма; слушать пока нигде
// слушать только на ip (порядок хоста; 0 - нигде): порты прежние, если свободны. port, tlsPort - на чём слушает
// (0 - нигде, tlsPort 0 - https не открылся); false - порты 8780-8799 заняты
bool ServerBind(uint32_t ip, int& port, int& tlsPort);
void ServerStop();
bool ServerOn();
const char* PageHtml();

std::wstring PhotosDir();                         // Изображения\Телефон
std::wstring DownloadsDir();                      // Загрузки
std::vector<std::wstring> ShareRoots();           // папки ПК, открытые телефону (phone_share)
bool InputAllowed();                              // phone_input: тачпад и клавиатура с телефона
bool ScreenAllowed();                             // phone_screen и не остановлено на ПК
bool FilesAllowed();                              // phone_files: открыть, копировать, переместить, в Корзину
std::string NewDevice(const std::wstring& name);  // запомнить телефон: ключ (пусто - не вышло)
void DeviceSeen(const std::string& key);          // запомненный телефон заходил
// управление с телефона (ключ запомненного; пусто - по QR, тогда по адресу): 1 - разрешено, 0 - спросили на ПК,
// ждём, -1 - запрещено
int ControlCheck(const std::string& devKey, uint32_t ip);
// То же без вопроса (для /poll: страница показывает, что мешает): -1 запрещено, 0 не спрашивали, 1 можно, 2 ждём ответа
int ControlState(const std::string& devKey, uint32_t ip);
extern std::atomic<ULONGLONG> g_ctlAt;  // когда телефон последний раз управлял (тачпад, клавиатура, кольцо)

// Соединение (сервер держит его открытым между запросами): что уже пришло и не разобрано, сколько тела осталось
struct Conn {
    std::shared_ptr<std::atomic<bool>> revoked = std::make_shared<std::atomic<bool>>(false);
    UINT_PTR c = ~(UINT_PTR)0;  // SOCKET (winsock2.h сюда не тянем: его надо включать раньше windows.h)
    std::string in;
    long long bodyLeft = 0;
    bool close = false;
    bool tls = false;            // пришло на порт https
    uint32_t ip = 0;             // адрес телефона (IPv4 в порядке хоста)
    int Read(char* buf, int n);  // тело текущего запроса: сначала из in, потом из сокета; 0 - всё, -1 - обрыв
};
bool SendAll(UINT_PTR c, const char* p, size_t n);
void ServerRevoke(const std::string& key);  // закрыть уже активные сокеты этого телефона
bool AutomaticTransferPaused();
void PaceTransfer(size_t bytes);
int NetRecv(UINT_PTR c, char* buf, int n);  // recv или расшифровка, если соединение по https

// HTTPS (tls.cpp): Schannel поверх сокета; сертификат - phone/cert
struct Tls;
bool TlsSetCert(PCCERT_CONTEXT c);  // новые учётные данные сервера (адрес поменялся)
bool TlsReady();
void TlsReset();
Tls* TlsAccept(UINT_PTR s);  // рукопожатие; nullptr - не вышло
int TlsRecv(Tls* t, char* buf, int n);
bool TlsSend(Tls* t, const char* p, size_t n);
void TlsClose(Tls* t);
std::string Head(Conn& cn, int code, const std::string& type, long long length, const std::string& extra = "");
void Reply(Conn& cn, int code, const std::string& type, const std::string& body, const std::string& extra = "");
void Text(Conn& cn, int code, const std::string& body);
void JsonReply(Conn& cn, const std::string& body);
bool ReadAllBody(Conn& cn, const Request& r, std::string& out, long long max);
void Seen();
std::atomic<bool>& Stopping();
// обработчики (server_files.cpp, server_more.cpp); rest - путь после ключа
bool HandleFiles(Conn& cn, const Request& r, const std::string& rest);
bool HandleMore(Conn& cn, const Request& r, const std::string& rest);
bool HandleRemote(Conn& cn, const Request& r, const std::string& rest);
bool HandleShare(Conn& cn, const Request& r, const std::string& rest);  // server_share.cpp: https, PWA, камера
bool HandleExtra(Conn& cn, const Request& r, const std::string& rest);  // server_extra.cpp: кольцо, музыка, ПК
bool HandleApp(Conn& cn, const Request& r, const std::string& rest);    // server_app.cpp: приложение на телефоне
void HoldKey(WORD vk, bool down);                                       // server_more.cpp: геймпад
void ReleaseKeys();
void MediaStart();  // что играет на ПК (desk/media) - пока идёт передача
void MediaStop();
std::wstring UniquePath(const std::wstring& dir, const std::wstring& name);  // «a (2).jpg», если занято
void MarkFromInternet(const std::wstring& path);                             // Zone.Identifier
const char* ServiceWorkerJs();                                               // web/ (cmake/embed.cmake)
const char* ManifestJson();
// Картинки страницы (web/img: шаги настройки, пустые вкладки) - тоже из exe; nullptr - такой нет
const unsigned char* WebImage(const std::string& name, size_t* len);
// камера телефона (camera.cpp): кадр JPEG из потока сервера; false - окно камеры на ПК закрыли (start - открыть снова)
bool CamFrame(std::string jpg, bool start);
void CamShow();  // в основном потоке (EV_CAM): окно «Камера телефона» и новый кадр в нём
void CamStop();
// путь в открытой папке ПК по параметрам rk (номер папки) и pk (путь внутри); пусто - нельзя
std::wstring SharePathOf(const Request& r, const char* rk, const char* pk);
bool ShareRootIs(const std::wstring& path);  // это сама открытая папка
// отдать файл с Range; inline - смотреть в браузере (видео, музыка), а не скачивать
bool SendFile(Conn& cn, const Request& r, const std::wstring& path, const std::wstring& name, bool inlineView,
              bool* complete = nullptr);

// Карточка справа снизу: QR со ссылкой, что передаётся и что с телефоном; «Другая сеть» - пути без общей сети
class PhoneCard : public Toast {
public:
    void Create(HINSTANCE inst);
    struct View {
        std::wstring title, what, status, url, hint;  // hint - брандмауэр, общественная сеть
        std::wstring fp;  // отпечаток корневого сертификата: сверить с приложением или Android при знакомстве
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
    // без общей сети: почему (общественная сеть - hint) и что делать
    std::wstring NoLanText() const {
        return v_.hint.empty() ? L"Общей сети с телефоном нет - выберите путь:" : v_.hint + L" Или выберите путь:";
    }

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
