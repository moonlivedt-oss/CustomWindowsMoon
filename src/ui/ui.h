#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "gfx/icons.h"
#include "core/themes.h"

// Маленький набор элементов интерфейса для студии, в духе immediate mode:
// каждый кадр страница заново рисует себя и заодно узнаёт, что нажато.
// Всё рисуется GDI+, кроме полей ввода: это настоящие EDIT поверх картинки.
namespace ui {

using Gdiplus::Color;
using Gdiplus::RectF;

enum class Kind { Primary, Secondary, Ghost, Danger };
enum Align { Left, Center, Right };

class Ui {
public:
    void Attach(HWND hwnd) { hwnd_ = hwnd; }
    HWND Hwnd() const { return hwnd_; }

    // ввод из оконной процедуры
    void OnMouseMove(int x, int y);
    void OnDown(int x, int y);
    void OnUp(int x, int y);
    void OnWheel(int x, int y, int delta);
    void OnLeave();
    void OnRightDown(int x, int y);
    void OnRightUp(int x, int y);
    void OnDouble(int x, int y);
    bool OnEditChange(HWND edit);
    bool OnSetCursor();

    // кадр
    void Begin(Gdiplus::Graphics& g, const UiColors& c, double k);
    void End();
    void Invalidate() { repaint_ = true; }
    bool TakeRepaint() {
        bool r = repaint_;
        repaint_ = false;
        return r;
    }
    void SetScale(double k);

    float S(float v) const { return (float)(v * k_); }
    double K() const { return k_; }
    const UiColors& C() const { return c_; }
    Gdiplus::Graphics& G() { return *g_; }
    bool Scaled() const;  // в кадре масштаб или поворот: картинки - со сглаживанием, не один к одному
    bool interactive = true;  // false - под открытым окном поверх (тур, библиотека)

    // попадания
    bool Hover(int id, const RectF& r);
    bool Click(int id, const RectF& r);
    bool Hot(int id) const { return hot_ == id; }
    bool Active(int id) const { return active_ == id; }
    bool Released(int id);                    // кнопку отпустили, а нажата она была на id (для перетаскивания)
    bool RightClick(int id, const RectF& r);  // правая кнопка отпущена над id
    bool DoubleClick(int id);                 // двойной клик по id (Hover/Click уже зарегистрировали рамку)
    bool HotChanged();                        // наведение сменилось с прошлого вопроса (для таймера подсказок)
    int HotId() const { return hot_; }
    bool Down() const { return down_; }
    POINT Mouse() const { return mouse_; }
    POINT PressPt() const { return press_; }
    bool Dragging(int id) const;  // нажали на id и уже тянут
    // живая сцена под курсором: каждое движение мыши над id - новый кадр (наведение, перетаскивание)
    void Track(int id) { track_.push_back(id); }
    float Scroll(int id, const RectF& view, float content);
    void ScrollTo(int id, float v) { scroll_[id] = v; }  // плавно прокрутить (Scroll ограничит границами)
    void ResetScroll(int id) {
        scroll_[id] = 0;
        scrollShown_[id] = 0;
    }

    // рисование
    void Fill(const RectF& r, Color c, float radius = 0);
    void Stroke(const RectF& r, Color c, float radius = 0, float width = 1, bool dashed = false);
    void Text(const std::wstring& s, const RectF& r, Color c, float size, bool bold = false, Align h = Left,
              bool wrap = false);
    float Measure(const std::wstring& s, float size, bool bold = false);
    float MeasureWrapped(const std::wstring& s, float size, bool bold, float width);
    void Icon(const std::wstring& file, const RectF& r, const std::wstring& shellPath = L"");
    Gdiplus::Bitmap* IconBitmap(const std::wstring& shellPath, int px) { return icons_.Get(L"", shellPath, px); }
    // значок из файла или системный, готовый размер px - для рисования с масштабом и поворотом
    Gdiplus::Bitmap* IconBitmap(const std::wstring& file, const std::wstring& shellPath, int px) {
        return icons_.Get(file, shellPath, px);
    }
    // alpha < 255 - картинка полупрозрачная (растворение одной в другую)
    void Picture(Gdiplus::Bitmap* b, const RectF& r, float radius, bool roundBottom = true, BYTE alpha = 255);
    Gdiplus::GraphicsPath* Round(const RectF& r, float radius, bool top = true, bool bottom = true);

    // плавность: значение тянется к on (0..1), пока не дотянется - кадры продолжаются
    float Anim(int id, bool on, float speed = 14);
    // рамка, которая догоняет target пружиной; передний край быстрее заднего - плашка тянется,
    // как капля. Ключи свои, с id элементов и Anim не пересекаются
    RectF Glide(int key, const RectF& target);
    void GlideSnap(int key) { glides_.erase(key); }
    // круг от точки нажатия внутри r (рамка элемента id), тает за полсекунды
    void Ripple(int id, const RectF& r, float radius);
    // ожидание вместо «...»: кадры идут таймером ~30 в секунду (TakeTick), а не подряд, как у Anim
    void HexSpinner(const RectF& r, Color c);                        // соты: волна бежит по кругу
    void Shimmer(const RectF& r, float radius, BYTE strength = 34);  // блик проходит слева направо
    float BounceDots(float x, float cy, Color c, float d);           // три точки волной; ширина
    void Skeleton(const RectF& r, int rows, float rowH);             // заготовки строк, пока список считается
    // пустой список: сота с галочкой и пояснение; под курсором сота покачивается и вокруг кружат
    // маленькие соты, щелчок по ней - россыпь сот
    void EmptyState(int id, const RectF& r, const std::wstring& text);
    void Tick() { tick_ = true; }
    bool TakeTick() {
        bool t = tick_;
        tick_ = false;
        return t;
    }
    // карточки страницы: при смене раздела выезжают по очереди (список живёт один кадр)
    void MarkCard(const RectF& r) { cards_.push_back(r); }
    const std::vector<RectF>& Cards() const { return cards_; }
    // рисовать в другой холст (появление страницы); возвращает прежний
    Gdiplus::Graphics* Redirect(Gdiplus::Graphics* g) {
        Gdiplus::Graphics* old = g_;
        g_ = g;
        return old;
    }
    // подсказка при наведении: регистрируется в кадре, рисуется DrawTip после задержки
    void Tip(int id, const std::wstring& text);
    void DrawTip(float w, float h);

    // элементы
    // busy - дело идёт: вместо многоточия прыгают точки и по кнопке бежит блик
    bool Button(int id, const RectF& r, const std::wstring& text, Kind kind = Kind::Secondary,
                const std::wstring& icon = L"", bool busy = false);
    bool Toggle(int id, const RectF& r, bool& v);
    bool Slider(int id, const RectF& r, int& v, int lo, int hi, int step = 1);
    bool Segmented(int id, const RectF& r, const std::vector<std::wstring>& items, int& sel);
    void Chip(const RectF& r, const std::wstring& t, Color bg, Color fg);
    void Keycap(const RectF& r, const std::wstring& t);
    float KeycapWidth(const std::wstring& t);
    void Field(int id, const RectF& r, const std::wstring& value, std::function<void(const std::wstring&)> onChange,
               const std::wstring& placeholder = L"");
    void Dots(const RectF& r, Color c);  // значок «ещё»: три точки

    // поля ввода
    HBRUSH FieldBrush();
    COLORREF FieldText() const { return RGB(c_.text.GetR(), c_.text.GetG(), c_.text.GetB()); }
    COLORREF FieldBack() const { return RGB(c_.field.GetR(), c_.field.GetG(), c_.field.GetB()); }
    void DropFocus();
    void FocusField(int id, bool selectAll = true);
    bool FieldFocused(int id) const;
    bool EditFocused() const;

private:
    struct Hit {
        int id;
        RectF r;
    };
    struct Edit {
        HWND hwnd = nullptr;
        std::function<void(const std::wstring&)> onChange;
        bool used = false;
        RECT pos{};
    };
    Gdiplus::Font* Font(float size, bool bold);
    int HitAt(int x, int y) const;

    HWND hwnd_ = nullptr;
    Gdiplus::Graphics* g_ = nullptr;
    UiColors c_{};
    double k_ = 1;
    std::vector<Hit> hits_, prevHits_;
    std::vector<int> track_, prevTrack_;
    std::map<int, float> scroll_, scrollShown_;  // куда прокрутить и где страница сейчас
    std::vector<std::pair<int, RectF>> scrollViews_, prevScrollViews_;
    int hot_ = 0, active_ = 0, clicked_ = 0, released_ = 0, rdown_ = 0, rclicked_ = 0, dbl_ = 0;
    bool hotChanged_ = false;
    ULONGLONG hotSince_ = 0, lastFrame_ = 0;
    float dt_ = 0;
    std::map<int, float> anims_;
    struct Spring {
        float e[4], v[4];  // края лево, верх, право, низ и их скорости
    };
    std::map<int, Spring> glides_;
    struct Wave {
        POINT p;
        ULONGLONG t;
    };
    std::vector<RectF> cards_;
    bool tick_ = false;
    std::map<int, Wave> ripples_;      // откуда и когда нажали на элемент
    std::map<int, ULONGLONG> bursts_;  // когда щёлкнул переключатель
    std::map<int, std::wstring> tips_;
    bool down_ = false, repaint_ = false, suppress_ = false;
    POINT mouse_{-1, -1}, press_{};
    std::map<int, Edit> edits_;
    std::map<long long, std::unique_ptr<Gdiplus::Font>> fonts_;
    std::unique_ptr<Gdiplus::FontFamily> fam_, famBold_;
    HFONT editFont_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    COLORREF fieldBrushColor_ = 0;
    IconCache icons_;
    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> pictures_;  // готовые карточки Picture
    std::unique_ptr<Gdiplus::GraphicsPath> path_;
};

}  // namespace ui
