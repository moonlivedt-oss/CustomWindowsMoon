#pragma once
// Разметка снимка без окна и рисования (под тестами): фигуры в координатах картинки, история правок, обрезка,
// выбор и перетаскивание фигур, выпрямление линий, поиск секретов среди распознанных слов. Окно - helpers/markup.
#include <cstdint>
#include <string>
#include <vector>

namespace markup {

// Порядок значений - ключ для запомненного инструмента в реестре: новые только в конец
enum class Tool { Arrow, Box, Marker, Label, Blur, Step, Select, Crop, Ocr, Translation };

struct Pt {
    float x = 0, y = 0;
};

struct Rc {
    float x = 0, y = 0, w = 0, h = 0;
    bool Empty() const { return w <= 0 || h <= 0; }
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(Pt p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
};

struct Shape {
    Tool tool = Tool::Arrow;
    std::vector<Pt> pts;  // стрелка, рамка, размытие - два угла; маркер - весь путь; подпись - начало; цифра - центр
    uint32_t rgb = 0xE5484D;
    float width = 3;  // толщина линии, у подписи - размер шрифта, у цифры - диаметр; в пикселях картинки
    std::wstring text;
    Pt extent;  // размер подписи на снимке: его знает только рисование
    // Перевод поверх снимка: rgb - цвет фона под текстом, width - размер шрифта, text - перевод, pts - два угла.
    // Исходный текст закрыт заливкой целиком: в готовой картинке его нет
    uint32_t ink = 0xFFFFFF;
    int align = 0;  // 0 - влево, 1 - по центру, 2 - вправо
};

// Случайный щелчок вместо рисования фигурой не считается: и в отмене его быть не должно
bool Meaningful(const Shape& s);

// История: каждая правка - новое состояние, поэтому отмена работает и для перемещения, и для обрезки
class Doc {
public:
    void Add(Shape s);
    void Replace(size_t index, Shape s);
    void Remove(size_t index);
    void SetCrop(Rc crop);
    void SetShapes(std::vector<Shape> shapes);  // всё разом одной правкой: перевод снимка отменяется одним шагом
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    bool Empty() const { return now_.shapes.empty() && now_.crop.Empty(); }  // правок нет: снимок как был
    const std::vector<Shape>& Shapes() const { return now_.shapes; }
    const Rc& Crop() const { return now_.crop; }

private:
    struct State {
        std::vector<Shape> shapes;
        Rc crop;
    };
    void Remember();
    State now_;
    std::vector<State> undo_, redo_;
};

// Видимая часть картинки: обрезка или вся
Rc View(const Rc& crop, int imageW, int imageH);

// Где картинка в окне: целиком и по центру области. maxScale - насколько можно увеличить мелкую
struct Fit {
    float scale = 1, x = 0, y = 0, w = 0, h = 0;
};
Fit FitImage(int imageW, int imageH, float areaX, float areaY, float areaW, float areaH, float maxScale = 1);
// Крошечный снимок в окне разметки не разглядеть и не разметить: целое увеличение, чтобы пиксели оставались чёткими
float EditZoom(int imageW, int imageH);
// Точка окна -> точка картинки; за краем видимой части прижимается к краю, чтобы фигуру можно было довести до него
Pt ToImage(const Fit& f, float windowX, float windowY, const Rc& view);
Pt ToWindow(const Fit& f, Pt image, const Rc& view);

// Стрелка: out[0] - где линия упирается в наконечник, out[1] - острие, out[2], out[3] - его крылья
void ArrowGeometry(Pt from, Pt to, float width, Pt out[4]);

// Размытие для скрытия: клетки одного цвета. Настоящее размытие можно частично обратить, а из усреднённых клеток
// исходный текст не восстановить. pixels - 32 бита на пиксель, stride - в пикселях.
int BlurCell(int rectW, int rectH);
void Pixelate(uint32_t* pixels, int imageW, int imageH, int stride, Pt a, Pt b);

// Выбор фигур. tolerance - запас вокруг линий, в пикселях картинки
Rc Bounds(const Shape& s);
bool HitTest(const Shape& s, Pt p, float tolerance);
int TopShape(const std::vector<Shape>& shapes, Pt p, float tolerance);  // верхняя под точкой, -1 - нет
// Ручки: концы стрелки, четыре угла рамки, размытия и маркера, один угол подписи и цифры
std::vector<Pt> Handles(const Shape& s);
int HandleAt(const Shape& s, Pt p, float tolerance);
void DragHandle(Shape& s, int handle, Pt to);
void Translate(Shape& s, float dx, float dy);

// Выпрямление без клавиш: почти горизонтальную или вертикальную линию довести до ровной
void SnapAxis(Pt a, Pt& b);
// Маркер провели почти по прямой (подчеркнули строку): заменить путь прямым отрезком. true - заменён
bool StraightenStroke(std::vector<Pt>& pts);

// Светлый цвет: на нём цифра должна быть тёмной
bool IsLight(uint32_t rgb);

// Что на снимке похоже на секрет. Слово с признаками токена закрывается само по себе; номер карты и другие секреты
// из нескольких слов находятся по строке, и закрывается вся строка. Рамки с запасом pad, пересечения склеены
struct OcrWord {
    std::wstring text;
    Rc box;
    int line = 0;
};
std::vector<Rc> SecretBoxes(const std::vector<OcrWord>& words, float pad);

// Сколько места нужно тексту при шрифте size и ширине wrapW: считает рисование
using MeasureFn = Pt (*)(const std::wstring& text, float size, float wrapW, void* ctx);
struct TextFit {
    float size = 0, w = 0, h = 0;
};
// Перевод в рамку на месте оригинала: шрифт от высоты строки, рамка шире (до maxW) для одной строки, иначе шрифт
// меньше; не влез и с мелким - рамка выше
TextFit FitText(const std::wstring& text, float boxW, float boxH, float lineH, float maxW, MeasureFn measure, void* ctx);

// Докуда можно расширить однострочную подпись вправо: до соседнего текста на той же высоте или до края
float RoomRight(const Rc& box, const std::vector<Rc>& others, float limit);

// Лента снимков: кого убрать, чтобы освободить место. Самый старый без звезды; все со звездой - самый старый
int EvictIndex(const std::vector<bool>& keep);

}  // namespace markup
