// Защита без своего движка: подписи запущенных программ и автозапуска, откуда они запущены, подмена системных
// имён, перехваты Winlogon и запуска программ в реестре. Только смотрит; завершение - через RadialTools.exe.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mscat.h>
#include <softpub.h>
#include <taskschd.h>
#include <tlhelp32.h>
#include <wintrust.h>

#include <algorithm>
#include <map>
#include <set>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[4096];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    return n && n <= 4096 ? std::wstring(buf) : s;
}

std::wstring Dir(const std::wstring& p) {
    size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos ? L"" : p.substr(0, k);
}

std::wstring LeafOf(const std::wstring& p) { return p.substr(p.find_last_of(L'\\') + 1); }

bool Under(const std::wstring& lowerPath, const std::wstring& lowerDir) {
    return !lowerDir.empty() && lowerPath.size() > lowerDir.size() && lowerPath.rfind(lowerDir + L"\\", 0) == 0;
}

// ---------------- подписи ----------------

struct SignCache {
    ULONGLONG stamp = 0, size = 0;
    SignState state = SIGN_NOFILE;
    std::wstring signer;
};
SRWLOCK g_signLock = SRWLOCK_INIT;
std::map<std::wstring, SignCache> g_signs;  // путь в нижнем регистре -> последняя проверка

std::wstring SignerName(HANDLE state) {
    CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(state);
    CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
    if (!sg || !sg->pasCertChain || !sg->csCertChain || !sg->pasCertChain[0].pCert) return L"";
    wchar_t name[256] = L"";
    CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
    return name;
}

SignState FromResult(LONG rc) {
    switch ((DWORD)rc) {
        case ERROR_SUCCESS:
            return SIGN_OK;
        case (DWORD)TRUST_E_NOSIGNATURE:
        case (DWORD)TRUST_E_SUBJECT_FORM_UNKNOWN:
        case (DWORD)TRUST_E_PROVIDER_UNKNOWN:
            return SIGN_NONE;
        case (DWORD)TRUST_E_BAD_DIGEST:
            return SIGN_BAD;  // файл изменён после подписи
        default:
            return SIGN_UNTRUSTED;  // самоподписанная, отозванная, просроченная без метки времени...
    }
}

// WinVerifyTrust без сети: отзыв сертификатов не проверяется (иначе секунды на файл и запросы наружу).
LONG Verify(WINTRUST_DATA& wd, std::wstring* signer) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    LONG rc = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (signer && wd.hWVTStateData) *signer = SignerName(wd.hWVTStateData);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return rc;
}

// Системные файлы Windows подписаны не внутри, а в каталогах (*.cat): ищем хэш файла по каталогам.
SignState CatalogSign(const std::wstring& path, std::wstring* signer) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return SIGN_NONE;
    SignState out = SIGN_NONE;
    for (const wchar_t* alg :
         {BCRYPT_SHA256_ALGORITHM, (const wchar_t*)nullptr}) {  // новые каталоги SHA256, старые SHA1
        HCATADMIN admin = nullptr;
        if (!CryptCATAdminAcquireContext2(&admin, nullptr, alg, nullptr, 0)) continue;
        BYTE hash[64];
        DWORD hlen = sizeof(hash);
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        if (CryptCATAdminCalcHashFromFileHandle2(admin, f, &hlen, hash, 0)) {
            HCATINFO cat = CryptCATAdminEnumCatalogFromHash(admin, hash, hlen, 0, nullptr);
            if (cat) {
                CATALOG_INFO ci{sizeof(ci)};
                if (CryptCATCatalogInfoFromContext(cat, &ci, 0)) {
                    std::wstring tag;
                    wchar_t hx[3];
                    for (DWORD i = 0; i < hlen; ++i) swprintf(hx, 3, L"%02X", hash[i]), tag += hx;
                    WINTRUST_CATALOG_INFO wc{sizeof(wc)};
                    wc.pcwszCatalogFilePath = ci.wszCatalogFile;
                    wc.pcwszMemberTag = tag.c_str();
                    wc.pcwszMemberFilePath = path.c_str();
                    wc.hMemberFile = f;
                    wc.pbCalculatedFileHash = hash;
                    wc.cbCalculatedFileHash = hlen;
                    wc.hCatAdmin = admin;
                    WINTRUST_DATA wd{};
                    wd.dwUnionChoice = WTD_CHOICE_CATALOG;
                    wd.pCatalog = &wc;
                    out = FromResult(Verify(wd, signer));
                }
                CryptCATAdminReleaseCatalogContext(admin, cat, 0);
            }
        }
        CryptCATAdminReleaseContext(admin, 0);
        if (out != SIGN_NONE) break;
    }
    CloseHandle(f);
    return out;
}

// ---------------- системные имена ----------------

// Расстояние правки не больше одного (замена, вставка, удаление, перестановка соседних): «scvhost», «svch0st».
bool OneEdit(const std::wstring& a, const std::wstring& b) {
    if (a == b) return false;
    size_t la = a.size(), lb = b.size();
    if (la == lb) {
        std::vector<size_t> diff;
        for (size_t i = 0; i < la && diff.size() < 3; ++i)
            if (a[i] != b[i]) diff.push_back(i);
        if (diff.size() == 1) return true;
        return diff.size() == 2 && diff[1] == diff[0] + 1 && a[diff[0]] == b[diff[1]] && a[diff[1]] == b[diff[0]];
    }
    if (la + 1 != lb && lb + 1 != la) return false;
    const std::wstring& s = la < lb ? a : b;
    const std::wstring& l = la < lb ? b : a;
    size_t i = 0;
    while (i < s.size() && s[i] == l[i]) ++i;
    return s.compare(i, std::wstring::npos, l, i + 1, std::wstring::npos) == 0;
}

std::wstring LookAlike(const std::wstring& lowerLeaf) {
    if (!EndsWithI(lowerLeaf, L".exe")) return L"";
    std::wstring base = lowerLeaf.substr(0, lowerLeaf.size() - 4);
    if (base.size() < 5) return L"";
    for (const std::wstring& n : SystemExeNames()) {
        std::wstring sys = n.substr(0, n.size() - 4);
        if (sys.size() >= 5 && OneEdit(base, sys)) return n;
    }
    return L"";
}

// «отчёт.pdf.exe»: документ, который на деле программа
bool DoubleExt(const std::wstring& lowerLeaf) {
    static const wchar_t* kDoc[] = {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".jpg", L".jpeg", L".png",
                                    L".txt", L".mp4", L".mp3",  L".zip", L".rar",  L".avi", L".mkv",  L".pptx"};
    static const wchar_t* kExe[] = {L".exe", L".scr", L".com", L".bat", L".cmd", L".pif"};
    for (const wchar_t* e : kExe) {
        if (!EndsWithI(lowerLeaf, e)) continue;
        std::wstring rest = lowerLeaf.substr(0, lowerLeaf.size() - wcslen(e));
        for (const wchar_t* d : kDoc)
            if (EndsWithI(rest, d)) return true;
    }
    return false;
}

// Длинная форма пути в нижнем регистре: %TEMP% бывает коротким (C:\Users\MOONLI~1\...), а путь процесса - длинным.
std::wstring LongLower(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetLongPathNameW(p.c_str(), buf, MAX_PATH * 2);
    return Lower(n && n < MAX_PATH * 2 ? std::wstring(buf, n) : p);
}

// Откуда запуск подозрителен для программы без подписи. Пусто - место обычное.
std::wstring RiskyPlace(const std::wstring& lowerPath) {
    static const std::wstring temp = LongLower(Expand(L"%TEMP%")), wtemp = LongLower(Expand(L"%SystemRoot%\\Temp")),
                              roaming = LongLower(Expand(L"%APPDATA%")), local = LongLower(Expand(L"%LOCALAPPDATA%")),
                              pdata = LongLower(Expand(L"%ProgramData%")), pub = LongLower(Expand(L"%PUBLIC%")),
                              down = LongLower(DownloadsDir());
    std::wstring d = Dir(lowerPath);
    if (Under(lowerPath, temp) || Under(lowerPath, wtemp)) return L"запущен из временных файлов";
    if (Under(lowerPath, down)) return L"запущен из Загрузок";
    if (Under(lowerPath, pub)) return L"запущен из общей папки Public";
    if (lowerPath.find(L"\\$recycle.bin\\") != std::wstring::npos) return L"запущен из Корзины";
    // прямо в корне AppData или ProgramData программы не живут - только в своих подпапках
    if (d == roaming || d == local || d == pdata) return L"лежит прямо в корне " + LeafOf(d);
    return L"";
}

bool InAppData(const std::wstring& lowerPath) {
    static const std::wstring roaming = LongLower(Expand(L"%APPDATA%")), local = LongLower(Expand(L"%LOCALAPPDATA%"));
    return Under(lowerPath, roaming) || Under(lowerPath, local);
}

void Flag(Suspect& s, int score, const std::wstring& why) {
    s.score += score;
    s.why += (s.why.empty() ? L"" : L"; ") + why;
}

// Общая оценка файла: подпись, место, двойное расширение, похожесть на системное имя.
void JudgeFile(Suspect& s) {
    std::wstring lp = LongLower(s.path), leaf = LeafOf(lp);
    if (GetFileAttributesW(s.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        s.sign = SIGN_NOFILE;
        Flag(s, s.pid ? 3 : 1, s.pid ? L"файла на диске уже нет" : L"файла нет");
        return;
    }
    s.sign = CheckSign(s.path, &s.signer);
    bool trusted = s.sign == SIGN_OK;
    bool microsoft = trusted && s.signer.find(L"Microsoft") != std::wstring::npos;
    if (SystemImposter(lp) && !microsoft) Flag(s, 6, L"имя системного " + leaf + L", но лежит не в папке Windows");
    if (DoubleExt(leaf)) Flag(s, 4, L"двойное расширение - притворяется документом");
    if (s.sign == SIGN_BAD) Flag(s, 5, L"подпись повреждена: файл изменён после подписи");
    if (trusted) return;  // подписанное доверенным издателем дальше по месту не судим
    // приложения Магазина подписаны целиком пакетом, а в WindowsApps пишет только система
    static const std::wstring store = LongLower(Expand(L"%ProgramFiles%\\WindowsApps"));
    if (s.sign == SIGN_NONE && Under(lp, store)) return;
    std::wstring like = LookAlike(leaf);
    if (!like.empty()) Flag(s, 4, L"имя похоже на системное " + like);
    if (s.sign == SIGN_UNTRUSTED)
        Flag(s, 2, L"подпись недоверенная" + (s.signer.empty() ? std::wstring() : L" («" + s.signer + L"»)"));
    else if (s.sign == SIGN_NONE)
        Flag(s, 1, L"без цифровой подписи");
    std::wstring place = RiskyPlace(lp);
    if (!place.empty())
        Flag(s, 2, place);
    else if (InAppData(lp))
        Flag(s, 1, L"из AppData");
    if (EndsWithI(lp, L".scr")) Flag(s, 2, L"заставка .scr вне папки Windows");
    DWORD a = GetFileAttributesW(s.path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
        Flag(s, 1, L"файл скрыт");
}

// ---------------- процессы ----------------

std::set<DWORD> NetPids() {  // у кого есть установленные соединения наружу (не на свой компьютер)
    std::set<DWORD> out;
    for (ULONG fam : {AF_INET, AF_INET6}) {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, fam, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        std::vector<BYTE> buf(size + 4096);
        size = (DWORD)buf.size();
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, fam, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR)
            continue;
        if (fam == AF_INET) {
            auto* t = (MIB_TCPTABLE_OWNER_PID*)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCPROW_OWNER_PID& r = t->table[i];
                if (r.dwState == MIB_TCP_STATE_ESTAB && (r.dwRemoteAddr & 0xFF) != 127) out.insert(r.dwOwningPid);
            }
        } else {
            auto* t = (MIB_TCP6TABLE_OWNER_PID*)buf.data();
            static const UCHAR kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCP6ROW_OWNER_PID& r = t->table[i];
                if (r.dwState == MIB_TCP_STATE_ESTAB && memcmp(r.ucRemoteAddr, kLoop, 16) != 0)
                    out.insert(r.dwOwningPid);
            }
        }
    }
    return out;
}

BOOL CALLBACK CollectWindowPid(HWND w, LPARAM lp) {
    if (!IsWindowVisible(w)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    ((std::set<DWORD>*)lp)->insert(pid);
    return TRUE;
}

// ---------------- реестр ----------------

std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* name, REGSAM view = 0) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(
            root, key, name,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : 0),
            nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name, REGSAM view = 0) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : 0), nullptr,
                     &v, &size) != ERROR_SUCCESS)
        return 0;
    return v;
}

Suspect RegFinding(const std::wstring& name, const std::wstring& where, const std::wstring& value, int score,
                   const std::wstring& why) {
    Suspect s;
    s.kind = FIND_REG;
    s.name = name;
    s.where = where;
    s.path = value;
    s.score = score;
    s.why = why;
    return s;
}

constexpr wchar_t kWinlogon[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
constexpr wchar_t kIfeo[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

void CheckRegistry(std::vector<Suspect>& out) {
    // Shell: кто становится рабочим столом после входа. Должен быть explorer.exe.
    std::wstring shell = Trim(RegStr(HKEY_LOCAL_MACHINE, kWinlogon, L"Shell"));
    if (!shell.empty() && Lower(shell) != L"explorer.exe")
        out.push_back(RegFinding(L"Shell", L"HKLM\\" + std::wstring(kWinlogon), shell, 6,
                                 L"вместо Проводника после входа запускается " + shell));
    std::wstring ushell = Trim(RegStr(HKEY_CURRENT_USER, kWinlogon, L"Shell"));
    if (!ushell.empty() && Lower(ushell) != L"explorer.exe")
        out.push_back(RegFinding(L"Shell (пользователь)", L"HKCU\\" + std::wstring(kWinlogon), ushell, 6,
                                 L"для этого пользователя вместо Проводника запускается " + ushell));
    // Userinit: только userinit.exe (с запятой на конце - так задумано)
    std::wstring ui = RegStr(HKEY_LOCAL_MACHINE, kWinlogon, L"Userinit");
    std::wstring sysInit = Lower(Expand(L"%SystemRoot%\\system32\\userinit.exe"));
    for (const std::wstring& part : Split(ui, L',')) {
        std::wstring p = Lower(Trim(part));
        if (p.empty() || p == sysInit || p == L"userinit.exe" || Lower(Expand(p)) == sysInit) continue;
        out.push_back(RegFinding(L"Userinit", L"HKLM\\" + std::wstring(kWinlogon), ui, 6,
                                 L"при входе вместе с userinit запускается " + Trim(part)));
        break;
    }
    // IFEO Debugger: при запуске программы вместо неё стартует другая
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIfeo, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
        wchar_t sub[260];
        for (DWORD i = 0;; ++i) {
            DWORD n = 260;
            if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring dbg =
                Trim(RegStr(HKEY_LOCAL_MACHINE, (std::wstring(kIfeo) + L"\\" + sub).c_str(), L"Debugger", view));
            if (dbg.empty()) continue;
            bool dup = false;
            for (const Suspect& s : out) dup |= s.name == sub && s.path == dbg;
            if (dup) continue;
            // свой отладчик вместо Диспетчера задач (System Informer, Process Explorer) - обычное дело
            std::wstring l = Lower(dbg);
            bool known = Lower(sub) == L"taskmgr.exe" &&
                         (l.find(L"systeminformer") != std::wstring::npos || l.find(L"procexp") != std::wstring::npos ||
                          l.find(L"processhacker") != std::wstring::npos);
            out.push_back(RegFinding(sub, L"HKLM\\" + std::wstring(kIfeo) + L"\\" + sub, dbg, known ? 1 : 5,
                                     L"при запуске " + std::wstring(sub) + L" вместо него стартует " + dbg));
        }
        RegCloseKey(k);
    }
    // AppInit_DLLs: библиотеки, которые грузятся в каждую программу с окнами
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows";
        std::wstring dlls = Trim(RegStr(HKEY_LOCAL_MACHINE, key, L"AppInit_DLLs", view));
        if (!dlls.empty() && RegDword(HKEY_LOCAL_MACHINE, key, L"LoadAppInit_DLLs", view))
            out.push_back(RegFinding(L"AppInit_DLLs", L"HKLM\\" + std::wstring(key), dlls, 5,
                                     L"в каждую программу подгружается " + dlls));
    }
    // запреты, которые вирусы ставят, чтобы их не нашли и не убрали
    const wchar_t* pol = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    if (RegDword(HKEY_CURRENT_USER, pol, L"DisableTaskMgr"))
        out.push_back(RegFinding(L"Диспетчер задач запрещён", L"HKCU\\" + std::wstring(pol), L"DisableTaskMgr = 1", 5,
                                 L"Диспетчер задач выключен политикой"));
    if (RegDword(HKEY_CURRENT_USER, pol, L"DisableRegistryTools"))
        out.push_back(RegFinding(L"Редактор реестра запрещён", L"HKCU\\" + std::wstring(pol),
                                 L"DisableRegistryTools = 1", 5, L"regedit выключен политикой"));
    // запуск .exe перехвачен на уровне пользователя
    std::wstring exeCmd = RegStr(HKEY_CURRENT_USER, L"Software\\Classes\\exefile\\shell\\open\\command", L"");
    if (!exeCmd.empty())
        out.push_back(RegFinding(L"Запуск .exe", L"HKCU\\Software\\Classes\\exefile\\shell\\open\\command", exeCmd, 6,
                                 L"каждая программа запускается через " + exeCmd));
}

// Строка автозапуска запускает скрипт (часто так прячутся без своего exe).
std::wstring ScriptLaunch(const std::wstring& cmd) {
    std::wstring l = Lower(cmd);
    auto has = [&](const wchar_t* s) { return l.find(s) != std::wstring::npos; };
    if (has(L"mshta")) return L"запускает mshta (HTML-приложение)";
    if (has(L"wscript") || has(L"cscript")) return L"запускает скрипт Windows Script Host";
    if ((has(L"powershell") || has(L"pwsh")) &&
        (has(L" -enc") || has(L" -e ") || has(L"frombase64") || has(L"hidden") || has(L"downloadstring") ||
         has(L"iex ") || has(L"iex(")))
        return L"скрытый или закодированный PowerShell";
    if (has(L"regsvr32") && (has(L"/i:http") || has(L"scrobj"))) return L"regsvr32 грузит скрипт из сети";
    if (has(L"rundll32") && (has(L"\\appdata\\") || has(L"\\temp\\") || has(L"javascript:")))
        return L"rundll32 с библиотекой из AppData или TEMP";
    if (has(L"cmd") && (has(L"/c ") || has(L"/k ")) && (has(L"\\appdata\\") || has(L"\\temp\\")))
        return L"командная строка из AppData или TEMP";
    return L"";
}

// Первое слово командной строки как файл: «"C:\x y\a.exe" -k» -> C:\x y\a.exe, «wscript.exe» - по PATH.
std::wstring ExeOfCommand(const std::wstring& raw) {
    std::wstring c = Trim(Expand(raw)), exe;
    if (c.empty()) return L"";
    if (c[0] == L'"') {
        size_t e = c.find(L'"', 1);
        exe = c.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
    } else {
        std::wstring l = Lower(c);
        size_t e = l.find(L".exe");
        exe = e != std::wstring::npos ? c.substr(0, e + 4) : c.substr(0, c.find(L' '));
    }
    if (!exe.empty() && exe.find(L'\\') == std::wstring::npos) {
        wchar_t found[MAX_PATH];
        if (SearchPathW(nullptr, exe.c_str(), L".exe", MAX_PATH, found, nullptr)) exe = found;
    }
    return exe;
}

// ---------------- задачи планировщика ----------------

VARIANT Var(LONG i) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = i;
    return v;
}

struct Bstr {
    BSTR b = nullptr;
    Bstr() = default;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(b); }
    std::wstring str() const { return b ? std::wstring(b, SysStringLen(b)) : L""; }
};

void WalkTasks(ITaskFolder* folder, std::vector<Suspect>& out, int& checked, const std::wstring& ownDir) {
    IRegisteredTaskCollection* tasks = nullptr;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks))) {
        LONG n = 0;
        tasks->get_Count(&n);
        for (LONG i = 1; i <= n; ++i) {
            IRegisteredTask* t = nullptr;
            if (FAILED(tasks->get_Item(Var(i), &t))) continue;
            VARIANT_BOOL on = VARIANT_FALSE;
            t->get_Enabled(&on);
            Bstr path;
            t->get_Path(&path.b);
            ITaskDefinition* def = nullptr;
            if (on && SUCCEEDED(t->get_Definition(&def))) {
                IActionCollection* acts = nullptr;
                if (SUCCEEDED(def->get_Actions(&acts))) {
                    LONG na = 0;
                    acts->get_Count(&na);
                    for (LONG a = 1; a <= na; ++a) {
                        IAction* act = nullptr;
                        if (FAILED(acts->get_Item(a, &act))) continue;
                        IExecAction* ex = nullptr;
                        if (SUCCEEDED(act->QueryInterface(IID_IExecAction, (void**)&ex))) {
                            Bstr prog, args;
                            ex->get_Path(&prog.b);
                            ex->get_Arguments(&args.b);
                            ++checked;
                            Suspect s;
                            s.kind = FIND_TASK;
                            s.where = path.str();
                            s.name = LeafOf(s.where);
                            std::wstring raw = prog.str();
                            std::wstring cmd =
                                (raw.find(L' ') != std::wstring::npos && raw[0] != L'"' ? L"\"" + raw + L"\"" : raw) +
                                L" " + args.str();
                            s.path = ExeOfCommand(cmd);
                            std::wstring script = ScriptLaunch(cmd);
                            if (!script.empty()) Flag(s, 4, script);
                            std::wstring lp = Lower(s.path);  // своя задача автозапуска - не находка
                            bool own = Dir(lp) == ownDir && LeafOf(lp) == L"radialmenu.exe";
                            if (!s.path.empty() && !own) JudgeFile(s);
                            if (s.sign == SIGN_NOFILE && s.score <= 1) s.score = 0;  // мёртвая задача - не опасна
                            s.parent = Trim(cmd);                                    // для карточки: полная команда
                            if (s.score >= kSuspectScore) out.push_back(s);
                            ex->Release();
                        }
                        act->Release();
                    }
                    acts->Release();
                }
                def->Release();
            }
            t->Release();
        }
        tasks->Release();
    }
    ITaskFolderCollection* subs = nullptr;
    if (SUCCEEDED(folder->GetFolders(0, &subs))) {
        LONG n = 0;
        subs->get_Count(&n);
        for (LONG i = 1; i <= n; ++i) {
            ITaskFolder* f = nullptr;
            if (SUCCEEDED(subs->get_Item(Var(i), &f))) {
                WalkTasks(f, out, checked, ownDir);
                f->Release();
            }
        }
        subs->Release();
    }
}

ITaskService* TaskService() {
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc)))
        return nullptr;
    if (FAILED(svc->Connect(VARIANT{}, VARIANT{}, VARIANT{}, VARIANT{}))) {
        svc->Release();
        return nullptr;
    }
    return svc;
}

void CheckTasks(std::vector<Suspect>& out, int& checked, const std::wstring& ownDir) {
    ITaskService* svc = TaskService();
    if (!svc) return;
    ITaskFolder* root = nullptr;
    if (SUCCEEDED(svc->GetFolder(Bstr(L"\\").b, &root))) {
        WalkTasks(root, out, checked, ownDir);
        root->Release();
    }
    svc->Release();
}

// ---------------- расширения браузеров ----------------

std::string ReadSmall(const std::wstring& path) {
    std::string s;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    char chunk[16384];
    DWORD got = 0;
    while (s.size() < 4 * 1024 * 1024 && ReadFile(f, chunk, sizeof(chunk), &got, nullptr) && got) s.append(chunk, got);
    CloseHandle(f);
    return s;
}

// Строковое поле JSON верхнего уровня (без полного разбора: для manifest.json хватает)
std::wstring JsonStr(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    for (size_t a = js.find(k); a != std::string::npos; a = js.find(k, a + 1)) {
        size_t c = js.find_first_not_of(" \t\r\n", a + k.size());
        if (c == std::string::npos || js[c] != ':') continue;
        size_t q = js.find_first_not_of(" \t\r\n", c + 1);
        if (q == std::string::npos || js[q] != '"') return L"";
        std::string v;
        for (size_t i = q + 1; i < js.size() && js[i] != '"'; ++i)
            v += js[i] == '\\' && i + 1 < js.size() ? js[++i] : js[i];
        return Widen(v);
    }
    return L"";
}

// «__MSG_appName__» -> строка из _locales/<язык>/messages.json
std::wstring ExtName(const std::wstring& dir, const std::string& manifest) {
    std::wstring name = JsonStr(manifest, "name");
    if (name.rfind(L"__MSG_", 0) != 0) return name;
    std::string key = Narrow(name.substr(6, name.size() >= 8 ? name.size() - 8 : 0));
    std::wstring loc = JsonStr(manifest, "default_locale");
    for (const std::wstring& l : {std::wstring(L"ru"), loc, std::wstring(L"en")}) {
        if (l.empty()) continue;
        std::string msgs = ReadSmall(dir + L"\\_locales\\" + l + L"\\messages.json");
        if (msgs.empty()) continue;
        for (const std::string& k : {key, std::string(Narrow(Lower(Widen(key))))}) {
            size_t a = msgs.find("\"" + k + "\"");
            if (a == std::string::npos) continue;
            std::wstring m = JsonStr(msgs.substr(a), "message");
            if (!m.empty()) return m;
        }
    }
    return name;
}

void CheckExtensions(std::vector<Suspect>& out, int& checked) {
    struct Browser {
        const wchar_t *name, *dir, *store;
    };
    static const Browser kBrowsers[] = {
        {L"Chrome", L"%LOCALAPPDATA%\\Google\\Chrome\\User Data", L"clients2.google.com"},
        {L"Edge", L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data", L"edge.microsoft.com"},
        {L"Brave", L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data", L"clients2.google.com"},
        {L"Яндекс", L"%LOCALAPPDATA%\\Yandex\\YandexBrowser\\User Data", L"yandex"},
    };
    auto dirs = [](const std::wstring& d) {
        std::vector<std::wstring> out;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                out.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
        return out;
    };
    for (const Browser& b : kBrowsers) {
        std::wstring root = Expand(b.dir);
        for (const std::wstring& profile : dirs(root)) {
            if (profile != L"Default" && profile.rfind(L"Profile ", 0) != 0) continue;
            std::wstring ext = root + L"\\" + profile + L"\\Extensions";
            for (const std::wstring& id : dirs(ext)) {
                if (id == L"Temp") continue;
                std::vector<std::wstring> vers = dirs(ext + L"\\" + id);
                if (vers.empty()) continue;
                std::sort(vers.begin(), vers.end());
                std::wstring dir = ext + L"\\" + id + L"\\" + vers.back();
                std::string man = ReadSmall(dir + L"\\manifest.json");
                if (man.empty()) continue;
                ++checked;
                Suspect s;
                s.kind = FIND_EXT;
                s.name = ExtName(dir, man);
                s.path = dir;
                s.where = std::wstring(b.name) + (profile == L"Default" ? L"" : L", " + profile);
                std::wstring upd = Lower(JsonStr(man, "update_url"));
                if (upd.empty())
                    Flag(s, 3, L"поставлено не из магазина (нет адреса обновлений)");
                else if (upd.find(b.store) == std::wstring::npos && upd.find(L"google.com") == std::wstring::npos &&
                         upd.find(L"microsoft.com") == std::wstring::npos)
                    Flag(s, 4, L"обновляется не из магазина: " + upd);
                if (s.score && man.find("<all_urls>") != std::string::npos) Flag(s, 1, L"читает и меняет все сайты");
                if (s.score >= kSuspectScore) out.push_back(s);
            }
        }
    }
}

// ---------------- hosts, DNS, прокси ----------------

bool PrivateIp(const std::wstring& ip) {
    unsigned a = 0, b = 0;
    if (swscanf(ip.c_str(), L"%u.%u", &a, &b) != 2) return ip == L"::1" || Lower(ip).rfind(L"fe80", 0) == 0;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 100 && b >= 64 && b <= 127) || (a == 169 && b == 254) || a == 0;
}

void CheckNetwork(std::vector<Suspect>& out) {
    // hosts: подмена адресов антивирусов, обновлений, банков и входа в аккаунты - признак заражения;
    // свои записи (Docker, локальные имена) - нормально
    static const wchar_t* kGuarded[] = {L"kaspersky",
                                        L"virustotal",
                                        L"eset",
                                        L"drweb",
                                        L"avast",
                                        L"malwarebytes",
                                        L"windowsupdate",
                                        L"update.microsoft",
                                        L"download.microsoft",
                                        L"login.live.com",
                                        L"login.microsoftonline",
                                        L"accounts.google",
                                        L"sberbank",
                                        L"tinkoff",
                                        L"tbank",
                                        L"paypal",
                                        L"steamcommunity",
                                        L"steampowered",
                                        L"github.com",
                                        L"vk.com",
                                        L"gosuslugi"};
    std::wstring hostsPath = Expand(L"%SystemRoot%\\System32\\drivers\\etc\\hosts");
    for (const std::wstring& raw : Split(Widen(ReadSmall(hostsPath)), L'\n')) {
        std::wstring line = Trim(raw.substr(0, raw.find(L'#')));
        if (line.empty()) continue;
        std::vector<std::wstring> parts;
        for (const std::wstring& p : Split(line, L' '))
            for (const std::wstring& q : Split(p, L'\t'))
                if (!q.empty()) parts.push_back(q);
        if (parts.size() < 2) continue;
        for (size_t i = 1; i < parts.size(); ++i) {
            std::wstring host = Lower(parts[i]);
            bool guarded = false;
            for (const wchar_t* g : kGuarded) guarded |= host.find(g) != std::wstring::npos;
            bool local = host.find(L'.') == std::wstring::npos || EndsWithI(host, L".local") ||
                         EndsWithI(host, L".internal") || EndsWithI(host, L".test") || EndsWithI(host, L".localhost") ||
                         EndsWithI(host, L".lan");
            Suspect s;
            s.kind = FIND_NET;
            s.name = L"hosts: " + host;
            s.where = hostsPath;
            s.path = parts[0] + L" " + host;
            if (guarded)
                Flag(s, 6,
                     (parts[0] == L"0.0.0.0" || parts[0] == L"127.0.0.1" ? L"hosts блокирует "
                                                                         : L"hosts подменяет адрес ") +
                         host + L" (" + parts[0] + L")");
            else if (!local && !PrivateIp(parts[0]))
                Flag(s, 3, L"hosts направляет " + host + L" на " + parts[0]);
            if (s.score >= kSuspectScore) out.push_back(s);
        }
    }
    // DNS, заданный вручную на адаптере: известные публичные и свои (роутер, туннель) - нормально
    static const wchar_t* kKnownDns[] = {L"1.1.1.1",   L"1.0.0.1",         L"8.8.8.8",        L"8.8.4.4",
                                         L"9.9.9.9",   L"149.112.112.112", L"208.67.222.222", L"208.67.220.220",
                                         L"77.88.8.8", L"77.88.8.1",       L"94.140.14.14",   L"94.140.15.15",
                                         L"76.76.2.0", L"76.76.10.0"};
    std::map<std::wstring, std::wstring> names;  // {guid} -> имя адаптера
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, nullptr, &size);
    std::vector<BYTE> buf(size + 1024);
    size = (ULONG)buf.size();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                             (IP_ADAPTER_ADDRESSES*)buf.data(), &size) == NO_ERROR)
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next)
            names[Lower(Widen(a->AdapterName))] = a->FriendlyName;
    const wchar_t* ifKey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ifKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t sub[128];
        for (DWORD i = 0;; ++i) {
            DWORD n = 128;
            if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            auto it = names.find(Lower(sub));
            if (it == names.end()) continue;  // адаптера уже нет
            std::wstring ns = RegStr(HKEY_LOCAL_MACHINE, (std::wstring(ifKey) + L"\\" + sub).c_str(), L"NameServer");
            for (const std::wstring& part : Split(ns, L',')) {
                for (const std::wstring& ip : Split(part, L' ')) {
                    if (ip.empty() || PrivateIp(ip)) continue;
                    bool known = false;
                    for (const wchar_t* d : kKnownDns) known |= ip == d;
                    if (known) continue;
                    Suspect s;
                    s.kind = FIND_NET;
                    s.name = L"DNS " + ip;
                    s.where = it->second;
                    s.path = ip;
                    Flag(s, 3, L"на адаптере «" + it->second + L"» вручную задан незнакомый DNS " + ip);
                    out.push_back(s);
                }
            }
        }
        RegCloseKey(k);
    }
    // системный прокси и PAC на чужой сервер (свой - 127.0.0.1, как у Happ в режиме прокси)
    const wchar_t* inet = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    std::wstring proxy = Lower(RegStr(HKEY_CURRENT_USER, inet, L"ProxyServer"));
    if (RegDword(HKEY_CURRENT_USER, inet, L"ProxyEnable") && !proxy.empty() &&
        proxy.find(L"127.0.0.1") == std::wstring::npos && proxy.find(L"localhost") == std::wstring::npos) {
        Suspect s;
        s.kind = FIND_NET;
        s.name = L"Прокси";
        s.where = L"HKCU\\" + std::wstring(inet);
        s.path = proxy;
        Flag(s, 3, L"весь трафик идёт через прокси " + proxy);
        out.push_back(s);
    }
    std::wstring pac = Lower(RegStr(HKEY_CURRENT_USER, inet, L"AutoConfigURL"));
    if (!pac.empty() && pac.find(L"127.0.0.1") == std::wstring::npos && pac.find(L"localhost") == std::wstring::npos) {
        Suspect s;
        s.kind = FIND_NET;
        s.name = L"PAC-скрипт";
        s.where = L"HKCU\\" + std::wstring(inet);
        s.path = pac;
        Flag(s, 4, L"адреса сайтов решает скрипт " + pac);
        out.push_back(s);
    }
}

}  // namespace

bool SetTaskEnabled(const std::wstring& taskPath, bool on, const Log& log) {
    ITaskService* svc = TaskService();
    if (!svc) {
        log(L"Планировщик заданий недоступен.");
        return false;
    }
    size_t cut = taskPath.find_last_of(L'\\');
    std::wstring folder = cut == 0 || cut == std::wstring::npos ? L"\\" : taskPath.substr(0, cut);
    std::wstring name = cut == std::wstring::npos ? taskPath : taskPath.substr(cut + 1);
    ITaskFolder* f = nullptr;
    IRegisteredTask* t = nullptr;
    HRESULT hr = svc->GetFolder(Bstr(folder.c_str()).b, &f);
    if (SUCCEEDED(hr)) hr = f->GetTask(Bstr(name.c_str()).b, &t);
    if (SUCCEEDED(hr)) hr = t->put_Enabled(on ? VARIANT_TRUE : VARIANT_FALSE);
    if (t) t->Release();
    if (f) f->Release();
    svc->Release();
    if (SUCCEEDED(hr)) {
        log(std::wstring(L"Задача «") + name +
            (on ? L"» включена." : L"» выключена. Включить обратно - в Планировщике заданий."));
        return true;
    }
    log(hr == E_ACCESSDENIED ? L"Это задача системы или другого пользователя: выключить можно только с правами."
                             : L"Выключить не получилось.");
    return false;
}

SignState CheckSign(const std::wstring& path, std::wstring* signer) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return SIGN_NOFILE;
    ULONGLONG stamp = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    ULONGLONG size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    std::wstring key = Lower(path);
    AcquireSRWLockShared(&g_signLock);
    auto it = g_signs.find(key);
    bool hit = it != g_signs.end() && it->second.stamp == stamp && it->second.size == size;
    SignCache c = hit ? it->second : SignCache{};
    ReleaseSRWLockShared(&g_signLock);
    if (hit) {
        if (signer) *signer = c.signer;
        return c.state;
    }
    c = SignCache{stamp, size, SIGN_NONE, L""};
    WINTRUST_FILE_INFO fi{sizeof(fi)};
    fi.pcwszFilePath = path.c_str();
    WINTRUST_DATA wd{};
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    c.state = FromResult(Verify(wd, &c.signer));
    if (c.state == SIGN_NONE) {
        c.signer.clear();
        c.state = CatalogSign(path, &c.signer);
    }
    AcquireSRWLockExclusive(&g_signLock);
    g_signs[key] = c;
    ReleaseSRWLockExclusive(&g_signLock);
    if (signer) *signer = c.signer;
    return c.state;
}

SecurityScan ScanSecurity() {
    SecurityScan r;
    std::set<DWORD> net = NetPids(), windowed;
    EnumWindows(CollectWindowPid, (LPARAM)&windowed);
    wchar_t own[MAX_PATH];
    GetModuleFileNameW(nullptr, own, MAX_PATH);
    std::wstring ownDir = Lower(Dir(own));
    // своё пропускаем поимённо: вся папка программы - обычная папка пользователя, и exe, подложенный туда,
    // иначе был бы невидим для проверки
    auto mine = [&](const std::wstring& lowerPath) {
        return Dir(lowerPath) == ownDir &&
               (LeafOf(lowerPath) == L"radialmenu.exe" || LeafOf(lowerPath) == L"radialtools.exe");
    };

    std::map<DWORD, PROCESSENTRY32W> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) procs[pe.th32ProcessID] = pe;
        CloseHandle(snap);
    }
    std::set<std::wstring> unsignedPaths;
    for (const auto& [pid, pe] : procs) {
        if (pid <= 4) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;  // защищённые системные процессы: путь не узнать
        wchar_t p[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        bool got = QueryFullProcessImageNameW(h, 0, p, &n);
        CloseHandle(h);
        if (!got) continue;
        ++r.processes;
        Suspect s;
        s.pid = pid;
        s.name = pe.szExeFile;
        s.path.assign(p, n);
        std::wstring lp = Lower(s.path);
        if (mine(lp)) continue;
        auto par = procs.find(pe.th32ParentProcessID);
        if (par != procs.end()) s.parent = par->second.szExeFile;
        s.net = net.count(pid) > 0;
        JudgeFile(s);
        if (s.sign == SIGN_NONE || s.sign == SIGN_UNTRUSTED) unsignedPaths.insert(lp);
        // настоящий svchost запускает только services.exe
        if (Lower(s.name) == L"svchost.exe" && par != procs.end() && Lower(s.parent) != L"services.exe")
            Flag(s, 4, L"svchost запущен не службами, а " + s.parent);
        if (s.sign != SIGN_OK && s.net && !windowed.count(pid)) Flag(s, 1, L"без окна, но с соединениями в сеть");
        if (s.score >= kSuspectScore) r.procs.push_back(s);
    }
    r.unsignedCount = (int)unsignedPaths.size();

    // автозапуск: что включено и что именно запустится
    r.starts = StartupItems();
    for (size_t i = 0; i < r.starts.size(); ++i) {
        const StartItem& it = r.starts[i];
        if (!it.enabled) continue;
        ++r.autoruns;
        Suspect s;
        s.name = it.name;
        s.where = it.where;
        s.kind = FIND_AUTO;
        s.start = (int)i;
        s.path = Expand(StartExe(it));
        if (!s.path.empty() && s.path.find(L'\\') == std::wstring::npos) {  // «wscript.exe» - ищется как Windows
            wchar_t found[MAX_PATH];
            if (SearchPathW(nullptr, s.path.c_str(), L".exe", MAX_PATH, found, nullptr)) s.path = found;
        }
        std::wstring script = ScriptLaunch(it.command);
        if (!script.empty()) Flag(s, 4, script);
        if (!s.path.empty() && !mine(Lower(s.path))) JudgeFile(s);
        if (s.score >= kSuspectScore) r.autos.push_back(s);
    }
    CheckRegistry(r.hijacks);
    r.hijacks.erase(
        std::remove_if(r.hijacks.begin(), r.hijacks.end(), [](const Suspect& s) { return s.score < kSuspectScore; }),
        r.hijacks.end());
    auto byScore = [](const Suspect& a, const Suspect& b) { return a.score > b.score; };
    std::stable_sort(r.procs.begin(), r.procs.end(), byScore);
    std::stable_sort(r.autos.begin(), r.autos.end(), byScore);
    CheckTasks(r.tasks, r.tasksChecked, ownDir);
    CheckExtensions(r.exts, r.extsChecked);
    CheckNetwork(r.nets);
    std::stable_sort(r.hijacks.begin(), r.hijacks.end(), byScore);
    std::stable_sort(r.tasks.begin(), r.tasks.end(), byScore);
    std::stable_sort(r.exts.begin(), r.exts.end(), byScore);
    std::stable_sort(r.nets.begin(), r.nets.end(), byScore);
    return r;
}

std::wstring AntivirusUi() {
    for (const wchar_t* root : {L"%ProgramFiles(x86)%\\Kaspersky Lab", L"%ProgramFiles%\\Kaspersky Lab"}) {
        std::wstring dir = Expand(root);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::wstring found;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring ui = dir + L"\\" + fd.cFileName + L"\\avpui.exe";
            if (GetFileAttributesW(ui.c_str()) != INVALID_FILE_ATTRIBUTES) found = ui;
        } while (found.empty() && FindNextFileW(h, &fd));
        FindClose(h);
        if (!found.empty()) return found;
    }
    return L"";
}

std::vector<SecurityApp> SecurityApps(const std::wstring& dirIn) {
    std::vector<SecurityApp> out;
    std::wstring dir = Expand(dirIn);
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    if (dir.empty()) return out;
    std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& d, int depth) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> subs;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring l = Lower(n);
                if (l == L"arm64" || l == L"i386" || l == L"x86" || l == L"plugins") continue;
                subs.push_back(d + L"\\" + n);
            } else if (EndsWithI(n, L".exe")) {
                std::wstring l = Lower(n);
                // установщики, деинсталляторы и вспомогательное - не пункты кольца
                if (l.find(L"unins") == 0 || l.find(L"setup") != std::wstring::npos || l == L"peview.exe" ||
                    l.find(L"update") != std::wstring::npos)
                    continue;
                SecurityApp a;
                a.exe = d + L"\\" + n;
                a.name = n.substr(0, n.size() - 4);
                if (Lower(a.name) == L"systeminformer") a.name = L"System Informer";
                out.push_back(a);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (depth < 2)
            for (const std::wstring& s : subs) walk(s, depth + 1);
    };
    walk(dir, 0);
    std::sort(out.begin(), out.end(),
              [](const SecurityApp& a, const SecurityApp& b) { return Lower(a.name) < Lower(b.name); });
    return out;
}

}  // namespace tools
