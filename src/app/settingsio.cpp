#include "app/settingsio.h"

#include <windows.h>
#include <shlobj.h>

#include <ctime>

#include "core/files.h"
#include "core/text.h"

namespace settingsio {

namespace {

constexpr char kMagic[] = "MOONRADIAL-SETTINGS 1\n";

bool ReadFileAll(const std::wstring& path, std::string& out) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool WriteFileAll(const std::wstring& path, const std::string& bytes) {
    size_t cut = path.find_last_of(L'\\');
    if (cut != std::wstring::npos) SHCreateDirectoryExW(nullptr, path.substr(0, cut).c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok =
        WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::wstring AppData() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", p, MAX_PATH);
    return p;
}

}  // namespace

std::string Pack(const std::vector<Part>& parts) {
    std::string out = kMagic;
    for (const Part& p : parts) {
        out += "@" + Narrow(p.name) + "\t" + std::to_string(p.bytes.size()) + "\n";
        out += p.bytes;
        out += "\n";
    }
    return out;
}

bool Unpack(const std::string& blob, std::vector<Part>& out) {
    out.clear();
    if (blob.rfind(kMagic, 0) != 0) return false;
    size_t at = sizeof(kMagic) - 1;
    while (at < blob.size()) {
        if (blob[at] != '@') return false;
        size_t tab = blob.find('\t', at), nl = blob.find('\n', at);
        if (tab == std::string::npos || nl == std::string::npos || tab > nl) return false;
        Part p;
        p.name = Widen(blob.substr(at + 1, tab - at - 1));
        unsigned long long n = strtoull(blob.c_str() + tab + 1, nullptr, 10);
        at = nl + 1;
        if (n > blob.size() - at || blob.size() - at - n < 1 || blob[at + n] != '\n') return false;
        // имя - только из известных: чужой файл не должен писать куда попало
        if (p.name.find(L"..") != std::wstring::npos || p.name.find(L':') != std::wstring::npos) return false;
        p.bytes = blob.substr(at, (size_t)n);
        at += (size_t)n + 1;
        out.push_back(std::move(p));
    }
    return true;
}

std::vector<std::pair<std::wstring, std::wstring>> Files() {
    std::wstring data = AppData();
    return {
        {L"radial.cfg", ExeDir() + L"\\radial.cfg"},
        {L"data/boost_profiles.tsv", data + L"\\boost_profiles.tsv"},
        {L"data/mc.ini", data + L"\\mc.ini"},
        {L"tools/ignored.tsv", data + L"\\tools\\ignored.tsv"},
    };
}

bool Export(const std::wstring& file, std::wstring& msg) {
    std::vector<Part> parts;
    for (const auto& [name, path] : Files()) {
        Part p;
        p.name = name;
        if (ReadFileAll(path, p.bytes)) parts.push_back(std::move(p));
    }
    if (parts.empty()) {
        msg = L"Нечего сохранять: radial.cfg не найден.";
        return false;
    }
    bool ok = WriteFileAll(file, Pack(parts));
    msg = ok ? L"Сохранено: " + Plural((int)parts.size(), L"файл", L"файла", L"файлов") + L" в " + file
             : L"Не удалось записать " + file;
    return ok;
}

bool Import(const std::wstring& file, std::wstring& msg) {
    std::string blob;
    std::vector<Part> parts;
    if (!ReadFileAll(file, blob) || !Unpack(blob, parts) || parts.empty()) {
        msg = L"Это не файл настроек RadialMenu или он повреждён.";
        return false;
    }
    // копия нынешнего - до записи
    wchar_t stamp[32];
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wcsftime(stamp, 32, L"%Y-%m-%d_%H-%M-%S", &lt);
    std::wstring backup = ExeDir() + L"\\backups\\import-" + stamp;
    int written = 0;
    for (const auto& [name, path] : Files()) {
        std::string old;
        if (ReadFileAll(path, old)) {
            std::wstring flat = name;
            for (wchar_t& ch : flat)
                if (ch == L'/') ch = L'_';
            WriteFileAll(backup + L"\\" + flat, old);
        }
        for (const Part& p : parts)
            if (p.name == name && WriteFileAll(path, p.bytes)) ++written;
    }
    msg = written ? L"Загружено: " + Plural(written, L"файл", L"файла", L"файлов") + L". Прежние - в " + backup
                  : L"В файле нет знакомых настроек.";
    return written > 0;
}

}  // namespace settingsio
