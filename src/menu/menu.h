#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/config.h"
#include "core/project.h"
#include "gfx/icons.h"

class MenuDrop;

// Полукольцевое меню у края экрана. Окно слоёное, без фокуса, рисуется GDI+ в DIB.
class RadialMenu {
public:
    bool Create(HINSTANCE inst);
    // demo - показ из студии: меню не гаснет, когда курсор далеко, закрывается кликом мимо или Esc
    void Show(const Config* cfg, POINT cursor, const RECT& monitor, bool leftEdge, UINT dpi, bool demo = false);
    // Целое кольцо вокруг курсора (двойной клик по пустому столу): секторы обоих краёв вместе.
    void ShowAround(const Config* cfg, POINT cursor, const RECT& work, UINT dpi);
    // Кольцо открытых окон вокруг курсора (Alt + ` или угол): в середине живая миниатюра наведённого окна.
    void ShowWindows(const Config* cfg, POINT cursor, const RECT& work, UINT dpi);
    // Кольцо «Сеть» (действие угла): пункты - действия (special 3, ключ в path), сверху строка состояния.
    void ShowNet(const Config* cfg, std::vector<Item> items, POINT cursor, const RECT& work, UINT dpi);
    std::function<void(const std::wstring& key)> onNet;  // выбрали действие сети
    std::function<std::wstring()> netStatus;            // строка состояния для кольца сети
    void Close();
    bool IsOpen() const { return open_; }
    // Заранее грузит значки, чтобы первое открытие не подтормаживало. Работает порциями не дольше budgetMs,
    // чтобы не замораживать программу; true - всё загружено.
    bool Prewarm(const Config* cfg, UINT dpi, int budgetMs = 8);
    void ResetIcons() {
        icons_.Clear();
        composed_.clear();
        project::Forget();  // значки проектов могли появиться или смениться
        warm_.clear();
        warmPos_ = 0;
    }

private:
    friend class MenuDrop;
    enum class Mode { None, Root, Search };
    struct Hit {
        int root = -1, sub = -1;
        double dist = 0;
    };
    struct Geo {
        double k, rHub, r1, r2, r3, r4;
        int iconRoot, iconSub;
        float font;
        int w, h;
    };
    struct Ring {
        double rIn, rOut;
        double rIcon, rLabel;
        int iconPx;
        bool sub;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK MouseHook(int, WPARAM, LPARAM);
    static LRESULT CALLBACK KeyHook(int, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    static Geo MakeGeo(double dpiScale);
    void EnsureSurface();
    void CaptureBackdrop();
    void StopHooks();
    void Tick();
    void Hide();
    void Launch();  // открыли пункт: выбранный сектор вспыхивает волной, меню гаснет, клики проходят насквозь
    double CloseMs() const;  // сколько гаснет: после запуска дольше, чтобы прошла волна
    void Start();  // общее начало показа: сброс состояния, первый кадр, перехватчики
    void PlaceAround(POINT cursor, const RECT& work);  // окно-квадрат с центром у курсора

    // ввод
    Hit HitTest(POINT screen) const;
    void UpdateHover(const Hit& h, ULONGLONG now);
    bool OnRawKey(DWORD vk, DWORD scan, bool down);
    void OnKey(UINT vk);
    void OnChar(wchar_t ch);
    void OnRightClick();
    void OnWheel(int delta);

    // второе кольцо
    void SwitchRoot(int root);
    void RebuildSub();
    void Drill(Item folder);
    bool Back();
    void TurnPage(int dir);  // прокрутить второе кольцо на почти целый экран пунктов

    // вращение: первое кольцо поворачивается по кругу, второе прокручивается, если пункты не влезли
    double RelAngle(POINT screen) const;  // угол курсора от направления «внутрь», -180..180
    double RootSlot(int i) const;         // где стоит сектор i с учётом поворота, 0..n
    int RootAt(double rel) const;         // какой сектор первого кольца под углом
    double SubWinFrom() const;            // окно второго кольца по углу
    double SubWinTo() const;
    double SubMaxScroll() const;
    void UpdateSubMid();  // второе кольцо держится у своего сектора, даже когда первое крутят
    void ScrollSub(double items);
    void EnsureSubVisible(int idx);
    void BeginGrab(POINT p);
    void DragGrab(POINT p, ULONGLONG now);
    void EndGrab();
    void OnClick(POINT p);  // одиночный клик выбирает, двойной - открывает
    std::wstring ItemKey(const Item* it) const;
    void BuildSearchPool();
    int HotSub() const;
    const Item* HoveredItem() const;

    // открытие
    void Activate(const Item& it);
    void ActivateHover();
    bool ToDialog(const Item& it) const;  // открыть не в Проводнике, а в диалоге «Открыть/Сохранить»

    // кольцо окон
    void CloseHoveredWindow();
    bool ThumbRect(double grow, RECT& rc);
    void UpdateThumb(double grow, double alpha);
    void DropThumb();

    // перетаскивание файлов
    void OnDragEnter(IDataObject* data);
    DWORD OnDragOver(DWORD keys, POINTL pt, DWORD allowed);
    void OnDragLeave();
    DWORD OnDrop(IDataObject* data, DWORD keys, POINTL pt, DWORD allowed);
    void LeaveShellTarget();

    // рисование
    double Extent() const { return mode_ != Mode::None ? geo_.r4 : geo_.r2; }
    // целое кольцо начинается сверху и идёт по часовой стрелке
    double AbsAngle(double rel) const { return full_ ? rel + 90.0 : left_ ? rel : 180.0 - rel; }
    double Span() const;  // полураскрыв первого кольца в градусах
    double SubStep() const;
    double SubFrom() const;  // угол начала первого пункта второго кольца (с учётом прокрутки)
    const std::wstring& IconPng(const Item& it) const;
    double RootHue(int root) const;  // свой оттенок у каждой группы, сдвиг в градусах
    // значок пункта; папки без своего значка - значок проекта или папка со значком типа
    Gdiplus::Bitmap* ItemIcon(const Item& it, int px);
    void DrawIcon(Gdiplus::Graphics& g, Gdiplus::Bitmap* icon, const Item& it, double x, double y, double sz,
                  double hl, double appear, bool hot);
    void DrawLabel(Gdiplus::Graphics& g, const Ring& r, double a0, double a1, const Item& it, double hl,
                   double appear);
    double FitWidth(const Ring& r, double a0, double a1, double rad, double halfH, double& x, double& y) const;
    struct LabelFit {
        std::wstring text;  // строки через \n, уже сокращённые
        double x = 0, y = 0;
        float px = 0;  // размер шрифта, при котором влезло
    };
    LabelFit LayoutLabel(const Ring& r, double a0, double a1, const Item& it);
    LabelFit LayoutLabelAt(const Ring& r, double a0, double a1, const Item& it);  // при текущем LabelPx
    float TextWidth(const std::wstring& s);
    // размер подписей секторов: крупный, а если имя не влезает - чуть мельче (подбирает LayoutLabel)
    float LabelPx() const { return labelPx_ > 0 ? labelPx_ : geo_.font * 1.15f; }
    float labelPx_ = 0;
    Gdiplus::Bitmap* LabelBitmap(const std::wstring& text, bool bright);
    void DrawInfoCard(Gdiplus::Graphics& g, double grow, double alpha, bool below);  // below - под кольцом
    void Render();
    void EnsureHexGlass();  // узор сотового стекла под размер окна (строится раз, потом из кэша)
    void DrawHexLift(Gdiplus::Graphics& g, Gdiplus::GraphicsPath& sector, double hl);  // сота под курсором
    void PaintBase(double ext, double catcherR);  // тень и подложка для кликов прямо в пикселях кадра
    struct Sector {
        Ring ring;
        double a0, a1;
        const Item* it;
        double hl, appear;
        bool active;
        double hue;
        bool hot;
    };
    // кольцо целиком: стекло и соты одним проходом по пикселям под маской всех секторов, остальное по секторам
    void DrawRing(Gdiplus::Graphics& g, const std::vector<Sector>& secs, Gdiplus::GraphicsPath* clip);
    // картинка 1:1 прямо в пиксели кадра, с той же билинейной выборкой на дробной позиции, что у GDI+;
    // false - когда так нельзя (кадр обрезан клипом), тогда рисует сам GDI+
    bool Blit(Gdiplus::Bitmap* bmp, double x, double y);
    void DrawPill(Gdiplus::Graphics& g, const std::wstring& text, const std::wstring& iconFile, double y, double alpha,
                  bool accentEdge);
    std::wstring BottomHint(double& alpha) const;
    std::wstring TopHint() const;

    HWND hwnd_ = nullptr;
    HHOOK mouseHook_ = nullptr, keyHook_ = nullptr;
    IDropTarget* drop_ = nullptr;  // MenuDrop из menu_drop.cpp
    const Config* cfg_ = nullptr;
    const std::vector<Item>* ring_ = nullptr;
    IconCache icons_;
    struct WarmJob {
        std::wstring png, shell;
        int px;
    };
    std::vector<WarmJob> warm_;  // очередь прогрева значков
    size_t warmPos_ = 0;
    Geo geo_{};

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int surfW_ = 0, surfH_ = 0;
    // стекло, соты и маска секторов - прямо пиксели размером с окно (PARGB): заливка TextureBrush
    // стоила по 0.35 мс на сектор, проход по пикселям под маской - доли миллисекунды на всё кольцо
    std::vector<uint32_t> blurPx_;
    std::vector<uint32_t> hexPx_;   // сотовое стекло: тонкие грани поверх секторов
    bool blitOk_ = false;  // Blit разрешён: рисуется кольцо без клипа
    std::vector<uint32_t> maskPx_;  // покрытие секторов кольца (альфа), рисуется GDI+ сплошной заливкой
    int hexW_ = 0, hexH_ = 0;
    bool hexOn_ = false;
    POINT cursorLocal_{};  // курсор в координатах окна (для приподнятой соты)

    bool open_ = false, closing_ = false;
    bool launching_ = false;
    int launchRoot_ = -1, launchSub_ = -1;
    int subSlide_ = 0;  // второе кольцо: 0 - раскрыть веером, +1/-1 - перелистнуть страницу в эту сторону
    bool left_ = true;
    bool full_ = false;           // целое кольцо вокруг курсора
    std::vector<Item> fullRing_;  // секторы обоих краёв для целого кольца
    bool winMode_ = false;        // кольцо окон
    std::vector<Item> winRing_;
    bool netMode_ = false;  // кольцо сети
    bool reached_ = false;  // курсор уже был внутри кольца (до этого далёкий курсор меню не закрывает)
    std::vector<Item> netRing_;
    HTHUMBNAIL thumb_ = nullptr;  // миниатюра DWM наведённого окна
    HWND thumbWin_ = nullptr;
    HWND dialog_ = nullptr;  // диалог «Открыть/Сохранить», который был впереди, когда открылось меню
    POINT origin_{};              // левый верхний угол окна на экране
    POINT center_{};              // центр колец в координатах окна
    ULONGLONG tOpen_ = 0, tSub_ = 0, tClose_ = 0, tLast_ = 0;
    bool altWasDown_ = false;
    bool demo_ = false;
    bool shift_ = false, ctrl_ = false;
    bool swallowed_[256] = {};

    Mode mode_ = Mode::None;
    int hoverRoot_ = -1, hoverSub_ = -1, subParent_ = -1;
    int pendingRoot_ = -1;
    ULONGLONG pendingSince_ = 0;
    std::wstring lastHoverKey_;
    double subMid_ = 0;
    std::vector<double> hlRoot_, hlSub_;
    std::vector<Item> subOwned_;
    std::vector<const Item*> subItems_;
    std::vector<Item> navStack_;  // папки, в которые зашли внутри второго кольца

    double rootRot_ = 0, rootRotTo_ = 0;      // поворот первого кольца, в секторах
    double subScroll_ = 0, subScrollTo_ = 0;  // прокрутка второго кольца, в пунктах
    int grab_ = -1;                           // за какое кольцо тянут: 0 - первое, 1 - второе
    bool grabSpun_ = false;                   // уже крутили, это не клик
    double grabAngle_ = 0, grabPath_ = 0, grabVel_ = 0;
    ULONGLONG grabT_ = 0;
    std::wstring clickKey_;  // первый клик двойного
    ULONGLONG clickT_ = 0;
    POINT clickPt_{};
    std::wstring hoverKey_;  // наведённый пункт
    ULONGLONG keyT_ = 0;     // с какого момента наведён (карточка)
    ULONGLONG hoverT_ = 0;   // с какого момента оживает значок (наведение или одиночный клик)

    struct LabelBmp {
        std::unique_ptr<Gdiplus::Bitmap> bmp;
        int w = 0, h = 0;
    };
    std::map<std::wstring, LabelBmp> labels_;               // подписи с тенью, рисуются один раз
    std::map<std::wstring, float> textW_;                   // ширина строк в пикселях
    std::map<std::wstring, LabelFit> labelFit_;             // раскладка подписей по сектору и имени
    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> composed_;  // папка + значок типа

    std::wstring query_;
    std::vector<Item> searchPool_;
    bool poolBuilt_ = false;
    int kbSel_ = 0;

    bool dragging_ = false;
    IDataObject* dragData_ = nullptr;
    IDropTarget* shellTarget_ = nullptr;
    std::wstring targetKey_, targetName_;
    bool targetDir_ = false;
    DWORD dragEffect_ = 0;
    std::wstring dwellKey_;
    ULONGLONG dwellSince_ = 0;
};
