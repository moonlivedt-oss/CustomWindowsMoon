#pragma once
// Перевод текста. По умолчанию - только на этом компьютере: Argos Translate через установленный Python (свой процесс,
// в нём отключена сеть; текст идёт через канал, а не через файлы и командную строку; помощник сам выходит через пять
// минут простоя). Google - лишь если пользователь включил его в настройках (translate_google): наружу уходит только
// текст и никогда картинки, а похожее на секрет не отправляется вовсе.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace translate {

enum class Via { Local, Google };
// NoLocal - нет Python с Argos, NoModel - нет модели на эту пару языков, Failed - всё остальное
enum class Problem { None, NoLocal, NoModel, Failed };

struct Result {
    uint64_t tag = 0;
    bool ok = false;
    Via via = Via::Local;
    std::vector<std::wstring> texts;  // столько же, сколько просили; пустая - не переведено
    std::wstring source, target, error;
    Problem problem = Problem::None;
    int blocked = 0;  // сколько текстов не отправлено в Google: похожи на секрет
};

struct Job {
    std::vector<std::wstring> texts;
    std::wstring source, target;
    bool forceGoogle = false;  // пользователь сам нажал «Через Google» (только если разрешено настройкой)
    uint64_t tag = 0;
    HWND wnd = nullptr;
    UINT msgDone = 0;  // lParam = new Result (окно удаляет)
    UINT msgStep =
        0;  // wParam - сколько готово (kStarting - запускается помощник), lParam - сколько всего; 0 - не слать
};
constexpr WPARAM kStarting = (WPARAM)-1;

void Start(Job job);
// Скачать модель пары с серверов Argos, текст при этом никуда не уходит. Итог - Result в msgDone
void InstallModel(const std::wstring& source, const std::wstring& target, uint64_t tag, HWND wnd, UINT msgDone);

void SetGoogleAllowed(bool on);
bool GoogleAllowed();
// Для проверок окон: вместо помощника и сети - предсказуемая подстановка «[en] текст»
void SetFake(bool on);
// Выйти: помощник закроется вместе с программой и сам, это быстрее
void Stop();

}  // namespace translate
