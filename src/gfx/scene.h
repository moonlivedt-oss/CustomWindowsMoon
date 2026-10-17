#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// Живые сцены утилит: частицы на пружинах, вихрь очистки, щит проверки.
// Сцена рисует себя в любой Graphics (студия, карточка справа снизу) и отвечает, движется ли ещё:
// пока движется - вызывающий просит следующий кадр, замерла - кадров нет. В игровом режиме сразу итог.
// Что показывать, сцены берут из «ленты» (CleanBegin/GuardBegin...): её пишет тот, кто чистит или проверяет,
// из любого потока, а смотрят и студия, и карточка.
namespace scene {

using Gdiplus::Color;
using Gdiplus::PointF;
using Gdiplus::RectF;

struct Palette {
    Color text, muted, faint, accent, accent2, gold, card;
};

// ---------------- частицы ----------------
struct Dot {
    float x = 0, y = 0, vx = 0, vy = 0;  // где и скорость
    float tx = 0, ty = 0;                // куда тянет пружина
    float px = 0, py = 0;                // где была кадр назад (хвост)
    float delay = 0;                     // когда тронется, секунды от начала
    float a = 0, r = 0;                  // место в вихре: угол и радиус
    int row = 0;                         // чья (строка, группа)
    BYTE col = 0, state = 0;
};
// пружина к (tx, ty): жёсткость и затухание в долях за секунду
void Spring(Dot& d, float dt, float stiff = 90, float damp = 12);
// Квадратики, собранные по цветам: на цвет - один FillRectangles.
class Batch {
public:
    void Add(int col, float x, float y, float size);
    void Flush(Gdiplus::Graphics& g, const Color* cols, int n, BYTE alpha = 255);

private:
    std::vector<RectF> rects_[8];
};
// подпись шрифтом Segoe UI, размер в пикселях
void Label(Gdiplus::Graphics& g, const std::wstring& s, const RectF& r, Color c, float px, bool bold = false,
           int align = 0);  // 0 - слева, 1 - по середине, 2 - справа
float Ease(float t);        // 0..1, плавный выход

// ---------------- лента очистки ----------------
struct CleanItem {
    std::wstring name;
    ULONGLONG bytes = 0;  // сколько было по последнему подсчёту (0 - неизвестно)
    ULONGLONG freed = 0;  // сколько освободилось на деле
    int state = 0;        // 0 - ждёт, 1 - чистится, 2 - готово
};
struct CleanFeed {
    int gen = 0;  // номер чистки: сменился - сцена собирается заново
    bool active = false, done = false;
    ULONGLONG doneAt = 0;
    std::vector<CleanItem> items;
};
void CleanBegin(const std::vector<CleanItem>& items);
void CleanMark(size_t i, int state, ULONGLONG freed = 0);
void CleanEnd();
CleanFeed Clean();
bool CleanRecent(ULONGLONG ms = 12000);  // идёт или закончилась недавно - сцену стоит показывать

// ---------------- лента проверки «Защиты» ----------------
struct GuardFind {
    std::wstring name, why;
    int score = 0;
};
struct GuardFeed {
    int gen = 0;
    bool active = false, done = false;
    ULONGLONG doneAt = 0;
    int expect = 0, checked = 0;  // сколько программ ждём / проверено на деле
    std::vector<GuardFind> finds;
};
void GuardBegin(int expect);
void GuardEnd(int checked, const std::vector<GuardFind>& finds);
GuardFeed Guard();

// ---------------- сцены ----------------
// Очистка: строки - полоски из частиц; та, что чистится, осыпается с конца, частицы закручиваются в вихрь,
// счётчик освобождённого растёт; в конце вихрь схлопывается, искры собираются в галочку.
class CleanScene {
public:
    // r - где рисовать, k - масштаб; mouse - курсор в тех же координатах; click - щелчок в этом кадре.
    // true - сцена движется, нужен следующий кадр
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, POINT mouse, bool click);
    static float Height(size_t items, float k);  // сколько места просит
    bool Settled() const { return settled_; }

private:
    struct Row {
        std::wstring name;
        ULONGLONG bytes = 0, freed = 0;
        int state = 0, first = 0, count = 0, released = 0, absorbed = 0;
        double since = -1;  // когда началась чистка строки
        float y = 0, w = 0;
    };
    void Build(const CleanFeed& f);
    void Burst();
    int gen_ = -1;
    RectF rect_{};
    float k_ = 0;
    std::vector<Row> rows_;
    std::vector<Dot> dots_, sparks_;
    std::vector<int> map_;  // строка ленты -> строка сцены (лишние сливаются в «ещё»)
    ULONGLONG last_ = 0;
    double t_ = 0, finAt_ = -1, burstAt_ = -1;
    float spin_ = 0, pulse_ = 0, cx_ = 0, cy_ = 0, vr_ = 0;
    float x0_ = 0, w0_ = 0;  // где строки: начало и ширина
    bool settled_ = false;
};

// Проверка: программы - точки, по ним бежит луч; потом чистые слетаются в щит (в нём проступает галочка
// или «!»), подозрительные краснеют и улетают к своим строкам под щитом.
class GuardScene {
public:
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, POINT mouse);
    static float Height(size_t finds, float k);
    bool Settled() const { return settled_; }

private:
    void Build(const GuardFeed& f);
    void Land(const GuardFeed& f);
    int gen_ = -1;
    bool landed_ = false, settled_ = false;
    RectF rect_{};
    float k_ = 0;
    std::vector<Dot> dots_;
    std::vector<int> bad_;  // номера точек-находок
    std::vector<GuardFind> finds_;
    int checked_ = 0;
    ULONGLONG last_ = 0;
    double t_ = 0, landAt_ = -1;
    float gx_ = 0, gy_ = 0, gw_ = 0, gh_ = 0, pitch_ = 0;  // сетка точек
    float sx_ = 0, sy_ = 0, sw_ = 0, sh_ = 0;              // щит: середина и полуразмеры
};

// ---------------- сцены с мышью: сеть, порты, автозагрузка, память, батарея, версии ----------------
// Студия даёт курсор и кнопку; сцена сама ловит нажатие, отпускание и щелчок. Опасное (завершить процесс,
// выключить автозапуск) сцена не делает: отдаёт запрос через Take...(), решает и делает вызывающий.
// Каждая играет коротко (Wake) и замирает; дальше кадры только от мыши. В игровом режиме - сразу итог.
struct Input {
    POINT m{-9999, -9999};  // курсор в координатах холста; не над сценой - далеко
    bool down = false;      // левая кнопка нажата
};
float Rnd(float a, float b);
Color Pale(const Palette& p);  // светлый в тон темы: пакеты, искры, штекеры
Color Deep(const Palette& p);  // тёмное дно колбы, гнёзда
void FillRound(Gdiplus::Graphics& g, const RectF& r, float radius, Color c);
void StrokeRound(Gdiplus::Graphics& g, const RectF& r, float radius, Color c, float width);
float Measure(Gdiplus::Graphics& g, const std::wstring& s, float px, bool bold = false);  // ширина подписи

class Live {
protected:
    float Step(const RectF& r, float k, const Input& in);  // шаг кадра, с; заодно курсор и фронты кнопки
    void Wake(double sec) { awake_ = std::max(awake_, t_ + sec); }
    bool Awake() const { return !game_ && (t_ < awake_ || over_ || down_); }
    bool In(const RectF& r) const { return mx_ >= r.X && mx_ <= r.X + r.Width && my_ >= r.Y && my_ <= r.Y + r.Height; }
    RectF r_{};
    float k_ = 1;
    double t_ = 0, awake_ = 0;
    ULONGLONG last_ = 0;
    bool game_ = false;
    float mx_ = -1e4f, my_ = -1e4f, px_ = 0, py_ = 0;  // курсор и где нажали - в координатах сцены
    bool over_ = false, down_ = false, pressed_ = false, released_ = false, click_ = false;
};

// Сеть: узлы на дугах, по ним бегут пакеты (квадрат - запрос, ромб - DNS); звено краснеет и толстеет от
// задержки, при наведении - столбики последних замеров; сменился выход туннеля - узел переворачивается;
// обрыв - звено рвётся, концы искрят, запросы копятся у разрыва; починка - концы свариваются, очередь уходит.
struct RouteNode {
    std::wstring name, sub, tag;  // «Роутер», «192.168.1.1», «Р»
    bool ok = true;               // первый неисправный узел - звено перед ним порвано
};
struct RouteData {
    std::vector<RouteNode> nodes;          // 2..5 узлов слева направо
    std::vector<std::vector<float>> hist;  // по звеньям: замеры в мс (-1 - нет), последний - сейчас
    float every = 1;                       // через сколько секунд замеры
    int tunnel = -1;                       // узел-выход туннеля: его тег - код страны
    bool fixing = false, checking = false;
    int gen = 0;  // новые замеры или итог проверки - сцена оживает
};
class RouteScene : public Live {
public:
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, const Input& in, const RouteData& d);
    bool TakeFix();  // нажали «Починить»
    static float Height(float k) { return 214 * k; }

private:
    struct Packet {
        int seg = 0;
        float u = 0, jx = 0, jy = 0;
        bool dns = false, wait = false, rush = false;
    };
    struct Node {
        float x = 0, y = 0, flash = 0, flip = -1;
        std::wstring tag;
        bool swapped = false;
    };
    PointF On(int seg, float u) const;
    std::vector<Node> nodes_;
    std::vector<Packet> packets_;
    std::vector<Dot> sparks_;
    int gen_ = -1, broken_ = -1, weldLink_ = -1, hotLink_ = -1, hotNode_ = -1;
    float spawn_ = 0, weld_ = -1, pulse_ = -1;
    bool fix_ = false, fixing_ = false, fresh_ = false, wasChecking_ = false;
    std::vector<float> seen_;  // последние замеры звеньев, на которые сцена уже откликнулась
    RectF btn_{};
};

// Порты: гнездо слева, программа справа, между ними провисший кабель (точки на связях); по кабелю бежит ток -
// чем больше соединений, тем быстрее. Штекер тянется мышью: далеко отпустил - запрос «завершить», рядом -
// защёлкнулся. Сервер, который больше 6 часов без соединений, - серый кабель в пыли.
struct PortData {
    int port = 0;
    DWORD pid = 0;
    std::wstring proc, proj;
    int minutes = -1, conns = 0;  // сколько работает и сколько соединений сейчас
};
class PortsScene : public Live {
public:
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, const Input& in,
              const std::vector<PortData>& rows);
    bool TakeKill(PortData& out);
    static float Height(size_t n, float k) { return (std::max<size_t>(n, 1) * 52 + 34) * k; }

private:
    struct Row {
        PortData d;
        std::vector<Dot> pts;  // x, y - сейчас; px, py - кадр назад
        float y = 0, vy = 0, ty = 0, flow = 0, snap = 0, gone = 0, led = 1;
        float still = 0, sx = 0, cx = 0;  // сколько кабель висит без дела; концы, при которых он успокоился
        bool cut = false, seen = false;
    };
    void Rope(Row& w, float dt, float sx, float cx);
    std::vector<Row> rows_;
    int hot_ = -1, drag_ = -1;
    std::vector<PortData> kill_;
    std::wstring toast_;
    float toastT_ = 0;
};

// Автозагрузка: значки программ висят на дорожке входа, как подвески на гирлянде; тяжёлые прогибают её.
// «Повторить вход» - луч проходит по дорожке, значки вспыхивают; щелчок по значку - он спрыгивает в «выключено».
struct StartData {
    std::wstring name;
    float sec = 1;        // сколько добавляет ко входу (оценка, если честного нет)
    ULONGLONG bytes = 0;  // память
    bool on = true, canToggle = true;
    Gdiplus::Bitmap* icon = nullptr;  // настоящий значок из exe
};
class StartScene : public Live {
public:
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, const Input& in,
              const std::vector<StartData>& items, bool estimated);
    bool TakeToggle(size_t& index, bool& on);
    static float Height(float k) { return 300 * k; }

private:
    struct Bead {
        std::wstring name;
        float x = 0, y = 0, vx = 0, vy = 0, tx = 0, ty = 0, s = 0, lit = 1, pop = 0, hang = 1, sw = 0, swv = 0;
        float tier = 0, tierShown = 0, sec = 0;
        ULONGLONG bytes = 0;
        bool on = true, air = false, seen = false;
    };
    float Sag(float x) const;
    void Layout();
    std::vector<Bead> beads_;
    int hot_ = -1, lastHot_ = -1;
    float replay_ = -1, est_ = 0, shown_ = -1;
    std::vector<std::pair<size_t, bool>> toggles_;
    std::wstring shake_;
    float shakeT_ = 0;
};

// Память: колба, программы - слои жидкости снизу вверх (система - на дне); выше пунктира «предел» лишнее
// переливается через носик в барабан «подкачка»; у растущей программы над колбой кран и капли.
// Щелчок по слою - запрос закрыть, слой выкипает; потянуть колбу вбок - жидкость плеснёт.
struct MemLayer {
    std::wstring name;
    ULONGLONG bytes = 0;
    bool leak = false, system = false;
};
struct MemData {
    std::vector<MemLayer> layers;   // снизу вверх
    ULONGLONG total = 0, swap = 0;  // вся память и сколько лежит в подкачке
};
class FlaskScene : public Live {
public:
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, const Input& in, const MemData& d);
    bool TakeClose(std::wstring& name);
    static float Height(float k) { return 300 * k; }

private:
    struct Layer {
        std::wstring name;
        float gb = 0, h = -1, top = 0, bot = 0, hh = 0, boil = -1;
        bool leak = false, system = false, seen = false;
        BYTE col = 0;
    };
    std::vector<Layer> layers_;
    std::vector<Dot> bubbles_, drops_;
    float tilt_ = 0, tv_ = 0, pf_ = -1, dragX_ = 0;
    bool drag_ = false;
    int hot_ = -1;
    std::vector<std::wstring> close_;
};

// Батарея: заряд течёт по шине и веткам-проводам к фоновым программам; толщина и импульсы - нагрузка.
// Щелчок по ветке - рубильник размыкается с искрой (запрос остановить), метка «сядет в» уезжает вправо.
struct BatBranch {
    std::wstring name, why;
    float cpu = 0;  // доля процессора, %
};
struct BatData {
    bool present = false, ac = true, charging = false;
    int percent = -1, minutesLeft = -1;
    std::vector<BatBranch> branches;
    std::wstring empty;  // что сказать, если веток нет
};
class BatteryScene : public Live {
public:
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, const Input& in, const BatData& d);
    bool TakeStop(std::wstring& name);
    static float Height(float k) { return 300 * k; }

private:
    struct Branch {
        BatBranch b;
        float y = 0, blade = 0, acc = 0;
        bool on = true, seen = false;
    };
    struct Pulse {
        int br = -1;  // -1 - заряд от сети
        float u = 0, v = 0;
    };
    std::vector<Branch> br_;
    std::vector<Pulse> pulses_;
    std::vector<Dot> sparks_;
    float shownH_ = -1;
    int hot_ = -1;
    std::vector<std::wstring> stop_;
};

// Версии: табло перещёлкивается со старой версии на новую, за ним поезд точек от своей до последней;
// у ветки на исходе поддержки мигает «поддержка до ...». Щелчок по строке - перещёлкнуть заново.
struct VerRow {
    std::wstring name, from, to;      // to пусто - свежая
    std::vector<std::wstring> train;  // ветки от своей до последней
    std::wstring eol;                 // «поддержка до 10.2026», пусто - не скоро
};
class VersScene : public Live {
public:
    bool Draw(Gdiplus::Graphics& g, const RectF& r, float k, const Palette& p, const Input& in,
              const std::vector<VerRow>& rows);
    static float Height(size_t n, float k) { return (std::max<size_t>(n, 1) * 56 + 30) * k; }

private:
    std::wstring key_;
    std::vector<double> at_;
    int hot_ = -1;
};

}  // namespace scene
