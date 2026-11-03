#pragma once
// Напоминание одним жестом (Е2): действие remind - кольцо «через 10 мин / 20 мин / час / к 18:00 / к 21:00»,
// подпись - из буфера (первая строка, если там текст и не секрет), иначе «Напоминание». Время пришло - карточка с
// «Готово» и «Ещё 10 мин»; в игре и при показе экрана карточка ждёт, пока режим кончится (без звука и поверх игры
// не выезжает). Напоминания живут в %APPDATA%\MoonDesk\reminders.tsv и переживают перезапуск и сон.
// В кольце - уже стоящие напоминания: щелчок - отменить. remind_logic.cpp - чистое (под тестами).
#include <windows.h>

#include <string>
#include <vector>

struct Config;
struct Item;
class RadialMenu;

namespace remind {

void Init(HINSTANCE inst, const Config* cfg, RadialMenu* menu, bool (*quiet)());
void ShowRing();
std::vector<Item> RingItems();
void Tick();
void Shutdown();

// --- чистое ---
struct Note {
    long long when = 0;  // секунды unix
    std::wstring text;
};
std::wstring Save(const std::vector<Note>& list);
std::vector<Note> Parse(const std::wstring& tsv);
std::wstring Caption(const std::wstring& clip);  // подпись из буфера: первая строка, до 80 знаков; пусто - не текст
// ближайшие «ровные» часы для кольца: не раньше чем через 30 мин; hour - местный час сейчас, minute - минута
std::vector<int> Hours(int hour, int minute);
std::wstring Clock(long long when, long long now);  // «18:00», «завтра 09:00»

}  // namespace remind
