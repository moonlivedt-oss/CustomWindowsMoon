// Защита без своего движка: подписи запущенных программ и автозапуска, откуда они запущены, подмена системных
// имён, перехваты Winlogon и запуска программ в реестре. Только смотрит; завершение - через RadialTools.exe.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <mscat.h>
#include <softpub.h>
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
    if ((has(L"powershell") || has(L"pwsh")) && (has(L" -enc") || has(L" -e ") || has(L"frombase64") ||
                                                 has(L"hidden") || has(L"downloadstring") || has(L"iex")))
        return L"скрытый или закодированный PowerShell";
    if (has(L"regsvr32") && (has(L"/i:http") || has(L"scrobj"))) return L"regsvr32 грузит скрипт из сети";
    if (has(L"rundll32") && (has(L"\\appdata\\") || has(L"\\temp\\") || has(L"javascript:")))
        return L"rundll32 с библиотекой из AppData или TEMP";
    if (has(L"cmd") && (has(L"/c ") || has(L"/k ")) && (has(L"\\appdata\\") || has(L"\\temp\\")))
        return L"командная строка из AppData или TEMP";
    return L"";
}

}  // namespace

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
        if (Dir(lp) == ownDir) continue;  // RadialMenu и RadialTools
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
        s.start = (int)i;
        s.path = Expand(StartExe(it));
        if (!s.path.empty() && s.path.find(L'\\') == std::wstring::npos) {  // «wscript.exe» - ищется как Windows
            wchar_t found[MAX_PATH];
            if (SearchPathW(nullptr, s.path.c_str(), L".exe", MAX_PATH, found, nullptr)) s.path = found;
        }
        std::wstring script = ScriptLaunch(it.command);
        if (!script.empty()) Flag(s, 4, script);
        if (!s.path.empty() && Dir(Lower(s.path)) != ownDir) JudgeFile(s);
        if (s.score >= kSuspectScore) r.autos.push_back(s);
    }
    CheckRegistry(r.hijacks);
    r.hijacks.erase(
        std::remove_if(r.hijacks.begin(), r.hijacks.end(), [](const Suspect& s) { return s.score < kSuspectScore; }),
        r.hijacks.end());
    auto byScore = [](const Suspect& a, const Suspect& b) { return a.score > b.score; };
    std::stable_sort(r.procs.begin(), r.procs.end(), byScore);
    std::stable_sort(r.autos.begin(), r.autos.end(), byScore);
    std::stable_sort(r.hijacks.begin(), r.hijacks.end(), byScore);
    return r;
}

std::wstring FileSha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::wstring out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<BYTE> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
            ok = BCryptHashData(hh, buf.data(), got, 0) == 0;
        BYTE hash[32];
        if (ok && BCryptFinishHash(hh, hash, 32, 0) == 0) {
            wchar_t hx[3];
            for (BYTE b : hash) swprintf(hx, 3, L"%02x", b), out += hx;
        }
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
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
