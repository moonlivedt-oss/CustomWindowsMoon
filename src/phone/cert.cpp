// «Телефон рядом»: свой корневой сертификат и сертификат сервера (ECDSA P-256, SHA-256) средствами Windows -
// CNG (ncrypt) и crypt32, без OpenSSL. Корневой: ключ живёт только в памяти и в файле под DPAPI (расшифровать может
// только этот пользователь на этом ПК), сертификат - 10 лет. Сервера: ключ - в хранилище ключей пользователя
// (Schannel берёт его по имени), сертификат - 397 дней на адреса ПК в subjectAltName (Chrome смотрит только туда).
#include <windows.h>

#include <bcrypt.h>
#include <ncrypt.h>
#include <shlobj.h>

#include <array>
#include <memory>

#include "phone/cert.h"

namespace phone::cert {

namespace {

std::string ReadAll(const std::wstring& p, size_t max) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::string out;
    if (sz.QuadPart > 0 && (size_t)sz.QuadPart <= max) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) || got != out.size()) out.clear();
    }
    CloseHandle(f);
    return out;
}

bool WriteAll(const std::wstring& p, const std::string& data) {
    std::wstring tmp = p + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string Encode(LPCSTR type, const void* info) {
    DWORD n = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, info, 0, nullptr, nullptr, &n)) return {};
    std::string out(n, '\0');
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, info, 0, nullptr, (BYTE*)out.data(), &n)) return {};
    out.resize(n);
    return out;
}

struct Prov {
    NCRYPT_PROV_HANDLE h = 0;
    Prov() { NCryptOpenStorageProvider(&h, MS_KEY_STORAGE_PROVIDER, 0); }
    ~Prov() {
        if (h) NCryptFreeObject(h);
    }
};

struct Key {
    NCRYPT_KEY_HANDLE h = 0;
    ~Key() {
        if (h) NCryptFreeObject(h);
    }
};

// открытый ключ в виде CERT_PUBLIC_KEY_INFO (указатели внутри буфера)
std::string PublicInfo(NCRYPT_KEY_HANDLE k) {
    DWORD n = 0;
    if (!CryptExportPublicKeyInfo(k, 0, X509_ASN_ENCODING, nullptr, &n)) return {};
    std::string buf(n, '\0');
    if (!CryptExportPublicKeyInfo(k, 0, X509_ASN_ENCODING, (CERT_PUBLIC_KEY_INFO*)buf.data(), &n)) return {};
    return buf;
}

std::string KeyId(const CERT_PUBLIC_KEY_INFO& pk) {  // subjectKeyIdentifier: SHA-1 открытого ключа
    BYTE h[20];
    DWORD n = sizeof(h);
    if (!CryptHashCertificate(0, CALG_SHA1, 0, pk.PublicKey.pbData, pk.PublicKey.cbData, h, &n)) return {};
    return std::string((const char*)h, n);
}

std::string Name(const std::wstring& x500) {
    DWORD n = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, x500.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &n, nullptr)) return {};
    std::string out(n, '\0');
    if (!CertStrToNameW(X509_ASN_ENCODING, x500.c_str(), CERT_X500_NAME_STR, nullptr, (BYTE*)out.data(), &n, nullptr))
        return {};
    out.resize(n);
    return out;
}

FILETIME Shift(long long days) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart += (ULONGLONG)(days * 864000000000ll);
    return {u.LowPart, u.HighPart};
}

struct Ext {
    const char* oid;
    bool critical;
    std::string value;
};

// подписать сертификат ключом signer; subject и issuer - закодированные имена
std::string Build(NCRYPT_KEY_HANDLE signer, const std::string& issuer, const std::string& subject,
                  const CERT_PUBLIC_KEY_INFO& pk, long long days, std::vector<Ext> exts) {
    BYTE serial[16];
    if (BCryptGenRandom(nullptr, serial, sizeof(serial), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return {};
    serial[15] &= 0x7F;  // номер в CryptoAPI - младшим байтом вперёд: старший бит 0 - число положительное
    serial[15] |= 0x01;
    std::vector<CERT_EXTENSION> ce;
    for (Ext& e : exts) {
        if (e.value.empty()) return {};
        ce.push_back({(LPSTR)e.oid, e.critical, {(DWORD)e.value.size(), (BYTE*)e.value.data()}});
    }
    CERT_INFO ci{};
    ci.dwVersion = CERT_V3;
    ci.SerialNumber = {sizeof(serial), serial};
    ci.SignatureAlgorithm.pszObjId = (LPSTR)szOID_ECDSA_SHA256;
    ci.Issuer = {(DWORD)issuer.size(), (BYTE*)issuer.data()};
    ci.Subject = {(DWORD)subject.size(), (BYTE*)subject.data()};
    ci.NotBefore = Shift(-1);  // часы телефона могут отставать
    ci.NotAfter = Shift(days);
    ci.SubjectPublicKeyInfo = pk;
    ci.cExtension = (DWORD)ce.size();
    ci.rgExtension = ce.data();
    CRYPT_ALGORITHM_IDENTIFIER alg{(LPSTR)szOID_ECDSA_SHA256, {}};
    DWORD n = 0;
    if (!CryptSignAndEncodeCertificate(signer, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci,
                                       &alg, nullptr, nullptr, &n))
        return {};
    std::string der(n, '\0');
    if (!CryptSignAndEncodeCertificate(signer, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci,
                                       &alg, nullptr, (BYTE*)der.data(), &n))
        return {};
    der.resize(n);
    return der;
}

std::string BasicConstraints(bool ca) {
    CERT_BASIC_CONSTRAINTS2_INFO bc{ca, ca, 0};  // корневой: подписывает только сертификаты серверов (путь 0)
    return Encode(X509_BASIC_CONSTRAINTS2, &bc);
}

// Ограничение корневого (NameConstraints, критическое): годится только для адресов своей сети (10/8, 172.16/12,
// 192.168/16, 169.254/16, 127/8) и одного несуществующего имени, IPv6 - ни для какого. Ключ корневого лежит у
// пользователя под DPAPI, и вредонос на ПК мог бы выпускать им сертификаты на любые сайты; телефон, доверивший
// корневому, принял бы их. С ограничением сертификат на bank.ru браузер телефона отвергнет.
std::string NameConstraints() {
    static const BYTE kNets[][8] = {{10, 0, 0, 0, 255, 0, 0, 0},
                                    {172, 16, 0, 0, 255, 240, 0, 0},
                                    {192, 168, 0, 0, 255, 255, 0, 0},
                                    {169, 254, 0, 0, 255, 255, 0, 0},
                                    {127, 0, 0, 0, 255, 0, 0, 0}};
    static const BYTE kAnyV6[32] = {};  // ::/0
    std::vector<CERT_GENERAL_SUBTREE> allow;
    for (const auto& n : kNets) {
        CERT_GENERAL_SUBTREE t{};
        t.Base.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        t.Base.IPAddress = {8, (BYTE*)n};
        allow.push_back(t);
    }
    CERT_GENERAL_SUBTREE dns{};
    dns.Base.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    dns.Base.pwszDNSName = (LPWSTR)L"moondesk.invalid";  // имён у сервера нет: любое другое имя - вне корневого
    allow.push_back(dns);
    CERT_GENERAL_SUBTREE v6{};
    v6.Base.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
    v6.Base.IPAddress = {32, (BYTE*)kAnyV6};
    CERT_NAME_CONSTRAINTS_INFO info{(DWORD)allow.size(), allow.data(), 1, &v6};
    return Encode(X509_NAME_CONSTRAINTS, &info);
}

std::string KeyUsage(BYTE bits) {
    CRYPT_BIT_BLOB b{1, &bits, 0};
    return Encode(X509_KEY_USAGE, &b);
}

std::string OctetString(const std::string& s) {
    CRYPT_DATA_BLOB b{(DWORD)s.size(), (BYTE*)s.data()};
    return Encode(X509_OCTET_STRING, &b);
}

std::string Protect(const std::string& plain) {
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()}, out{};
    if (!CryptProtectData(&in, L"MoonDesk phone CA", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string r((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}

std::string Unprotect(const std::string& sealed) {
    DATA_BLOB in{(DWORD)sealed.size(), (BYTE*)sealed.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string r((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}

// корневой: сертификат и ключ в памяти (ключ из файла под DPAPI); false - нет или не сходится
bool LoadCa(const Prov& prov, const std::wstring& dir, std::string& der, Key& key) {
    der = ReadAll(dir + L"\\phone-ca.cer", 64 << 10);
    std::string blob = Unprotect(ReadAll(dir + L"\\phone-ca.key", 64 << 10));
    if (der.empty() || blob.empty()) return false;
    SECURITY_STATUS st =
        NCryptImportKey(prov.h, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr, &key.h, (BYTE*)blob.data(), (DWORD)blob.size(), 0);
    SecureZeroMemory(blob.data(), blob.size());
    if (st) return false;
    PCCERT_CONTEXT c = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)der.data(), (DWORD)der.size());
    if (!c) return false;
    std::string pk = PublicInfo(key.h);
    bool same = !pk.empty() && CertComparePublicKeyInfo(X509_ASN_ENCODING, &c->pCertInfo->SubjectPublicKeyInfo,
                                                        (CERT_PUBLIC_KEY_INFO*)pk.data());
    FILETIME soon = Shift(30);  // корневой на исходе - выпустить новый
    bool fresh = CompareFileTime(&c->pCertInfo->NotAfter, &soon) > 0;
    CertFreeCertificateContext(c);
    return same && fresh;
}

bool CreateCa(const Prov& prov, const std::wstring& dir, std::string& der) {
    Key k;
    if (NCryptCreatePersistedKey(prov.h, &k.h, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0, 0)) return false;
    DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    NCryptSetProperty(k.h, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE*)&policy, sizeof(policy), 0);
    if (NCryptFinalizeKey(k.h, 0)) return false;
    std::string pkBuf = PublicInfo(k.h);
    if (pkBuf.empty()) return false;
    const CERT_PUBLIC_KEY_INFO& pk = *(CERT_PUBLIC_KEY_INFO*)pkBuf.data();
    wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = L"ПК";
    DWORD pcn = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(pc, &pcn);
    std::wstring cn = L"MoonDesk " + std::wstring(pc);
    std::string name = Name(L"CN=\"" + cn + L"\", O=MoonDesk");
    der = Build(k.h, name, name, pk, 3650,
                {{szOID_BASIC_CONSTRAINTS2, true, BasicConstraints(true)},
                 {szOID_KEY_USAGE, true, KeyUsage(CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE)},
                 {szOID_SUBJECT_KEY_IDENTIFIER, false, OctetString(KeyId(pk))},
                 {szOID_NAME_CONSTRAINTS, true, NameConstraints()}});
    if (der.empty()) return false;
    DWORD n = 0;
    if (NCryptExportKey(k.h, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr, nullptr, 0, &n, 0)) return false;
    std::string blob(n, '\0');
    if (NCryptExportKey(k.h, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr, (BYTE*)blob.data(), n, &n, 0)) return false;
    std::string sealed = Protect(blob);
    SecureZeroMemory(blob.data(), blob.size());
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return !sealed.empty() && WriteAll(dir + L"\\phone-ca.key", sealed) && WriteAll(dir + L"\\phone-ca.cer", der);
}

}  // namespace

std::wstring DefaultDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir + L"\\MoonDesk";
}

bool LoadOrCreateCa(const std::wstring& dir, std::string& der) {
    Prov prov;
    if (!prov.h) return false;
    Key k;
    if (LoadCa(prov, dir, der, k)) return true;
    return CreateCa(prov, dir, der);
}

PCCERT_CONTEXT IssueServer(const std::wstring& dir, const std::vector<uint32_t>& ips, const std::wstring& keyName) {
    Prov prov;
    std::string caDer;
    Key ca;
    if (!prov.h || ips.empty()) return nullptr;
    if (!LoadCa(prov, dir, caDer, ca)) {
        if (!CreateCa(prov, dir, caDer)) return nullptr;
        ca.h = 0;
        if (!LoadCa(prov, dir, caDer, ca)) return nullptr;
    }
    PCCERT_CONTEXT caCert =
        CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)caDer.data(), (DWORD)caDer.size());
    if (!caCert) return nullptr;
    std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> caHold(caCert,
                                                                                      &CertFreeCertificateContext);
    std::string caKeyId = KeyId(caCert->pCertInfo->SubjectPublicKeyInfo);
    // ключ сервера: тот же между запусками, если уже есть
    Key sk;
    if (NCryptOpenKey(prov.h, &sk.h, keyName.c_str(), 0, 0)) {
        sk.h = 0;
        if (NCryptCreatePersistedKey(prov.h, &sk.h, BCRYPT_ECDSA_P256_ALGORITHM, keyName.c_str(), 0,
                                     NCRYPT_OVERWRITE_KEY_FLAG) ||
            NCryptFinalizeKey(sk.h, 0))
            return nullptr;
    }
    std::string pkBuf = PublicInfo(sk.h);
    if (pkBuf.empty()) return nullptr;
    const CERT_PUBLIC_KEY_INFO& pk = *(CERT_PUBLIC_KEY_INFO*)pkBuf.data();
    std::vector<std::array<BYTE, 4>> raw;
    for (uint32_t ip : ips) raw.push_back({(BYTE)(ip >> 24), (BYTE)(ip >> 16), (BYTE)(ip >> 8), (BYTE)ip});
    std::vector<CERT_ALT_NAME_ENTRY> alt(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        alt[i].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        alt[i].IPAddress = {4, raw[i].data()};
    }
    CERT_ALT_NAME_INFO an{(DWORD)alt.size(), alt.data()};
    LPSTR serverAuth = (LPSTR)szOID_PKIX_KP_SERVER_AUTH;
    CERT_ENHKEY_USAGE eku{1, &serverAuth};
    CERT_AUTHORITY_KEY_ID2_INFO aki{};
    aki.KeyId = {(DWORD)caKeyId.size(), (BYTE*)caKeyId.data()};
    wchar_t first[32];
    swprintf(first, 32, L"CN=%u.%u.%u.%u", ips[0] >> 24, (ips[0] >> 16) & 255, (ips[0] >> 8) & 255, ips[0] & 255);
    CERT_NAME_BLOB issuer = caCert->pCertInfo->Subject;
    std::string der = Build(ca.h, std::string((const char*)issuer.pbData, issuer.cbData), Name(first), pk, 397,
                            {{szOID_BASIC_CONSTRAINTS2, true, BasicConstraints(false)},
                             {szOID_KEY_USAGE, true, KeyUsage(CERT_DIGITAL_SIGNATURE_KEY_USAGE)},
                             {szOID_ENHANCED_KEY_USAGE, false, Encode(X509_ENHANCED_KEY_USAGE, &eku)},
                             {szOID_SUBJECT_ALT_NAME2, false, Encode(X509_ALTERNATE_NAME, &an)},
                             {szOID_AUTHORITY_KEY_IDENTIFIER2, false, Encode(X509_AUTHORITY_KEY_ID2, &aki)},
                             {szOID_SUBJECT_KEY_IDENTIFIER, false, OctetString(KeyId(pk))}});
    if (der.empty()) return nullptr;
    PCCERT_CONTEXT c = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)der.data(), (DWORD)der.size());
    if (!c) return nullptr;
    CRYPT_KEY_PROV_INFO kp{};
    kp.pwszContainerName = (LPWSTR)keyName.c_str();
    kp.pwszProvName = (LPWSTR)MS_KEY_STORAGE_PROVIDER;
    if (!CertSetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, 0, &kp)) {
        CertFreeCertificateContext(c);
        return nullptr;
    }
    return c;
}

void DeleteKey(const std::wstring& keyName) {
    Prov prov;
    Key k;
    if (prov.h && !NCryptOpenKey(prov.h, &k.h, keyName.c_str(), 0, 0) && !NCryptDeleteKey(k.h, 0)) k.h = 0;
}

}  // namespace phone::cert
