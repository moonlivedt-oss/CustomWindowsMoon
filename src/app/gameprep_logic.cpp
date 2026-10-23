// Подготовка к игре: что помешает и какая кнопка (без окон, под тестами).
#include "app/gameprep.h"

namespace gameprep {

bool Check(const In& in, Out& out) {
    out = {};
    std::vector<std::wstring> lines;
    if (in.updates) lines.push_back(L"Windows ставит обновления - диск и процессор заняты, бывают подвисания.");
    if (in.steam)
        lines.push_back(L"Steam качает - канал и диск заняты.");
    else if (in.partials)
        lines.push_back(L"Браузер качает - сеть занята, пинг выше.");
    else if (in.downMbps >= 20)
        lines.push_back(L"Сеть занята приёмом (" + std::to_wstring((int)in.downMbps) + L" Мбит/с) - пинг выше.");
    if (in.battery)
        lines.push_back(L"От батареи" + (in.percent >= 0 ? L" (" + std::to_wstring(in.percent) + L"%)" : L"") +
                        L": медленнее, хватит на час-полтора; с зарядкой станет «максимум».");
    if (lines.empty()) return false;
    out.title = lines.size() == 1 && in.battery ? L"Игра от батареи" : L"Перед игрой";
    for (size_t i = 0; i < lines.size(); ++i) out.text += (i ? L"\n" : L"") + lines[i];
    if (in.updates) {
        out.action = Out::OPEN_UPDATES;
        out.yes = L"Отложить обновления";
    } else if (in.steam) {
        out.action = Out::OPEN_STEAM_DOWNLOADS;
        out.yes = L"Пауза в Steam";
    }
    return true;
}

}  // namespace gameprep
