#pragma once
#include <string>
#include <vector>

#include <windows.h>

#include "core/files.h"

struct Item {
    std::wstring name;
    std::wstring path;  // как в radial.cfg, может содержать %VAR% и {downloads}
    std::wstring icon;  // как в radial.cfg, относительный путь считается от папки программы
    std::wstring full;  // path после подстановок
    std::wstring iconFull;
    bool expand = false;
    bool files = false;
    bool dir = false;    // папка (для значка папок и входа внутрь)
    int special = 0;     // служебные секторы: 1 - следующая страница, 2 - окно (кольцо окон), 3 - действие сети
    std::wstring note;   // пояснение для карточки (действия сети)
    HWND win = nullptr;  // для special 2: какое окно вызвать
    std::vector<Item> children;

    bool HasRing() const { return expand || !children.empty(); }
};

struct Config {
    bool desktopOnly = false;
    bool skipFullscreen = true;
    double scale = 1.0;
    bool accentAuto = true;
    COLORREF accentManual = RGB(215, 38, 61);
    COLORREF accent = RGB(215, 38, 61);  // действующий цвет (из обоев или ручной)
    int maxChildren = 14;
    std::wstring theme = L"sunset";  // sunset - закатная палитра, wallpaper - тёмная с акцентом из обоев
    bool blur = true;
    bool sounds = true;
    int volume = 35;
    std::wstring folderIcon = L"icons/folder.png";  // общий значок папок, пусто - системный
    std::wstring folderIconFull;
    std::wstring vscode;    // путь к Code.exe, пусто - найти самим
    std::wstring terminal;  // wt / powershell / cmd, пусто - wt, если он есть
    std::wstring library;   // папка с картинками для редактора
    // своя палитра (тема custom)
    COLORREF customBg = RGB(27, 20, 32), customAccent = RGB(255, 122, 107), customAccent2 = RGB(232, 96, 140),
             customGold = RGB(255, 211, 110);
    bool tourDone = false;  // тур по студии уже показан
    // горячие углы: левый верхний, правый верхний, левый нижний, правый нижний;
    // значение - действие (desktop, tasks, screen_off...) или путь к программе, пусто - ничего
    std::wstring corners[4] = {L"", L"", L"desktop", L"tasks"};
    std::wstring corners2[4];  // двойной толчок в угол: то же, что corners
    // колесо мыши в углу: volume, brightness, track или пусто
    std::wstring cornerWheel[4] = {L"", L"", L"brightness", L"volume"};
    int cornerDelay = 250;  // сколько держать курсор в углу, мс
    bool media = true;      // панель «Сейчас играет» у нижнего края
    bool mediaToast = true;
    bool winFx = true;
    bool winRing = true;              // Alt + ` - кольцо открытых окон
    bool deskDouble = true;           // двойной клик по пустому столу - целое кольцо вокруг курсора
    bool batteryGlow = true;          // свечение у нижнего края: мало заряда, подключили зарядку
    int batteryLow = 15;              // с какого процента заряд считается низким
    bool snipCard = true;             // карточка после снимка области
    bool night = true;                // ночной режим: стекло темнее, звуки тише, свечение спокойнее
    int nightFrom = 23, nightTo = 7;  // часы ночного режима
    // курсор: пусто - системный, иначе цвет заливки #RRGGBB; размер в пикселях, 0 - как в системе
    std::wstring cursorColor;
    int cursorSize = 0;
    // движение: 0 - спокойно (медленнее), 1 - обычно, 2 - живо (быстрее)
    int motion = 1;
    bool windowFrame = true;     // рамка активного окна плавно загорается цветом темы
    bool windowCaptions = true;  // заголовки окон в цветах темы
    // игровой режим: всё красивое выключается, пока запущена игра
    bool gameAuto = true;
    // игры: маски путей через | (как исключения Windhawk); games_skip - что не считать игрой
    std::wstring games = L"*\\steamapps\\common\\*|*\\Epic Games\\*|*\\EA Games\\*|*\\Wuthering Waves*\\*|NFS14.exe";
    std::wstring gamesSkip = L"*\\wallpaper_engine\\*|*\\Steamworks Shared\\*";
    bool gameWinAnim = true;       // выключать анимации Windows
    bool gameTransparency = true;  // выключать прозрачность Windows
    bool gameWallpaper = true;     // ставить Wallpaper Engine на паузу
    // режим разработки: пока открыт тяжёлый редактор, ему отдаются память и видеокарта (красота остаётся)
    bool devAuto = true;
    std::wstring devApps = L"UnrealEditor.exe|UE4Editor.exe";  // маски, как у games
    bool devWallpaper = true;  // ставить Wallpaper Engine на паузу
    bool devBoost = true;      // при запуске редактора предлагать освободить память
    // «Защита» в кольце утилит: программы из этой папки (System Informer, RecoveryKit...) - пунктами кольца
    std::wstring securityDir = L"D:\\tools\\security";
    // экран блокировки
    std::wstring lockSource;  // картинка для экрана блокировки, пусто - обои
    bool lockReveal = true;  // после входа картинка блокировки плавно растворяется в стол
    bool lockPortal = true;  // переход «портал» (кольцо раскрывается в стол); иначе картинка просто тает
    bool hexGlass = true;    // сотовое стекло: углы, кольцо, заряд, волна игрового режима
    bool bootPortal = true;  // портал при входе после включения компьютера
    bool wakePortal = true;  // портал после сна и после выключения экрана из угла
    // Boost: папка со скриптами и boost.config.json (пусто - ..\Boost рядом с программой)
    std::wstring boostDir;
    bool boostOffer = true;    // при запуске игры предлагать освободить память
    bool boostRestore = true;  // после игры предлагать вернуть закрытые программы
    bool boostAfter = true;    // после игры вернуть приоритеты и ещё раз почистить память
    // сеть: сторож связи, смена выходного узла, проверка маршрута игры
    bool netWatch = true;    // следить за интернетом и предлагать починку при обрыве
    bool netExit = true;     // сообщать, когда выходной узел (страна) сменился
    bool netGame = true;     // при запуске игры проверить, идёт ли она через туннель, и пинг
    int netInterval = 20;    // раз во сколько секунд проверять связь
    // автоматика: тихая чистка раз в неделю и при нехватке места, разбор Загрузок (с вопросом),
    // режим питания по батарее и игре, самовосстановление автозапуска и настроек
    bool autoClean = true;
    bool autoDownloads = true;
    int downloadsDays = 14;  // установщики и архивы старше стольких дней - кандидаты в корзину
    bool autoPower = true;
    bool autoRepair = true;
    bool toolsDiskWarn = true;  // карточка, когда на диске мало места (меньше 10% и меньше 20 ГБ)
    int lockPattern = 1;  // узор стекла при переходе: 0 - нет, 1 - соты, 2 - осколки
    std::wstring avatarSource;  // своя картинка в середине аватара, пусто - эмблема-полумесяц
    std::vector<Item> left, right;

    // Кольцо для края; если для него ничего не задано - берётся другое.
    const std::vector<Item>& RingFor(bool leftEdge) const {
        const auto& v = leftEdge ? left : right;
        return v.empty() ? (leftEdge ? right : left) : v;
    }
};

void Resolve(Item& it);
bool LoadConfig(const std::wstring& file, Config& out);
bool SaveConfig(const std::wstring& file, const Config& cfg);
