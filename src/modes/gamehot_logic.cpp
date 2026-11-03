// Температура в игре: когда «долго горячо» (под тестами).
#include "modes/gamehot.h"

namespace gamehot {

bool HotWatch::Feed(uint64_t nowMs, int temp, bool thermal) {
    if (said_) return false;
    bool hot = thermal || temp >= 85;
    if (!hot && temp < 80) {  // остыло - отсчёт заново
        since_ = 0;
        return false;
    }
    if (!hot) return false;  // 80-84 без торможения: отсчёт не сбрасываем, но и не копим начало
    if (!since_) since_ = nowMs ? nowMs : 1;
    if (nowMs - since_ < 5 * 60000ull) return false;
    said_ = true;
    return true;
}

}  // namespace gamehot
