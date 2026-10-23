#pragma once
// Подготовка к игре (Л21): при запуске игры одна карточка - что сейчас помешает: Windows ставит обновления, идёт
// тяжёлая загрузка (Steam, браузер, сеть занята), игра от батареи. Кнопка - самое полезное действие (отложить
// обновления, пауза загрузки Steam), остальное строками. Память освобождает Boost своей карточкой, здесь её нет.
// Здесь - текст и выбор действия по готовым признакам (под тестами); признаки собирает boostflow.
#include <string>
#include <vector>

namespace gameprep {

struct In {
    bool updates = false;  // TiWorker / MoUsoCoreWorker работают (ставятся обновления)
    bool steam = false;    // Steam качает (steamapps\downloading)
    int partials = 0;      // браузер качает (свежие .crdownload/.part)
    double downMbps = 0;   // приём по сети за последнюю минуту, Мбит/с
    bool battery = false;  // от батареи
    int percent = -1;      // заряд
};

struct Out {
    enum Action { NONE, OPEN_UPDATES, OPEN_STEAM_DOWNLOADS } action = NONE;
    std::wstring title, text, yes;  // yes - подпись кнопки действия (пусто - карточка без кнопок)
};

// false - мешать нечему
bool Check(const In& in, Out& out);

}  // namespace gameprep
