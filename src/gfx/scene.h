#pragma once
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

}  // namespace scene
