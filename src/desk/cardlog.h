#pragma once
// Память карточек-вопросов (Л10, Л11). Карточка с кнопками ушла без нажатия (таймер, крестик, бросок, «Не сейчас»,
// её сменила другая) - она попадает в «Пропущенные», и её кнопки можно нажать потом из студии. Вид карточки
// трижды подряд закрыт без действия - один раз спросить «больше не показывать?»; заглушённый вид не выезжает, а
// уходит сразу в пропущенные и в вечерний дайджест. Заглушённые виды - %APPDATA%\MoonDesk\cards.tsv.
// cardlog_logic.cpp - вид по заголовку, счёт закрытий, файл (под тестами); cardlog.cpp - список и файл.
#include <windows.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cardlog {

// --- логика без окон и файлов (под тестами) ---
// Вид карточки по заголовку: без чисел и того, что за ними («Освободить ~1.2 ГБ перед игрой?» -> «Освободить»),
// чтобы одна и та же карточка с разными числами считалась одной
std::wstring KindOf(const std::wstring& title);
// Закрытия без действия подряд по видам: Closed возвращает true ровно один раз - на третьем подряд; действие
// (acted) обнуляет счёт
struct Strikes {
    std::map<std::wstring, int> n;
    bool Closed(const std::wstring& kind, bool acted);
};
std::wstring MutesText(const std::set<std::wstring>& kinds);  // строка на вид
std::set<std::wstring> ParseMutes(const std::wstring& text);

// --- список и заглушки ---
struct Missed {
    int id = 0;
    std::wstring kind, title, sub, yes, no;  // подписи кнопок как на карточке
    std::function<void()> onYes, onNo;       // onNo пусто - вторая кнопка просто закрывала
    SYSTEMTIME at{};
    ULONGLONG tick = 0;
};
void Record(Missed m);       // ушла без нажатия: в начало списка (тот же вид с тем же текстом заменяется)
std::vector<Missed> List();  // новые сверху, не старше суток, до 12
int Count();
bool Press(int id, bool yes);  // нажать кнопку пропущенной карточки (и убрать её из списка)
void Forget(int id);           // убрать без действия
unsigned Gen();                // растёт при каждой правке списка (студии - перерисоваться)

// Л11: сколько закрытий подряд, заглушить, вопрос
void Closed(const std::wstring& kind, bool acted);  // карточка ушла; на третьем подряд без действия - вопрос
bool Muted(const std::wstring& kind);
void Mute(const std::wstring& kind, bool on);
std::set<std::wstring> MutedKinds();
// Как спросить «больше не показывать?» (основной поток, своя карточка) и куда класть заглушённое (дайджест: true -
// принято). Задаются один раз при старте.
void SetAsk(std::function<void(const std::wstring& kind)> ask);
void SetDigest(std::function<bool(const std::wstring& title, const std::wstring& text)> digest);
bool ToDigest(const std::wstring& title, const std::wstring& text);

}  // namespace cardlog
