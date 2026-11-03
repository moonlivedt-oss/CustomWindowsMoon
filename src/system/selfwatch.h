#pragma once
// Своя память и дескрипторы: программа работает сутками, и утечка у себя видна только по росту за долгое время.
// Слежка раз в час пишет снимок в журнал автоматики; рост за сутки больше порога - карточка. Без окон, для тестов.
#include <windows.h>

#include <string>
#include <vector>

namespace tools {

struct SelfSample {
    ULONGLONG at = 0;              // GetTickCount64, мс
    ULONGLONG work = 0, priv = 0;  // рабочий набор и частные байты
    DWORD handles = 0, gdi = 0, user = 0;
};
SelfSample SampleSelf();
std::wstring SelfLine(const SelfSample& s);  // «своя память: рабочий набор 80 МБ, частные 60 МБ, дескрипторов 900...»

// Пороги роста за сутки: больше - похоже на утечку у себя.
constexpr ULONGLONG kSelfPrivGrow = 300ull * 1024 * 1024;
constexpr DWORD kSelfHandleGrow = 3000, kSelfGdiGrow = 1500, kSelfUserGrow = 1500;
// Снимки по времени. Нужно не меньше 23 часов наблюдений; последний снимок сравнивается с наименьшим за прошедшие
// сутки. what - что выросло и на сколько; false - роста нет или мало данных.
bool SelfLeak(const std::vector<SelfSample>& s, std::wstring& what);

}  // namespace tools
