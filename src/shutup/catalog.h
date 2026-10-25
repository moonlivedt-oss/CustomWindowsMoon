#pragma once
// RadialShutUp: каталог настроек приватности Windows в духе O&O ShutUp10. Каждая настройка - известные значения
// реестра; прежние (или «значения не было») уходят в бэкап до первой правки, «как было» возвращает их из бэкапа.
// Своё (HKCU) пишется без прав, для всего компьютера (HKLM) - только из процесса с правами администратора.
#include <windows.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace shutup {

enum class Level { Recommended, Limited, NotRecommended };  // как у ShutUp10: зелёный, жёлтый, красный

struct RegValue {
    HKEY root;
    const wchar_t* key;
    const wchar_t* name;
    DWORD type;           // REG_DWORD или REG_SZ
    DWORD num;            // значение «приватно» для REG_DWORD
    const wchar_t* text;  // значение «приватно» для REG_SZ
};

struct Setting {
    const wchar_t* id;
    const wchar_t* group;
    const wchar_t* title;
    const wchar_t* why;
    Level level;
    std::vector<RegValue> values;
    // задачи планировщика телеметрии (путь «\\Microsoft\\Windows\\...»): «включено» - задача выключена
    std::vector<const wchar_t*> tasks = {};
    // домены, закрытые строкой «0.0.0.0 домен # RadialShutUp» в hosts
    std::vector<const wchar_t*> hosts = {};
    bool Machine() const;  // для всего компьютера: HKLM, задачи, hosts
    bool User() const;     // есть значения в HKCU - пишутся в профиль того, от чьего имени запущено
    bool Admin() const;    // нужны права: для компьютера или политики в HKCU\Software\Policies (их пишет только админ)
};

// что нужно, чтобы правка заработала: значения Проводника он перечитывает при перезапуске, службы - при загрузке
enum class Restart { None, Explorer, Reboot };
Restart RestartOf(const Setting& s);

const std::vector<Setting>& Catalog();
// «Подробнее»: что делает настройка и чем платите, человеческим языком (details.cpp); nullptr - текста нет
const wchar_t* Details(const std::wstring& id);
const Setting* Find(const std::wstring& id);
std::vector<std::wstring> Groups();  // в порядке каталога

enum class State { Off, On, Partial };
struct Status {
    State state = State::Off;
    bool ours = false;  // хотя бы одно значение меняли мы (есть в бэкапе)
    // меняли мы, а сейчас не включено: вернуло обновление Windows, Параметры или другая программа
    bool Drifted() const { return ours && state != State::On; }
};

// ---------------- бэкап ----------------
// Прежние значения лежат в реестре рядом с изменёнными: <корень>\Software\RadialShutUp\Backup, имя значения -
// «ключ|имя», данные - «было (1/0)<TAB>тип<TAB>значение». Бэкап для HKLM может писать только администратор -
// обычный пользователь не подсунет в него своё, что потом «вернётся» с правами.
struct Saved {
    bool had = false;
    DWORD type = REG_DWORD;
    std::wstring value;  // число десятичной строкой, текст или байты шестнадцатеричной строкой
};
using Backup = std::map<std::wstring, Saved>;  // ключ - «ключ|имя»
std::wstring BackupKey(const RegValue& v);
std::wstring EncodeSaved(const Saved& s);
bool DecodeSaved(const std::wstring& text, Saved& out);
Backup LoadBackup(HKEY root);
// Старый бэкап вкладки «Приватность» студии (файл «корень<TAB>ключ<TAB>имя<TAB>было<TAB>число», до общего каталога):
// записи этого корня, которых ещё нет, - в бэкап в реестре; файл переименовывается в .imported. Сколько перенесено.
int ImportLegacy(HKEY root, const std::wstring& file);

// ---------------- реестр ----------------
bool IsAdmin();
Status StatusOf(const Setting& s, const Backup& user, const Backup& machine);
// текущее значение в виде записи бэкапа; false - значения нет
bool ReadValue(const RegValue& v, Saved& out);
using Log = std::function<void(const std::wstring&)>;
// Включить (priv) или вернуть как было. HKLM без прав не трогается - false и пояснение в log.
bool Apply(const Setting& s, bool priv, const Log& log);
// откуда правка - для журнала: «окно», «профиль», «без окна», «RadialMenu»...
void SetSource(const std::wstring& source);

// ---------------- журнал ----------------
// %APPDATA%\RadialShutUp\journal.tsv: время, откуда, id, включено (1) или как было (0), получилось ли
struct JournalEntry {
    std::wstring when, source, id;
    bool priv = false, ok = false;
};
std::wstring EncodeJournal(const JournalEntry& e);
bool DecodeJournal(const std::wstring& line, JournalEntry& out);
std::vector<JournalEntry> ReadJournal(size_t max);  // свежие первыми

// ---------------- готовые профили ----------------
// 0 - мягкий (рекомендуемое), 1 - строгий (+ с оговорками), 2 - игровой (мягкий + меньше фона во время игры)
constexpr int kBuiltinProfiles = 3;
const wchar_t* BuiltinName(int which);
std::vector<std::wstring> BuiltinProfile(int which);
// страница Параметров Windows для настройки (ms-settings:...); nullptr - своей страницы нет (details.cpp)
const wchar_t* SettingsUri(const std::wstring& id);

// задачи планировщика и hosts - для проверки и тестов
int TaskEnabled(const wchar_t* path);  // 1 - включена, 0 - выключена, -1 - нет такой
std::string HostsText();               // содержимое hosts как есть
// строки hosts: закрыт ли домен (любой строкой 0.0.0.0/127.0.0.1 домен), добавить свои, убрать свои
bool HostsBlocks(const std::string& hosts, const std::string& domain);
std::string HostsAdd(const std::string& hosts, const std::vector<std::string>& domains);
std::string HostsRemove(const std::string& hosts, const std::vector<std::string>& domains);
// разослать окнам «политики и настройки поменялись» - часть значений Проводник и Параметры подхватят без перезапуска
void NotifyChanged(bool wait = false);

// ---------------- профиль ----------------
// Профиль - список id включённых, по одному в строке; # - комментарий. Чужие id пропускаются.
std::wstring EncodeProfile(const std::vector<std::wstring>& ids);
std::vector<std::wstring> DecodeProfile(const std::wstring& text);

// Точка восстановления системы (нужны права и включённая защита системы). false - с пояснением в why.
bool CreateRestorePoint(const std::wstring& name, std::wstring& why);

}  // namespace shutup
