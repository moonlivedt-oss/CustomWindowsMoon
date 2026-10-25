#pragma once
// Режим показа экрана: идёт трансляция или запись (OBS, показ экрана в Zoom) или включили руками - окна
// RadialMenu (кольца, карточки, студия, свечения) не попадают в захват экрана: вы их видите, зрители нет.
// Уведомления Windows на это время молчат (тишину держит своей отметкой, как игра и фокус).
#include <windows.h>

#include <functional>
#include <string>

struct Config;

namespace share {

void Init(const Config* cfg);
void Tick();    // из общего опроса; проверка раз в секунду
void Toggle();  // вручную: включить или выключить
bool On();
bool Manual();
const std::wstring& Reason();  // что включило: имя программы или «вручную»
// Как выставлять окну захват экрана: в режиме - исключить из захвата, иначе - как обычно. Своё окно, которое
// меняет это само (кольцо на время снимка под стекло), возвращает именно это значение, а не WDA_NONE.
DWORD Affinity();
void Cover(HWND w);  // только что показанное окно - сразу по режиму, не ждать опроса
void Shutdown();     // при выходе: окна и тишину вернуть
extern std::function<void(bool on)> onChange;

}  // namespace share
