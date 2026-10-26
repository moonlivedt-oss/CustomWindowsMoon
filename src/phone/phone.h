#pragma once
// Телефон рядом (Ф1, Ф4-Ф6): свой маленький HTTP-сервер на время передачи и страница для телефона по QR.
// Бросили файлы на сектор action:phone - карточка с QR, телефон скачивает их по Wi-Fi прямо с ПК (без облака и
// кабеля); ссылка одноразовая: всё скачано - через минуту сервер закрывается. Действие phone - та же страница без
// файлов: буфер ПК и телефона (Ф4), фото и видео с телефона в «Изображения\Телефон\дата» (Ф5), пульт - громкость,
// пауза, следующий, выключить экран, усыпить (Ф6). Путь выбирается сам: есть общая сеть - QR по Wi-Fi; нет -
// карточка подскажет раздать Wi-Fi с телефона или ноутбука (QR обновится сам, как только появится адрес), мелкое -
// Bluetooth (fsquirt). Ссылка - http://адрес:порт/<ключ>/: без ключа сервер ничего не отдаёт, после 20 неверных
// ключей сессия закрывается. Секреты из буфера (core/secrets) на телефон не уходят.
// phone_logic.cpp - разбор запросов, имена, адреса, выбор пути (под тестами); server.cpp - сокеты; page.cpp -
// страница; phone.cpp - сессия, карточка и действия.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct Config;

namespace phone {

inline constexpr UINT WM_PHONE = WM_APP + 76;  // событие сервера (wp - что, lp - строка в куче или 0)

// --- в программе ---
void Init(HINSTANCE inst, HWND app, const Config* cfg);
void Tick();  // из общего опроса: буфер ПК для телефона, конец сессии, новые адреса
void Shutdown();
void Offer(const std::vector<std::wstring>& files);  // Ф1: эти файлы - на телефон
void OpenPage();                                     // Ф4-Ф6: страница телефона без файлов (действие phone)
void Stop();
bool Active();
std::wstring Status();     // «Телефон: на связи, принято 5» / пусто
std::wstring Url();        // ссылка для телефона (пусто - нет сессии или общей сети)
std::wstring SecureUrl();  // та же страница по https (пусто - https не открылся)
std::wstring Diag();
int Devices();  // сколько телефонов запомнено
void Forget();  // забыть все запомненные телефоны
// действия для углов и кольца: найти телефон (страница звенит), на телефон - то, что в буфере ПК (файлы
// Проводника, ссылка или текст)
void Find();
void SendClipboard();
void SendFiles(const std::vector<std::wstring>& files);  // «Отправить - На телефон» в Проводнике
// запуск пункта кольца с телефона: main делает его тем же путём, что и щелчок в кольце
void SetRunner(void (*run)(const std::wstring& action));
void EnsureSendTo();  // ярлык «Телефон (RadialMenu)» в меню «Отправить» Проводника, если его нет
// вторая копия RadialMenu.exe --to-phone <файлы>: передать файлы работающей копии (её окно - app)
bool PostToRunning(HWND app, const std::vector<std::wstring>& files);  // адаптеры, адрес для телефона, профиль сети,
                                                                       // Bluetooth, брандмауэр (--phone-diag)
void OnMessage(WPARAM wp, LPARAM lp);

// --- без сокетов и окон (под тестами) ---
struct Request {
    std::string method, path;                    // путь без запроса, %XX раскрыты
    std::map<std::string, std::string> query;    // значения раскрыты (UTF-8)
    std::map<std::string, std::string> headers;  // имена строчными
    long long length = 0;                        // Content-Length (0 - нет)
};
bool ParseHead(const std::string& head, Request& r);  // до пустой строки; false - не HTTP
std::string UrlDecode(const std::string& s);          // %XX и + как пробел (в запросе)
std::string UrlEncode(const std::string& s);          // для filename*=UTF-8''...
// имя файла с телефона: без папок, запретных знаков и имён устройств (CON, NUL...), не длиннее 120 знаков
std::wstring SafeName(const std::wstring& name);
std::wstring DatedDir(const std::wstring& base, long long unixMs);  // base\2026-10-25 по местному времени
bool IsMedia(const std::wstring& name);                             // фото или видео с камеры
std::string ContentType(const std::wstring& name);
// Range: bytes=a-b / a- / -n; false - нет заголовка или не понять (отдать целиком)
bool ParseRange(const std::string& value, long long size, long long& from, long long& to);
std::string Json(const std::wstring& s);                  // строка JSON в кавычках
std::string Token(const unsigned char* bytes, size_t n);  // base64url без =

struct Adapter {
    std::wstring name;
    uint32_t ip = 0;    // IPv4 в порядке хоста (192.168.1.5 = 0xC0A80105)
    int type = 0;       // IF_TYPE_*: 71 - Wi-Fi, 6 - Ethernet
    bool virt = false;  // VPN, TUN, виртуальные машины, WSL - телефон через них не дойдёт
    std::wstring guid;  // {GUID} адаптера - найти его сеть и её профиль
};
bool PrivateIp(uint32_t ip);
bool HotspotIp(uint32_t ip);  // 192.168.137.x - «Мобильный хот-спот» Windows раздаёт этот адрес себе
// лучший адрес для телефона: раздача с ноутбука, потом Wi-Fi, потом провод; 0 - общей сети нет
uint32_t PickAddress(const std::vector<Adapter>& list);
std::vector<Adapter> LocalAdapters();  // включённые адаптеры с IPv4 (phone.cpp)
std::wstring IpText(uint32_t ip);

// --- второй заход (phone_more_logic.cpp, под тестами) ---
uint32_t Crc32(uint32_t crc, const void* data, size_t n);  // продолжить crc (начало - 0)
struct ZipEntry {
    std::string name;  // UTF-8, через /
    uint32_t size = 0, crc = 0, offset = 0;
    uint16_t dosTime = 0, dosDate = 0;
};
std::string ZipLocal(const std::string& name, uint32_t size, uint16_t dosTime, uint16_t dosDate);
std::string ZipDescriptor(uint32_t crc, uint32_t size);
std::string ZipCentral(const ZipEntry& e);
std::string ZipEnd(size_t count, uint32_t dirSize, uint32_t dirOffset);
long long ZipSize(const std::vector<std::pair<std::string, long long>>& files);  // точная длина архива
bool ZipFits(const std::vector<std::pair<std::string, long long>>& files);       // без ZIP64: до 4 ГБ и 65535 файлов
std::string ZipName(const std::wstring& rel);                                    // «Папка\a.jpg» -> «Папка/a.jpg»
// путь внутри папки ПК из адреса: «Фильмы/2026/a.mkv» -> «Фильмы\2026\a.mkv»; «?» - нельзя (.., :, \, управляющие)
std::wstring SafeRel(const std::string& utf8);
bool UploadId(const std::string& id);  // 8-40 знаков [A-Za-z0-9_-]
// «Отдаю 38,2 МБ/с, ещё 20 с»; пусто - стоим
std::wstring RateText(const wchar_t* verb, double bytesPerSec, long long left);

// --- пульт (phone_remote_logic.cpp, под тестами) ---
POINT TapPoint(double nx, double ny, const RECT& monitor);  // касание в долях картинки -> точка экрана
SIZE FrameSize(int w, int h, int maxW);                     // кадр для телефона: не шире maxW (320..2560)
std::vector<WORD> Combo(const std::string& name);           // «copy» -> Ctrl+C; пусто - нет такого
bool FsOp(const std::string& op);                           // open, copy, move, rename, mkdir, delete
WORD KeyVk(const std::string& name);  // «enter», «left», «a», «7», «f5», «shift» -> код клавиши; 0 - нет такой

// --- «Поделиться - на ПК» (phone_share_logic.cpp, под тестами) ---
std::string Boundary(const std::string& contentType);  // из «multipart/form-data; boundary=...»; пусто - не то
struct FormPart {
    std::string name, filename, type;  // filename - как прислали (UTF-8), чистит SafeName; type - строчными
    bool file = false;                 // у части было filename
};
// multipart/form-data потоком: тело кусками любого размера, данные части отдаются по мере прихода (большой файл не
// держится в памяти). Обработчик вернул false - разбор прерван
class Multipart {
public:
    explicit Multipart(const std::string& boundary);
    std::function<bool(const FormPart&)> onBegin;
    std::function<bool(const char*, size_t)> onData;
    std::function<bool()> onEnd;
    bool Feed(const char* p, size_t n);  // false - тело испорчено или прервали
    bool Done() const { return state_ == DONE; }

private:
    enum State { PREAMBLE, AFTER, HEADERS, BODY, DONE, BAD } state_ = PREAMBLE;
    std::string delim_, buf_;
};
// ссылка из «Поделиться»: поле url или первая http(s)-ссылка в тексте (без хвостовой точки и скобки); пусто - нет
std::string ShareLink(const std::string& url, const std::string& text);

// Пути, если телефон не в этой сети: по порядку - что предложить первым
enum Way { WAY_PHONE_HOTSPOT, WAY_PC_HOTSPOT, WAY_BLUETOOTH };
std::vector<Way> Ways(bool lan, bool bluetooth, long long bytes);
inline constexpr long long kBluetoothMax = 20ll << 20;  // крупнее по Bluetooth идёт слишком долго

}  // namespace phone
