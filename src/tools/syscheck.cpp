// «Защита», системная часть: куда обычный пользователь может подложить файл, который потом выполнится с правами
// (папки системного PATH, службы), скрытый автозапуск через подписки WMI и целостность своих exe.
// Только смотрит; права у папок PATH закрывает задача с правами (FixPathAclAdminMain).
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <wbemidl.h>

#include <algorithm>
#include <set>

#include "core/elevate.h"
#include "core/files.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

// Кто считается «обычным пользователем»: Все, Прошедшие проверку, Пользователи, Интерактивные и сам пользователь.
// Через них подложить файл может любая программа без прав.
const std::vector<std::vector<BYTE>>& LowSids() {
    static const std::vector<std::vector<BYTE>> sids = [] {
        std::vector<std::vector<BYTE>> out;
        for (const wchar_t* s : {L"S-1-1-0", L"S-1-5-11", L"S-1-5-32-545", L"S-1-5-4"}) {
            PSID sid = nullptr;
            if (ConvertStringSidToSidW(s, &sid)) {
                out.emplace_back((BYTE*)sid, (BYTE*)sid + GetLengthSid(sid));
                LocalFree(sid);
            }
        }
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            DWORD size = 0;
            GetTokenInformation(tok, TokenUser, nullptr, 0, &size);
            std::vector<BYTE> buf(size);
            if (size && GetTokenInformation(tok, TokenUser, buf.data(), size, &size)) {
                PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
                out.emplace_back((BYTE*)sid, (BYTE*)sid + GetLengthSid(sid));
            }
            CloseHandle(tok);
        }
        return out;
    }();
    return sids;
}

bool IsLow(PSID sid) {
    for (const auto& s : LowSids())
        if (EqualSid(sid, (PSID)s.data())) return true;
    return false;
}

// Может ли обычный пользователь писать в объект: разрешение на запись (или полный доступ) кому-то из LowSids, либо
// объект принадлежит обычному пользователю (владелец сам выдаст себе любые права). Запреты не учитываются -
// в папках программ их почти не бывает, а лишняя тревога лучше пропущенной дыры.
bool LowCanWrite(const std::wstring& path, DWORD writeMask) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS)
        return false;
    bool can = owner && IsLow(owner);
    if (!dacl) can = true;  // пустой DACL - доступ всем
    for (DWORD i = 0; !can && dacl && i < dacl->AceCount; ++i) {
        ACE_HEADER* h = nullptr;
        if (!GetAce(dacl, i, (void**)&h) || h->AceType != ACCESS_ALLOWED_ACE_TYPE || (h->AceFlags & INHERIT_ONLY_ACE))
            continue;
        auto* ace = (ACCESS_ALLOWED_ACE*)h;
        DWORD m = ace->Mask;
        bool write = (m & writeMask) || (m & (GENERIC_WRITE | GENERIC_ALL | WRITE_DAC | WRITE_OWNER));
        if (write && IsLow((PSID)&ace->SidStart)) can = true;
    }
    LocalFree(sd);
    return can;
}

bool IsDirPath(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Parent(const std::wstring& p) {
    size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos || k < 3 ? (p.size() >= 2 ? p.substr(0, 2) + L"\\" : p) : p.substr(0, k);
}

std::wstring Norm(std::wstring p) {
    p = Lower(Trim(p));
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

std::wstring WinDir() {
    wchar_t w[MAX_PATH] = {};
    GetWindowsDirectoryW(w, MAX_PATH);
    return Lower(w);
}

std::wstring RegString(HKEY root, const std::wstring& key, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr,
                     &size) != ERROR_SUCCESS)
        return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, s.data(),
                     &size) != ERROR_SUCCESS)
        return {};
    s.resize(wcslen(s.c_str()));
    return s;
}

DWORD RegDw(HKEY root, const std::wstring& key, const wchar_t* name, DWORD def) {
    DWORD v = def, size = sizeof(v);
    RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

// Системные переменные в пути службы: %SystemRoot%, %ProgramFiles%; «\SystemRoot\» - так пишут драйверы и ядро.
std::wstring ExpandSys(std::wstring s) {
    if (s.rfind(L"\\SystemRoot\\", 0) == 0) s = L"%SystemRoot%" + s.substr(11);
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buf[4096];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    return n && n <= 4096 ? std::wstring(buf) : s;
}

Suspect Finding(FindKind kind, const std::wstring& name, const std::wstring& where, const std::wstring& path, int score,
                const std::wstring& why) {
    Suspect s;
    s.kind = kind;
    s.name = name;
    s.where = where;
    s.path = path;
    s.score = score;
    s.why = why;
    return s;
}

// Папку, которой нет, сможет создать тот, кто может писать в ближайшую существующую папку выше.
bool LowCanCreate(const std::wstring& dir) {
    std::wstring p = dir;
    for (int i = 0; i < 32 && !p.empty(); ++i) {
        std::wstring up = Parent(p);
        if (up == p) break;
        p = up;
        if (IsDirPath(p)) return LowCanWrite(p, FILE_ADD_SUBDIRECTORY);
    }
    return false;
}

}  // namespace

// ---------------- PATH ----------------

std::vector<PathAcl> SystemPathAcl() {
    std::vector<PathAcl> out;
    std::set<std::wstring> seen;
    const std::wstring win = WinDir();
    for (const auto& [raw, full] : SystemPathEntries()) {
        std::wstring n = Norm(full);
        if (n.empty() || n.find(L'%') != std::wstring::npos || !seen.insert(n).second) continue;
        PathAcl a;
        a.raw = raw;
        a.dir = full;
        a.exists = IsDirPath(full);
        // папки самой Windows права задаёт система - не наше дело и не наша починка
        a.windows = n == win || n.rfind(win + L"\\", 0) == 0;
        if (a.windows) continue;
        a.writable = a.exists ? LowCanWrite(full, FILE_ADD_FILE) : LowCanCreate(full);
        if (a.writable) out.push_back(a);
    }
    return out;
}

void CheckPathAcl(std::vector<Suspect>& out) {
    for (const PathAcl& a : SystemPathAcl())
        out.push_back(Finding(
            FIND_PATH, L"PATH: " + a.dir, L"Системный PATH", a.dir, a.exists ? 6 : 5,
            a.exists ? L"в папку из системного PATH может писать обычный пользователь: подложенная туда DLL или exe "
                       L"выполнится в программах и службах с правами администратора"
                     : L"папки из системного PATH нет, а создать её может обычный пользователь - и подложить туда "
                       L"файлы для программ с правами"));
}

// В процессе с правами: каждой папке системного PATH, куда может писать обычный пользователь, - владелец
// «Администраторы», запись обычным пользователям убрана (чтение и запуск остаются). Копия прав - в папке помощника.
// Список считается здесь же, по системному PATH: из запроса пользователя берётся только сам факт задания.
int FixPathAclAdminMain(const Log& log) {
    std::vector<PathAcl> list = SystemPathAcl();
    if (list.empty()) {
        log(L"Папок системного PATH, открытых на запись обычным пользователям, нет.");
        return 0;
    }
    std::wstring backups = elevate::DataDir() + L"\\acl-backups";
    CreateDirectoryW(elevate::DataDir().c_str(), nullptr);
    CreateDirectoryW(backups.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    int fixed = 0;
    for (const PathAcl& a : list) {
        if (!a.exists) {
            log(L"Нет папки " + a.dir + L" - её стоит убрать из PATH («Почистить системный PATH»).");
            continue;
        }
        if (a.dir.size() <= 3) {  // корень диска не трогаем: это сломало бы всё остальное на нём
            log(L"Корень диска в PATH: " + a.dir + L" - уберите его из PATH вручную.");
            continue;
        }
        std::wstring q = L"\"" + a.dir + L"\"", out;
        std::wstring save = L"\"" + backups + L"\\path-" + std::to_wstring(fixed) + L"-" + stamp + L".acl\"";
        RunCapture(L"icacls.exe " + q + L" /save " + save, out, 30000);
        int c1 = RunCapture(L"icacls.exe " + q + L" /setowner *S-1-5-32-544", out, 30000);
        int c2 = RunCapture(L"icacls.exe " + q + L" /inheritance:d", out, 30000);
        int c3 = RunCapture(L"icacls.exe " + q + L" /remove:g *S-1-1-0 *S-1-5-11 *S-1-5-32-545 *S-1-5-4", out, 60000);
        int c4 =
            RunCapture(L"icacls.exe " + q + L" /grant:r *S-1-5-32-545:(OI)(CI)RX *S-1-5-11:(OI)(CI)RX", out, 60000);
        bool ok = c1 == 0 && c2 == 0 && c3 == 0 && c4 == 0;
        log(ok ? L"Закрыта запись в " + a.dir + L" (копия прав - в бэкапах помощника)."
               : L"Не получилось закрыть " + a.dir + L": " + Trim(out));
        fixed += ok;
    }
    return fixed;
}

// ---------------- службы ----------------

void CheckServices(std::vector<Suspect>& out) {
    const wchar_t* root = L"SYSTEM\\CurrentControlSet\\Services";
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    const std::wstring win = WinDir();
    std::set<std::wstring> checkedDirs;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD n = 256;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring key = std::wstring(root) + L"\\" + name;
        DWORD type = RegDw(HKEY_LOCAL_MACHINE, key, L"Type", 0), start = RegDw(HKEY_LOCAL_MACHINE, key, L"Start", 4);
        if (!(type & (SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS)) || start == SERVICE_DISABLED) continue;
        std::wstring image = Trim(RegString(HKEY_LOCAL_MACHINE, key, L"ImagePath"));
        if (image.empty()) continue;
        std::wstring display = RegString(HKEY_LOCAL_MACHINE, key, L"DisplayName");
        if (display.empty() || display[0] == L'@') display = name;
        std::wstring where = L"HKLM\\" + key;

        // какой файл запустится
        std::wstring exe;
        bool quoted = image[0] == L'"';
        if (quoted) {
            size_t e = image.find(L'"', 1);
            exe = image.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
        } else {
            std::wstring l = Lower(image);
            size_t e = l.find(L".exe");
            exe = e != std::wstring::npos ? image.substr(0, e + 4) : image.substr(0, image.find(L' '));
        }
        exe = ExpandSys(exe);
        std::wstring lexe = Lower(exe);
        bool inWindows = lexe.rfind(win + L"\\", 0) == 0;

        // путь с пробелами без кавычек: «C:\Program Files\A B\x.exe» Windows сначала пробует как C:\Program.exe,
        // потом C:\Program Files\A.exe - опасно, если в такое место может писать обычный пользователь
        if (!quoted && exe.find(L' ') != std::wstring::npos && !inWindows) {
            std::wstring risky;
            for (size_t sp = exe.find(L' '); sp != std::wstring::npos; sp = exe.find(L' ', sp + 1)) {
                std::wstring cand = exe.substr(0, sp) + L".exe", dir = Parent(cand);
                if (IsDirPath(dir) && LowCanWrite(dir, FILE_ADD_FILE)) {
                    risky = cand;
                    break;
                }
            }
            out.push_back(Finding(FIND_SVC, display, where, exe, risky.empty() ? 3 : 6,
                                  risky.empty() ? L"путь службы с пробелами без кавычек (сейчас подложить некуда, "
                                                  L"но так пишут небрежные установщики)"
                                                : L"путь службы без кавычек: Windows сначала попробует запустить " +
                                                      risky + L", а туда может писать обычный пользователь"));
        }
        if (inWindows) continue;  // системные службы проверяет сама Windows

        // сам файл или его папка открыты на запись: подменить exe или подложить DLL рядом
        std::wstring dir = Parent(exe);
        bool fileW = GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES && LowCanWrite(exe, FILE_WRITE_DATA);
        bool dirW = IsDirPath(dir) && (checkedDirs.count(Lower(dir)) || LowCanWrite(dir, FILE_ADD_FILE));
        if (dirW) checkedDirs.insert(Lower(dir));
        if (fileW || dirW)
            out.push_back(Finding(FIND_SVC, display, where, exe, 6,
                                  fileW ? L"файл службы может изменить обычный пользователь - служба выполнит его "
                                          L"с правами системы"
                                        : L"в папку службы может писать обычный пользователь: подложенная DLL "
                                          L"загрузится в службу с правами системы"));
        else if (lexe.rfind(L"c:\\users\\", 0) == 0 || lexe.find(L"\\appdata\\") != std::wstring::npos ||
                 lexe.find(L"\\temp\\") != std::wstring::npos)
            out.push_back(Finding(FIND_SVC, display, where, exe, 5,
                                  L"служба запускается из папки пользователя - так делают вредоносы"));
    }
    RegCloseKey(k);
}

// ---------------- WMI ----------------

// Подписки WMI: «при событии X выполнить команду или скрипт». Так закрепляются вредоносы без файлов в автозапуске.
// Безвредные потребители (журнал событий, файл журнала) не показываются - только запуск команд и скриптов.
void CheckWmi(std::vector<Suspect>& out) {
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc)))
        return;
    IWbemServices* svc = nullptr;
    BSTR ns = SysAllocString(L"ROOT\\subscription");
    if (SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) {
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        for (const wchar_t* cls : {L"CommandLineEventConsumer", L"ActiveScriptEventConsumer"}) {
            BSTR lang = SysAllocString(L"WQL");
            BSTR q = SysAllocString((std::wstring(L"SELECT * FROM ") + cls).c_str());
            IEnumWbemClassObject* en = nullptr;
            if (SUCCEEDED(
                    svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en))) {
                IWbemClassObject* o = nullptr;
                ULONG got = 0;
                while (en->Next(5000, 1, &o, &got) == S_OK && got) {
                    auto str = [&](const wchar_t* prop) {
                        VARIANT v;
                        VariantInit(&v);
                        std::wstring s;
                        if (SUCCEEDED(o->Get(prop, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
                            s = v.bstrVal;
                        VariantClear(&v);
                        return s;
                    };
                    bool script = cls[0] == L'A';
                    std::wstring name = str(L"Name"),
                                 what = script ? str(L"ScriptFileName") : str(L"CommandLineTemplate");
                    if (what.empty()) what = script ? str(L"ScriptText").substr(0, 160) : str(L"ExecutablePath");
                    out.push_back(Finding(FIND_WMI, L"WMI: " + name, std::wstring(L"root\\subscription, ") + cls, what,
                                          6,
                                          script ? L"подписка WMI выполняет скрипт по событию - скрытый автозапуск без "
                                                   L"файла в автозагрузке"
                                                 : L"подписка WMI запускает команду по событию - скрытый автозапуск; "
                                                   L"если ставили её не вы, это почти наверняка вредонос"));
                    o->Release();
                }
                en->Release();
            }
            SysFreeString(q);
            SysFreeString(lang);
        }
        svc->Release();
    }
    SysFreeString(ns);
    loc->Release();
}

// ---------------- целостность своих exe ----------------

namespace {

std::wstring Sha256Text(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::wstring out;
    BYTE hash[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 && BCryptFinishHash(h, hash, 32, 0) == 0) {
        wchar_t hx[3];
        for (BYTE b : hash) swprintf(hx, 3, L"%02x", b), out += hx;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

}  // namespace

// Файл-отметка, которую пишет сборка (cmake/integrity.cmake): %APPDATA%\MoonDesk\integrity\<sha256 пути>.txt,
// внутри - SHA-256 exe сразу после сборки. Путь в нижнем регистре только по латинице - так же считает CMake.
std::wstring IntegrityRecord(const std::wstring& exePath) {
    std::wstring p = exePath;
    for (wchar_t& c : p)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\integrity", ad, MAX_PATH);
    return std::wstring(ad) + L"\\" + Sha256Text(Narrow(p)) + L".txt";
}

void CheckSelf(std::vector<Suspect>& out) {
    for (const wchar_t* exe : {L"RadialMenu.exe", L"RadialTools.exe"}) {
        std::wstring path = ExeDir() + L"\\" + exe;
        HANDLE f = CreateFileW(IntegrityRecord(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;  // собрано без отметки - сравнивать не с чем
        char buf[128] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(f);
        std::wstring want = Lower(Trim(Widen(std::string(buf, got))));
        std::wstring have = FileSha256(path);
        if (want.size() != 64 || have.empty() || have == want) continue;
        out.push_back(
            Finding(FIND_SELF, exe, L"папка программы", path, 6,
                    L"файл изменился после сборки: его подменили или испортили (вирус, который дописывает "
                    L"себя в чужие exe, так и выглядит). Пересоберите проект и проверьте компьютер антивирусом"));
    }
}

}  // namespace tools
