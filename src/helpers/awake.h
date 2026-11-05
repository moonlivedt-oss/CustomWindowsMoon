#pragma once
// «Не спать» на время (Ж6): действие угла или пункта awake - кольцо: час, два, четыре, до конца загрузки, пока не
// выключу. Пока включено, компьютер не засыпает (экран гаснуть может), в трее свой значок с остатком; щелчок по
// нему - то же кольцо. «До конца загрузки» - пока в Загрузках есть недокачанные файлы браузеров и торрентов или
// приём по сети больше 150 КБ/с (Steam, Epic, обновления); 5 минут тихо - сон снова разрешён. Ничего не качается
// 10 минут с начала - тоже. Не дольше 12 часов. awake_logic.cpp - чистое, под тестами.
#include <windows.h>

#include <string>
#include <vector>

struct Config;
struct Item;
class RadialMenu;

namespace awake {

void Init(HINSTANCE inst, const Config* cfg, RadialMenu* menu);
void ShowRing();
std::vector<Item> RingItems();
void Tick();  // из общего опроса
void Stop();
bool On();
std::wstring Status();  // «Не сплю: ещё 47 мин» / «Не сплю до конца загрузки» / пусто
void Shutdown();

// --- чистое ---
enum Kind { OFF, TIMED, DOWNLOAD, FOREVER };
bool PartialDownload(const std::wstring& name);  // .crdownload, .part, .opdownload, .!ut...
// конец «до конца загрузки»: busy - качается ли сейчас; true - пора разрешить сон
class DoneWatch {
public:
    void Start(ULONGLONG now) { start_ = now, lastBusy_ = 0, seenBusy_ = false; }
    bool Feed(ULONGLONG now, bool busy);
    bool SeenBusy() const { return seenBusy_; }

private:
    ULONGLONG start_ = 0, lastBusy_ = 0;
    bool seenBusy_ = false;
};
std::wstring Left(ULONGLONG ms);  // «ещё 1 ч 05 мин», «ещё 47 мин», «ещё меньше минуты»

}  // namespace awake
