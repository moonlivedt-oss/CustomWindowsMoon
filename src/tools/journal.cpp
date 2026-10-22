#include "tools/journal.h"

#include "core/text.h"

namespace tools {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;  // журнал пишут потоки автоматики, читает студия

std::wstring JournalFile(const std::wstring& dir, bool old) {
    return dir + (old ? L"\\journal.old.tsv" : L"\\journal.tsv");
}

std::string ReadAll(const std::wstring& file) {
    std::string s;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) s.append(buf, got);
    CloseHandle(f);
    return s;
}

}  // namespace

void JournalAdd(const std::wstring& dir, const std::wstring& when, const std::wstring& text, ULONGLONG limit,
                const std::wstring& undo) {
    std::string u = Narrow(when + L"\t" + text + (undo.empty() ? L"" : L"\t" + undo) + L"\n");
    AcquireSRWLockExclusive(&g_lock);
    std::wstring file = JournalFile(dir, false);
    HANDLE f = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        CloseHandle(f);
        if ((ULONGLONG)size.QuadPart > limit)
            MoveFileExW(file.c_str(), JournalFile(dir, true).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::vector<JournalEntry> JournalRead(const std::wstring& dir, size_t max) {
    std::vector<JournalEntry> out;
    AcquireSRWLockShared(&g_lock);
    std::string now = ReadAll(JournalFile(dir, false));
    std::string old = ReadAll(JournalFile(dir, true));
    ReleaseSRWLockShared(&g_lock);
    // новые первыми: сначала с конца свежего файла, не хватило - с конца старого
    for (const std::string* part : {&now, &old}) {
        std::wstring w = Widen(*part);
        size_t end = w.size();
        while (end > 0 && out.size() < max) {
            size_t start = w.rfind(L'\n', end - 1);
            size_t from = start == std::wstring::npos ? 0 : start + 1;
            std::wstring line = w.substr(from, end - from);
            end = start == std::wstring::npos ? 0 : start;
            size_t tab = line.find(L'\t');
            if (tab == std::wstring::npos) continue;
            size_t tab2 = line.find(L'\t', tab + 1);
            if (tab2 == std::wstring::npos)
                out.push_back({line.substr(0, tab), line.substr(tab + 1), L""});
            else
                out.push_back({line.substr(0, tab), line.substr(tab + 1, tab2 - tab - 1), line.substr(tab2 + 1)});
        }
    }
    return out;
}

std::wstring UndoPack(const std::wstring& kind, const std::vector<std::wstring>& args) {
    std::wstring s = kind;
    for (const std::wstring& a : args) {
        if (a.find_first_of(L"|\t\n\r") != std::wstring::npos) continue;  // такой путь не упаковать - пропускаем
        s += L"|" + a;
    }
    return s;
}

bool UndoParse(const std::wstring& s, Undo& u) {
    u = {};
    size_t bar = s.find(L'|');
    if (s.empty() || bar == 0 || bar == std::wstring::npos) return false;
    u.kind = s.substr(0, bar);
    while (bar != std::wstring::npos) {
        size_t next = s.find(L'|', bar + 1);
        std::wstring a = s.substr(bar + 1, next == std::wstring::npos ? std::wstring::npos : next - bar - 1);
        while (!a.empty() && a.back() == L'\r') a.pop_back();
        if (!a.empty()) u.args.push_back(a);
        bar = next;
    }
    return !u.args.empty();
}

}  // namespace tools
