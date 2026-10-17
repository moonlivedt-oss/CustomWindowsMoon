// Приватность и доверие: кто брал камеру, микрофон и геопозицию; чужие корневые сертификаты; откуда скачан файл;
// входящие разрешения брандмауэра для программ из папок пользователя. Только смотрит.
#include <windows.h>
#include <netfw.h>
#include <wincrypt.h>

#include <algorithm>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

ULONGLONG RegQword(HKEY k, const wchar_t* name) {
    ULONGLONG v = 0;
    DWORD size = sizeof(v);
    return RegQueryValueExW(k, name, nullptr, nullptr, (BYTE*)&v, &size) == ERROR_SUCCESS ? v : 0;
}

void ReadConsent(HKEY root, const std::wstring& base, const wchar_t* cap, bool packaged, std::vector<PrivacyUse>& out) {
    HKEY k;
    if (RegOpenKeyExW(root, base.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD n = 512;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (!_wcsicmp(name, L"NonPackaged")) continue;
        HKEY app;
        if (RegOpenKeyExW(k, name, 0, KEY_READ, &app) != ERROR_SUCCESS) continue;
        PrivacyUse u;
        u.capability = cap;
        u.start = RegQword(app, L"LastUsedTimeStart");
        u.stop = RegQword(app, L"LastUsedTimeStop");
        RegCloseKey(app);
        if (!u.start) continue;
        u.app = name;
        if (!packaged) {
            // обычная программа: путь записан с # вместо \ («C:#Program Files#X#x.exe»)
            std::replace(u.app.begin(), u.app.end(), L'#', L'\\');
            u.path = u.app;
            u.app = u.path.substr(u.path.find_last_of(L'\\') + 1);
        } else {
            // приложение Магазина: «Microsoft.WindowsCamera_8wekyb3d8bbwe» -> «WindowsCamera»
            std::wstring a = u.app.substr(0, u.app.find(L'_'));
            size_t dot = a.find_last_of(L'.');
            u.app = dot == std::wstring::npos ? a : a.substr(dot + 1);
        }
        u.inUse = u.stop == 0 || u.stop < u.start;
        out.push_back(u);
    }
    RegCloseKey(k);
}

}  // namespace

// ---------------- камера, микрофон, геопозиция ----------------

std::vector<PrivacyUse> PrivacyUses() {
    std::vector<PrivacyUse> out;
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    struct Cap {
        const wchar_t *key, *name;
    } caps[] = {{L"webcam", L"камера"}, {L"microphone", L"микрофон"}, {L"location", L"геопозиция"}};
    for (const Cap& c : caps) {
        std::wstring k = std::wstring(base) + c.key;
        ReadConsent(HKEY_CURRENT_USER, k, c.name, true, out);
        ReadConsent(HKEY_CURRENT_USER, k + L"\\NonPackaged", c.name, false, out);
    }
    std::sort(out.begin(), out.end(), [](const PrivacyUse& a, const PrivacyUse& b) {
        return a.inUse != b.inUse ? a.inUse : a.start > b.start;
    });
    for (PrivacyUse& u : out)
        if (!u.path.empty()) {
            SignState st = CheckSign(u.path, &u.signer);
            u.unsignedApp = st != SIGN_OK && st != SIGN_NOFILE;
        }
    return out;
}

// ---------------- корневые сертификаты ----------------

namespace {

// Корни, которые ставят известные программы сами (перехват HTTPS антивирусом и т. п.) или которые стоят
// в Windows из коробки, - по имени. Остальные в «своих» хранилищах (не из программы Microsoft) - с вопросом.
bool KnownRoot(const std::wstring& lowerName) {
    static const wchar_t* kKnown[] = {L"microsoft",       L"kaspersky",    L"digicert",  L"globalsign",
                                      L"verisign",        L"thawte",       L"symantec",  L"comodo",
                                      L"sectigo",         L"usertrust",    L"addtrust",  L"entrust",
                                      L"go daddy",        L"godaddy",      L"starfield", L"isrg",
                                      L"identrust",       L"certum",       L"ssl.com",   L"securetrust",
                                      L"trustwave",       L"google trust", L"gts root",  L"harica",
                                      L"hellenic",        L"emsign",       L"secom",     L"security communication",
                                      L"aaa certificate", L"baltimore",    L"quovadis",  L"amazon",
                                      L"buypass",         L"actalis",      L"d-trust",   L"t-telesec",
                                      L"swisssign",       L"izenpe",       L"certigna",  L"network solutions",
                                      L"affirmtrust",     L"xramp",        L"twca",      L"telia",
                                      L"russian trusted", L"минцифр"};
    for (const wchar_t* k : kKnown)
        if (lowerName.find(k) != std::wstring::npos) return true;
    return false;
}

std::wstring CertName(PCCERT_CONTEXT c, DWORD flags = 0) {
    wchar_t buf[256] = L"";
    CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, buf, 256);
    return buf;
}

}  // namespace

void CheckCerts(std::vector<Suspect>& out) {
    // хранилища в реестре без «сторонних корней Microsoft» (AuthRoot): сюда корни кладут руками и программы
    struct Store {
        DWORD loc;
        const wchar_t* where;
    } stores[] = {{CERT_SYSTEM_STORE_CURRENT_USER, L"Корневые сертификаты пользователя"},
                  {CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Корневые сертификаты компьютера"}};
    for (const Store& st : stores) {
        HCERTSTORE s = CertOpenStore(CERT_STORE_PROV_SYSTEM_REGISTRY_W, 0, 0,
                                     st.loc | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, L"Root");
        if (!s) continue;
        for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(s, c));) {
            std::wstring name = CertName(c), issuer = CertName(c, CERT_NAME_ISSUER_FLAG);
            if (KnownRoot(Lower(name)) || KnownRoot(Lower(issuer))) continue;
            FILETIME nb = c->pCertInfo->NotBefore;
            SYSTEMTIME t;
            FileTimeToSystemTime(&nb, &t);
            wchar_t since[16];
            swprintf(since, 16, L"%02d.%02d.%04d", t.wDay, t.wMonth, t.wYear);
            Suspect sp;
            sp.kind = FIND_CERT;
            sp.name = L"Корень: " + name;
            sp.where = st.where;
            sp.path = name;
            sp.score = st.loc == CERT_SYSTEM_STORE_CURRENT_USER ? 4 : 3;
            sp.why = std::wstring(L"незнакомый корневой сертификат (с ") + since +
                     L"): тот, у кого есть его ключ, может подменять сайты и подписывать программы так, что Windows "
                     L"им доверяет. Свои сертификаты разработки - нормально, остальные стоит убрать";
            out.push_back(sp);
        }
        CertCloseStore(s, 0);
    }
}

// ---------------- откуда скачан ----------------

std::wstring FileOrigin(const std::wstring& path) {
    HANDLE f = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    char buf[4096] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    std::wstring host, ref, zone;
    for (const std::wstring& line : Split(Widen(std::string(buf, got)), L'\n')) {
        if (line.rfind(L"HostUrl=", 0) == 0) host = line.substr(8);
        if (line.rfind(L"ReferrerUrl=", 0) == 0) ref = line.substr(12);
        if (line.rfind(L"ZoneId=", 0) == 0) zone = line.substr(7);
    }
    std::wstring url = !host.empty() && host != L"about:internet" ? host : ref;
    if (!url.empty()) return url;
    return zone == L"3" ? L"из интернета (адрес не сохранён)" : L"";
}

// ---------------- брандмауэр ----------------

namespace {
// GUID задаём сами: в заголовках MinGW они объявлены, но не всегда есть в библиотеке
const CLSID kFwPolicy2 = {0xE2B3C97F, 0x6AE1, 0x41AC, {0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD}};
const IID kIFwPolicy2 = {0x98325047, 0xC671, 0x4174, {0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86}};
const IID kIFwRule = {0xAF230D27, 0xBABA, 0x4E42, {0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2}};

bool UserPlace(const std::wstring& lowerPath) {
    return lowerPath.find(L"\\users\\") != std::wstring::npos || lowerPath.find(L"\\temp\\") != std::wstring::npos ||
           lowerPath.find(L"\\downloads\\") != std::wstring::npos;
}
}  // namespace

void CheckFirewall(std::vector<Suspect>& out) {
    INetFwPolicy2* pol = nullptr;
    if (FAILED(CoCreateInstance(kFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, kIFwPolicy2, (void**)&pol))) return;
    INetFwRules* rules = nullptr;
    IUnknown* en = nullptr;
    IEnumVARIANT* it = nullptr;
    if (SUCCEEDED(pol->get_Rules(&rules)) && SUCCEEDED(rules->get__NewEnum(&en)) &&
        SUCCEEDED(en->QueryInterface(IID_IEnumVARIANT, (void**)&it))) {
        VARIANT v;
        VariantInit(&v);
        ULONG got = 0;
        while (it->Next(1, &v, &got) == S_OK && got) {
            INetFwRule* r = nullptr;
            if (v.vt == VT_DISPATCH && v.pdispVal && SUCCEEDED(v.pdispVal->QueryInterface(kIFwRule, (void**)&r))) {
                NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
                NET_FW_ACTION act = NET_FW_ACTION_BLOCK;
                VARIANT_BOOL on = VARIANT_FALSE;
                BSTR app = nullptr, name = nullptr;
                r->get_Direction(&dir);
                r->get_Action(&act);
                r->get_Enabled(&on);
                r->get_ApplicationName(&app);
                r->get_Name(&name);
                std::wstring path = app ? app : L"", lp = Lower(path);
                if (on && dir == NET_FW_RULE_DIR_IN && act == NET_FW_ACTION_ALLOW && !path.empty() && UserPlace(lp)) {
                    Suspect s;
                    s.kind = FIND_FW;
                    s.name = L"Брандмауэр: " + std::wstring(name ? name : L"");
                    s.where = L"Входящие разрешения брандмауэра";
                    s.path = path;
                    s.score = 2;
                    s.why = L"программа из папки пользователя может принимать входящие подключения из сети";
                    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        s.score = 1;  // программы уже нет - правило-хвост
                        s.why = L"правило осталось от программы, которой уже нет";
                    } else if (CheckSign(path, &s.signer) != SIGN_OK) {
                        s.score += 2;
                        s.why += L"; без подписи";
                    }
                    bool dup = false;
                    for (const Suspect& o : out) dup |= o.kind == FIND_FW && !_wcsicmp(o.path.c_str(), path.c_str());
                    if (!dup) out.push_back(s);
                }
                SysFreeString(app);
                SysFreeString(name);
                r->Release();
            }
            VariantClear(&v);
        }
    }
    if (it) it->Release();
    if (en) en->Release();
    if (rules) rules->Release();
    pol->Release();
}

}  // namespace tools
