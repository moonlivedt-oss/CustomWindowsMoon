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
    bool Machine() const;  // есть значения в HKLM - нужны права
};

const std::vector<Setting>& Catalog();
const Setting* Find(const std::wstring& id);
std::vector<std::wstring> Groups();  // в порядке каталога

enum class State { Off, On, Partial };
struct Status {
    State state = State::Off;
    bool ours = false;  // хотя бы одно значение меняли мы (есть в бэкапе)
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

// ---------------- реестр ----------------
bool IsAdmin();
Status StatusOf(const Setting& s, const Backup& user, const Backup& machine);
using Log = std::function<void(const std::wstring&)>;
// Включить (priv) или вернуть как было. HKLM без прав не трогается - false и пояснение в log.
bool Apply(const Setting& s, bool priv, const Log& log);

// ---------------- профиль ----------------
// Профиль - список id включённых, по одному в строке; # - комментарий. Чужие id пропускаются.
std::wstring EncodeProfile(const std::vector<std::wstring>& ids);
std::vector<std::wstring> DecodeProfile(const std::wstring& text);

// Точка восстановления системы (нужны права и включённая защита системы). false - с пояснением в why.
bool CreateRestorePoint(const std::wstring& name, std::wstring& why);

}  // namespace shutup
