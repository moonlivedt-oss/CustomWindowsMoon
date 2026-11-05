// Свои exe рядом с программой (RadialTools.exe, RadialShutUp.exe): перед запуском - сверка SHA-256 с тем, что вшито в
// RadialMenu.exe при сборке (cmake/ownhash.cmake), а если сборка без него - с отметкой целостности
// (cmake/integrity.cmake). Подменённый или испорченный файл не запускается: вместо него - карточка (SetTamperHook).
#include <windows.h>
#include <bcrypt.h>

#include <shellapi.h>

#include <atomic>
#include <vector>

#include "core/files.h"
#include "core/text.h"
#include "system/tools.h"

namespace tools {

namespace {

std::atomic<void (*)(const std::wstring&)> g_hook{nullptr};

// SHA-256 по уже открытому файлу (его держим, пока не запустим)
std::wstring HandleSha256(HANDLE f) {
    std::wstring out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<unsigned char> buf(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        ok = BCryptHashData(h, buf.data(), got, 0) == 0;
    unsigned char hash[32];
    if (ok && BCryptFinishHash(h, hash, sizeof(hash), 0) == 0) {
        wchar_t hx[3];
        for (unsigned char b : hash) swprintf(hx, 3, L"%02x", b), out += hx;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::wstring RecordOf(const std::wstring& path) {
    HANDLE f =
        CreateFileW(IntegrityRecord(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    char buf[128] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    return Lower(Trim(Widen(std::string(buf, got))));
}

}  // namespace

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

OwnExe OwnExeVerdict(const std::wstring& have, const std::wstring& built, const std::wstring& record) {
    if (have.size() != 64) return OWN_UNREADABLE;
    if (built.size() == 64) return Lower(built) == have ? OWN_OK : OWN_CHANGED;
    if (record.size() == 64) return Lower(record) == have ? OWN_OK : OWN_CHANGED;
    return OWN_OK;  // сверять не с чем (сборка без отметок)
}

HANDLE OwnExeOpen(const std::wstring& name, std::wstring* why) {
    std::wstring path = ExeDir() + L"\\" + name;
    // без общего доступа на запись и удаление: пока файл открыт, его не подменить и не переименовать - то, что
    // сверили, то и запустится
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    OwnExe v = f == INVALID_HANDLE_VALUE ? OWN_UNREADABLE
                                         : OwnExeVerdict(HandleSha256(f), OwnHash(name.c_str()), RecordOf(path));
    if (v == OWN_OK) return f;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (why)
        *why = v == OWN_CHANGED ? name + L" изменён после сборки - не запускаю: файл подменили или испортили"
                                : name + L" не прочитать (нет рядом с программой или его держит антивирус)";
    if (v == OWN_CHANGED)
        if (auto hook = g_hook.load()) hook(name);
    return nullptr;
}

bool OwnExeShell(const std::wstring& name, const std::wstring& args) {
    std::wstring why;
    HANDLE lock = OwnExeOpen(name, &why);
    if (!lock) return false;
    std::wstring path = ExeDir() + L"\\" + name;
    bool ok = (INT_PTR)ShellExecuteW(nullptr, L"open", path.c_str(), args.empty() ? nullptr : args.c_str(),
                                     ExeDir().c_str(), SW_SHOWNORMAL) > 32;
    CloseHandle(lock);
    return ok;
}

void SetTamperHook(void (*hook)(const std::wstring& name)) { g_hook = hook; }

}  // namespace tools
