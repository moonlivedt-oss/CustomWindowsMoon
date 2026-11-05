#include "helpers/markup.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "core/text.h"
#include "core/themes.h"
#include "helpers/markup_logic.h"
#include "helpers/ocr.h"
#include "helpers/pin.h"
#include "helpers/snip.h"
#include "helpers/translate.h"
#include "helpers/translate_logic.h"
#include "ui/ui.h"

using namespace Gdiplus;

namespace markup {
namespace {

constexpr wchar_t kClass[] = L"MoonRadialMarkup";
constexpr wchar_t kPrefsKey[] = L"Software\\MoonRadial\\Markup";
constexpr UINT WM_ACT = WM_APP + 2, WM_TEXT_END = WM_APP + 3, WM_OCR_TEXT = WM_APP + 4, WM_OCR_WORDS = WM_APP + 5,
               WM_OCR_LINES = WM_APP + 6, WM_TR_DONE = WM_APP + 7, WM_TR_STEP = WM_APP + 8;
enum Act { ACT_COPY = 1, ACT_APPLY, ACT_SAVE, ACT_PIN, ACT_OPEN, ACT_TRANSLATE };
enum {
    ID_TOOL = 100,  // Segmented занимает id * 16 и дальше
    ID_THICK = 110,
    ID_COLOR = 900,  // шесть кружков цвета
    ID_UNDO = 910,
    ID_REDO,
    ID_COPY,
    ID_APPLY,
    ID_SAVE,
    ID_PIN,
    ID_OPEN,
    ID_PICK,
    ID_DELETE,
    ID_HIDE_SECRETS,
    ID_KEEP_SECRETS,
    ID_TRANSLATE,
    ID_TOOL_BUTTON = 930  // девять кнопок инструментов
};
// порядок на панели; в истории и реестре инструмент хранится своим значением
constexpr Tool kToolOrder[] = {Tool::Select, Tool::Arrow, Tool::Box,  Tool::Marker, Tool::Label,
                               Tool::Step,   Tool::Blur,  Tool::Crop, Tool::Ocr};
constexpr uint32_t kColors[] = {0xE5484D, 0xFFD23F, 0x46C26B, 0x4DA3FF, 0xFFFFFF};
constexpr int kCustomColor = 5;  // шестой кружок - цвет с пипетки
constexpr float kThick[] = {0.6f, 1.0f, 1.6f};
constexpr const wchar_t* kFontName = L"Segoe UI";

Color Opaque(uint32_t rgb, BYTE a = 255) { return Color(a, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

RectF ToRectF(const Rc& r) { return RectF(r.x, r.y, r.w, r.h); }

// Размер подписи: путь текста знает его точно, и выбор с ручкой работают по настоящим краям
void MeasureLabel(Shape& s) {
    if (s.tool != Tool::Label || s.pts.empty() || s.text.empty()) return;
    FontFamily fam(kFontName);
    GraphicsPath path;
    path.AddString(s.text.c_str(), -1, &fam, FontStyleBold, s.width, PointF(s.pts[0].x, s.pts[0].y), nullptr);
    RectF b;
    path.GetBounds(&b);
    s.extent = {b.GetRight() - s.pts[0].x, b.GetBottom() - s.pts[0].y};
}

void TranslationFormat(StringFormat& f, int align) {
    f.SetAlignment(align == 1 ? StringAlignmentCenter : align == 2 ? StringAlignmentFar : StringAlignmentNear);
    f.SetTrimming(StringTrimmingNone);
}

// Размер текста перевода: так же, как он потом будет нарисован
Pt MeasureTranslation(const std::wstring& text, float size, float wrapW, void*) {
    Bitmap one(1, 1, PixelFormat32bppRGB);
    Graphics g(&one);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    FontFamily fam(kFontName);
    Font font(&fam, size, FontStyleRegular, UnitPixel);
    StringFormat f;
    TranslationFormat(f, 0);
    RectF out;
    g.MeasureString(text.c_str(), -1, &font, RectF(0, 0, wrapW, 100000), &f, &out);
    return {out.Width, out.Height};
}

// Одна фигура в координатах картинки. Размытие здесь - только рамка пока тянут; в картинку его кладёт Compose.
// number - номер цифры по порядку среди цифр: удалили одну, остальные перенумеровались сами
void DrawShape(Graphics& g, const Shape& s, int number = 0) {
    g.SetSmoothingMode(SmoothingModeHighQuality);
    switch (s.tool) {
        case Tool::Arrow: {
            if (s.pts.size() < 2) return;
            Pt a[4];
            ArrowGeometry(s.pts[0], s.pts[1], s.width, a);
            Pen line(Opaque(s.rgb), s.width);
            line.SetStartCap(LineCapRound);
            g.DrawLine(&line, s.pts[0].x, s.pts[0].y, a[0].x, a[0].y);
            PointF head[3] = {PointF(a[1].x, a[1].y), PointF(a[2].x, a[2].y), PointF(a[3].x, a[3].y)};
            SolidBrush b(Opaque(s.rgb));
            g.FillPolygon(&b, head, 3);
            break;
        }
        case Tool::Box: {
            if (s.pts.size() < 2) return;
            Pen pen(Opaque(s.rgb), s.width);
            pen.SetLineJoin(LineJoinRound);
            g.DrawRectangle(&pen, ToRectF(Bounds(s)));
            break;
        }
        case Tool::Marker: {
            if (s.pts.size() < 2) return;
            // одним путём: на пересечениях штрих не темнеет
            GraphicsPath path;
            std::vector<PointF> pts;
            pts.reserve(s.pts.size());
            for (const Pt& p : s.pts) pts.emplace_back(p.x, p.y);
            path.AddLines(pts.data(), (INT)pts.size());
            Pen pen(Opaque(s.rgb, 110), s.width);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            pen.SetLineJoin(LineJoinRound);
            g.DrawPath(&pen, &path);
            break;
        }
        case Tool::Label: {
            if (s.pts.empty() || s.text.empty()) return;
            // тёмный контур: подпись читается и на светлом, и на тёмном снимке
            FontFamily fam(kFontName);
            GraphicsPath path;
            path.AddString(s.text.c_str(), -1, &fam, FontStyleBold, s.width, PointF(s.pts[0].x, s.pts[0].y), nullptr);
            Pen edge(Color(220, 0, 0, 0), std::max(2.0f, s.width / 5));
            edge.SetLineJoin(LineJoinRound);
            g.DrawPath(&edge, &path);
            SolidBrush fill(Opaque(s.rgb));
            g.FillPath(&fill, &path);
            break;
        }
        case Tool::Step: {
            if (s.pts.empty()) return;
            RectF r = ToRectF(Bounds(s));
            SolidBrush fill(Opaque(s.rgb));
            g.FillEllipse(&fill, r);
            Pen ring(Color(200, 0, 0, 0), std::max(1.0f, s.width / 16));
            g.DrawEllipse(&ring, r);
            FontFamily fam(kFontName);
            Font font(&fam, s.width * (number >= 10 ? 0.46f : 0.58f), FontStyleBold, UnitPixel);
            StringFormat cf;
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush ink(IsLight(s.rgb) ? Color(255, 20, 20, 24) : Color(255, 255, 255, 255));
            std::wstring text = std::to_wstring(number);
            g.DrawString(text.c_str(), -1, &font, r, &cf, &ink);
            break;
        }
        case Tool::Blur:
        case Tool::Crop:
        case Tool::Ocr: {
            if (s.pts.size() < 2) return;
            RectF r = ToRectF(Bounds(s));
            SolidBrush veil(Color(s.tool == Tool::Blur ? 90 : 40, 0, 0, 0));
            g.FillRectangle(&veil, r);
            Pen dash(Color(230, 255, 255, 255), std::max(1.0f, s.width / 2));
            dash.SetDashStyle(DashStyleDash);
            g.DrawRectangle(&dash, r);
            break;
        }
        case Tool::Translation: {
            if (s.pts.size() < 2) return;
            RectF r = ToRectF(Bounds(s));
            SolidBrush fill(Opaque(s.rgb));
            g.SetSmoothingMode(SmoothingModeNone);
            g.FillRectangle(&fill, r);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            FontFamily fam(kFontName);
            Font font(&fam, s.width, FontStyleRegular, UnitPixel);
            StringFormat f;
            TranslationFormat(f, s.align);
            SolidBrush ink(Opaque(s.ink));
            g.DrawString(s.text.c_str(), -1, &font, r, &f, &ink);
            break;
        }
        case Tool::Select:
            break;
    }
}

// Картинка с нанесёнными фигурами. Размытие меняет сами пиксели: под ним в итоговом файле исходника нет
void Compose(Bitmap* base, const std::vector<Shape>& shapes, std::unique_ptr<Bitmap>& out) {
    int w = (int)base->GetWidth(), h = (int)base->GetHeight();
    out = std::make_unique<Bitmap>(w, h, PixelFormat32bppRGB);
    {
        Graphics g(out.get());
        g.DrawImage(base, 0, 0, w, h);
    }
    int number = 0;
    for (const Shape& s : shapes) {
        if (s.tool == Tool::Blur && s.pts.size() >= 2) {
            Rect all(0, 0, w, h);
            BitmapData d{};
            if (out->LockBits(&all, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppRGB, &d) == Ok) {
                Pixelate((uint32_t*)d.Scan0, w, h, d.Stride / 4, s.pts[0], s.pts[1]);
                out->UnlockBits(&d);
            }
        } else {
            if (s.tool == Tool::Step) ++number;
            Graphics g(out.get());
            DrawShape(g, s, number);
        }
    }
}

std::unique_ptr<Bitmap> CutOut(Bitmap* src, const Rc& r) {
    int w = std::max(1, (int)r.w), h = std::max(1, (int)r.h);
    auto out = std::make_unique<Bitmap>(w, h, PixelFormat32bppRGB);
    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(src, Rect(0, 0, w, h), (INT)r.x, (INT)r.y, w, h, UnitPixel);
    return out;
}

DWORD ReadPref(const wchar_t* name, DWORD fallback) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kPrefsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS
               ? v
               : fallback;
}

void WritePref(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kPrefsKey, name, REG_DWORD, &v, sizeof(v));
}

// Значки панели: узнаются быстрее подписей, а подпись снимает сомнения. r - квадрат значка
void ToolIcon(Graphics& g, Tool tool, const RectF& r, Color c, float k) {
    g.SetSmoothingMode(SmoothingModeHighQuality);
    Pen pen(c, 1.6f * k);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush fill(c);
    const float x = r.X, y = r.Y, w = r.Width, cx = x + w / 2, cy = y + w / 2;
    auto line = [&](float x0, float y0, float x1, float y1) {
        g.DrawLine(&pen, x + w * x0, y + w * y0, x + w * x1, y + w * y1);
    };
    switch (tool) {
        case Tool::Select: {
            PointF p[] = {PointF(x + w * .22f, y + w * .1f), PointF(x + w * .82f, y + w * .56f),
                          PointF(x + w * .52f, y + w * .6f), PointF(x + w * .38f, y + w * .9f)};
            g.DrawPolygon(&pen, p, 4);
            break;
        }
        case Tool::Arrow:
            line(.15f, .85f, .85f, .15f);
            line(.42f, .15f, .85f, .15f);
            line(.85f, .15f, .85f, .58f);
            break;
        case Tool::Box:
            g.DrawRectangle(&pen, x + w * .12f, y + w * .22f, w * .76f, w * .56f);
            break;
        case Tool::Marker: {
            Pen thick(Color(170, c.GetR(), c.GetG(), c.GetB()), 4.5f * k);
            thick.SetStartCap(LineCapRound);
            thick.SetEndCap(LineCapRound);
            g.DrawLine(&thick, x + w * .14f, y + w * .68f, x + w * .86f, y + w * .32f);
            break;
        }
        case Tool::Label:
            line(.18f, .16f, .82f, .16f);
            line(.5f, .16f, .5f, .86f);
            break;
        case Tool::Step:
            g.DrawEllipse(&pen, x + w * .08f, y + w * .08f, w * .84f, w * .84f);
            line(.44f, .36f, .54f, .28f);
            line(.54f, .28f, .54f, .72f);
            break;
        case Tool::Blur:
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    if ((i + j) % 2 == 0)
                        g.FillRectangle(&fill, x + w * (.1f + i * .28f), y + w * (.1f + j * .28f), w * .24f, w * .24f);
            break;
        case Tool::Translation:
            break;  // на панели такой кнопки нет: перевод снизу, среди действий
        case Tool::Crop:
            line(.28f, .04f, .28f, .72f);
            line(.28f, .72f, .96f, .72f);
            line(.04f, .28f, .72f, .28f);
            line(.72f, .28f, .72f, .96f);
            break;
        case Tool::Ocr:
            line(.1f, .28f, .1f, .1f);
            line(.1f, .1f, .28f, .1f);
            line(.72f, .1f, .9f, .1f);
            line(.9f, .1f, .9f, .28f);
            line(.1f, .72f, .1f, .9f);
            line(.1f, .9f, .28f, .9f);
            line(.72f, .9f, .9f, .9f);
            line(.9f, .9f, .9f, .72f);
            line(.32f, .36f, .68f, .36f);
            line(.5f, .36f, .5f, .68f);
            break;
    }
    (void)cx;
    (void)cy;
}

// Круговая стрелка: mirror - «вернуть», без него - «отменить»
void TurnIcon(Graphics& g, const RectF& r, Color c, float k, bool mirror) {
    g.SetSmoothingMode(SmoothingModeHighQuality);
    Pen pen(c, 1.7f * k);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    const float w = r.Width;
    auto X = [&](float v) { return r.X + w * (mirror ? 1 - v : v); };
    auto Y = [&](float v) { return r.Y + w * v; };
    PointF curve[] = {PointF(X(.18f), Y(.4f)), PointF(X(.45f), Y(.2f)), PointF(X(.82f), Y(.32f)), PointF(X(.82f), Y(.6f)),
                      PointF(X(.62f), Y(.84f))};
    g.DrawCurve(&pen, curve, 5);
    g.DrawLine(&pen, X(.18f), Y(.4f), X(.18f), Y(.12f));
    g.DrawLine(&pen, X(.18f), Y(.4f), X(.46f), Y(.46f));
}

class Editor;
Editor* g_editor = nullptr;

class Editor {
public:
    // keepPrefs - запоминать инструмент и цвет; показательные режимы настройки пользователя не читают и не пишут
    Editor(const UiColors& colors, std::unique_ptr<Bitmap> image, Callbacks callbacks, bool keepPrefs)
        : colors_(colors), base_(std::move(image)), callbacks_(std::move(callbacks)), keepPrefs_(keepPrefs) {
        imageW_ = (int)base_->GetWidth();
        imageH_ = (int)base_->GetHeight();
        Compose(base_.get(), {}, shown_);
        LoadPrefs();
    }

    bool Create(bool visible) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Static;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);  // повторная регистрация безвредна

        POINT cur;
        GetCursorPos(&cur);
        HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
        // масштаб монитора с курсором: окно откроется на нём, а не на основном
        UINT dpiX = 96, dpiY = 96;
        if (auto* fn = (HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*))(void*)GetProcAddress(LoadLibraryW(L"shcore.dll"),
                                                                                            "GetDpiForMonitor"))
            fn(mon, 0, &dpiX, &dpiY);
        k_ = dpiX / 96.0;
        // окно по снимку, но не меньше панелей и не больше экрана
        int minW = std::min((int)(980 * k_), workW), minH = std::min((int)(560 * k_), workH);
        int w = std::clamp(imageW_ + (int)(60 * k_), minW, std::max(minW, workW * 9 / 10));
        int h = std::clamp(imageH_ + (int)(240 * k_), minH, std::max(minH, workH * 9 / 10));
        wnd_ = CreateWindowExW(0, kClass, L"Разметка снимка", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               visible ? mi.rcWork.left + (workW - w) / 2 : -6000,
                               visible ? mi.rcWork.top + (workH - h) / 2 : -6000, w, h, nullptr, nullptr, inst, this);
        if (!wnd_) return false;
        k_ = GetDpiForWindow(wnd_) / 96.0;
        ui_.Attach(wnd_);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(wnd_, 20, &dark, sizeof(dark));
        if (visible) {
            ShowWindow(wnd_, SW_SHOWNORMAL);
            SetForegroundWindow(wnd_);
        }
        return true;
    }

    // Поиск секретов сам, сразу после открытия: не обязан ничего находить, лишь предлагает
    void ScanForSecrets(const std::vector<uint8_t>& png) { ocr::RecognizeWordsAsync(png, wnd_, WM_OCR_WORDS); }

    void CloseNow() {
        confirmed_ = true;
        if (wnd_) DestroyWindow(wnd_);
    }

    void ShowWindowToFront() {
        if (IsIconic(wnd_)) ShowWindow(wnd_, SW_RESTORE);
        SetForegroundWindow(wnd_);
    }

    HWND Wnd() const { return wnd_; }

    // Для снимка в картинку: кадр окна на этом Bitmap
    void PaintInto(Bitmap& target) {
        Graphics g(&target);
        ui_.Begin(g, colors_, k_);
        Frame((float)target.GetWidth(), (float)target.GetHeight());
        ui_.End();
    }

    void AddDemoShapes() {
        const float W = (float)imageW_, H = (float)imageH_, s = std::min(W / 1000, H / 600);
        auto add = [&](Tool t, std::vector<Pt> pts, uint32_t rgb, float width, const std::wstring& text = L"") {
            Shape shape;
            shape.tool = t;
            shape.pts = std::move(pts);
            shape.rgb = rgb;
            shape.width = width * s;
            shape.text = text;
            MeasureLabel(shape);
            doc_.Add(std::move(shape));
        };
        add(Tool::Blur, {{W * .08f, H * .12f}, {W * .46f, H * .24f}}, 0, 3);
        add(Tool::Box, {{W * .54f, H * .12f}, {W * .92f, H * .42f}}, kColors[0], 4);
        add(Tool::Arrow, {{W * .28f, H * .78f}, {W * .56f, H * .44f}}, kColors[0], 4);
        std::vector<Pt> wave;
        for (int i = 0; i <= 24; ++i)
            wave.push_back({W * (.08f + .34f * i / 24), H * .55f + (float)std::sin(i * .7f) * 6 * s});
        add(Tool::Marker, std::move(wave), kColors[1], 16);
        add(Tool::Label, {{W * .58f, H * .62f}}, kColors[4], 24, L"Вот здесь ошибка");
        add(Tool::Step, {{W * .12f, H * .72f}}, kColors[0], 26);
        add(Tool::Step, {{W * .20f, H * .72f}}, kColors[1], 26);
        selected_ = 1;  // рамка выбрана: видны ручки
        tool_ = Tool::Select;
        secretBoxes_ = {{W * .06f, H * .84f, W * .3f, H * .08f}};
        Compose(base_.get(), doc_.Shapes(), shown_);
    }

    std::unique_ptr<Bitmap> Flatten() { return CutOut(shown_.get(), CurrentView()); }

    void DemoStartTranslate() { TranslateShot(); }
    bool DemoWorking() const { return !working_.empty(); }
    const std::wstring& DemoNote() const { return note_; }

public:
    bool standalone_ = false;

private:
    static LRESULT CALLBACK Static(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        auto* self = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (!self->wnd_) self->wnd_ = h;
        return self->Proc(msg, wp, lp);
    }

    void LoadPrefs() {
        if (!keepPrefs_ || snipimg::TestMode()) return;
        DWORD tool = ReadPref(L"Tool", (DWORD)Tool::Arrow);
        // обрезка и распознавание - разовые дела: после нового снимка с них начинать незачем
        if (tool <= (DWORD)Tool::Select && (Tool)tool != Tool::Select) tool_ = lastDrawTool_ = (Tool)tool;
        colorIndex_ = std::clamp((int)ReadPref(L"Color", 0), 0, kCustomColor);
        custom_ = ReadPref(L"Custom", 0xFFFFFFFF);
        if (colorIndex_ == kCustomColor && custom_ == 0xFFFFFFFF) colorIndex_ = 0;
        thick_ = std::clamp((int)ReadPref(L"Thick", 1), 0, 2);
    }

    void SavePrefs() const {
        if (!keepPrefs_ || snipimg::TestMode()) return;
        WritePref(L"Tool", (DWORD)lastDrawTool_);
        WritePref(L"Color", (DWORD)colorIndex_);
        WritePref(L"Custom", custom_);
        WritePref(L"Thick", (DWORD)thick_);
    }

    Rc CurrentView() const { return View(doc_.Crop(), imageW_, imageH_); }
    uint32_t CurrentColor() const { return colorIndex_ == kCustomColor ? custom_ : kColors[colorIndex_]; }
    float ImageUnit() const {
        return fit_.scale > 0 ? (float)k_ / fit_.scale : (float)k_;
    }  // один dip в пикселях снимка
    float WidthFor(Tool tool) const {
        float dip = tool == Tool::Marker ? 16 : tool == Tool::Label ? 20 : tool == Tool::Step ? 26 : 4;
        return dip * kThick[thick_] * ImageUnit();
    }

    void Refresh() { InvalidateRect(wnd_, nullptr, FALSE); }
    void After() {
        if (ui_.TakeRepaint()) Refresh();
    }

    // Фигуры с учётом той, которую сейчас тянут: она ещё не в истории
    void Rebuild() {
        if (selected_ >= 0 && dragging_ && selected_ < (int)doc_.Shapes().size()) {
            std::vector<Shape> live = doc_.Shapes();
            live[(size_t)selected_] = live_;
            Compose(base_.get(), live, shown_);
        } else {
            Compose(base_.get(), doc_.Shapes(), shown_);
        }
        composedAt_ = GetTickCount64();
        Refresh();
    }

    bool InImage(int x, int y) const {
        return x >= fit_.x && y >= fit_.y && x < fit_.x + fit_.w && y < fit_.y + fit_.h;
    }

    void PickColor(int x, int y) {
        Pt p = ToImage(fit_, (float)x, (float)y, CurrentView());
        Color c;
        if (shown_->GetPixel(std::min((int)p.x, imageW_ - 1), std::min((int)p.y, imageH_ - 1), &c) == Ok) {
            custom_ = (uint32_t)c.GetR() << 16 | (uint32_t)c.GetG() << 8 | c.GetB();
            colorIndex_ = kCustomColor;
            ApplyStyleToSelected();
        }
        picking_ = false;
        Refresh();
    }

    void ApplyStyleToSelected() {
        if (selected_ < 0 || selected_ >= (int)doc_.Shapes().size()) return;
        Shape s = doc_.Shapes()[(size_t)selected_];
        if (s.tool == Tool::Blur || s.tool == Tool::Translation) return;
        s.rgb = CurrentColor();
        s.width = WidthFor(s.tool);
        MeasureLabel(s);
        doc_.Replace((size_t)selected_, std::move(s));
        Rebuild();
    }

    void BeginAt(int x, int y) {
        const Rc view = CurrentView();
        Pt p = ToImage(fit_, (float)x, (float)y, view);
        if (tool_ == Tool::Select) {
            const float tol = 6 * ImageUnit();
            const auto& shapes = doc_.Shapes();
            if (selected_ >= 0 && selected_ < (int)shapes.size()) {
                int handle = HandleAt(shapes[(size_t)selected_], p, tol);
                if (handle >= 0) {
                    StartDrag(handle, p);
                    return;
                }
            }
            selected_ = TopShape(shapes, p, tol);
            if (selected_ >= 0) StartDrag(-1, p);
            Refresh();
            return;
        }
        if (!InImage(x, y)) return;
        lastDrawTool_ = tool_ == Tool::Crop || tool_ == Tool::Ocr ? lastDrawTool_ : tool_;
        if (tool_ == Tool::Label) {
            StartText(x, y, p);
            return;
        }
        if (tool_ == Tool::Step) {
            Shape s;
            s.tool = Tool::Step;
            s.pts = {p};
            s.rgb = CurrentColor();
            s.width = WidthFor(Tool::Step);
            doc_.Add(std::move(s));
            Rebuild();
            return;
        }
        drawing_ = true;
        current_ = Shape{};
        current_.tool = tool_;
        current_.rgb = CurrentColor();
        current_.width = WidthFor(tool_);
        current_.pts = {p, p};
        if (tool_ == Tool::Marker) current_.pts.resize(1);
    }

    void StartDrag(int handle, Pt p) {
        dragging_ = true;
        dragHandle_ = handle;
        dragFrom_ = p;
        live_ = doc_.Shapes()[(size_t)selected_];
        dragMoved_ = false;
    }

    void MoveAt(int x, int y) {
        Pt p = ToImage(fit_, (float)x, (float)y, CurrentView());
        if (drawing_) {
            if (tool_ == Tool::Marker)
                current_.pts.push_back(p);
            else
                current_.pts[1] = p;
            Refresh();
            return;
        }
        if (dragging_) {
            dragMoved_ = true;
            if (dragHandle_ >= 0)
                DragHandle(live_, dragHandle_, p);
            else
                Translate(live_, p.x - dragFrom_.x, p.y - dragFrom_.y);
            dragFrom_ = p;
            // пересборка всего снимка: на большом не чаще 30 раз в секунду, последний кадр соберёт отпускание
            if (GetTickCount64() - composedAt_ >= 33) Rebuild();
        }
    }

    void EndDrawing() {
        drawing_ = false;
        Shape s = std::move(current_);
        current_ = Shape{};
        if (s.tool == Tool::Arrow && s.pts.size() == 2) SnapAxis(s.pts[0], s.pts[1]);
        if (s.tool == Tool::Marker) StraightenStroke(s.pts);
        if (s.tool == Tool::Crop) {
            Rc r = Bounds(s);
            if (r.w >= 8 && r.h >= 8) {
                doc_.SetCrop(r);
                selected_ = -1;
                tool_ = lastDrawTool_;
                Rebuild();
            } else {
                Refresh();
            }
            return;
        }
        if (s.tool == Tool::Ocr) {
            if (Meaningful(s)) ReadRegion(Bounds(s));
            Refresh();
            return;
        }
        if (Meaningful(s)) {
            doc_.Add(std::move(s));
            Rebuild();
        } else {
            Refresh();
        }
    }

    void EndDrag() {
        dragging_ = false;
        if (dragMoved_) doc_.Replace((size_t)selected_, live_);
        Rebuild();
    }

    void CancelGesture() {
        drawing_ = false;
        dragging_ = false;
        current_ = Shape{};
        Rebuild();
    }

    // Текст только из выделенного куска снимка; мелкое увеличивает само распознавание
    void ReadRegion(const Rc& r) {
        Rc area = View(r, imageW_, imageH_);
        std::unique_ptr<Bitmap> part = CutOut(shown_.get(), area);
        note_ = L"Читаю текст...";
        ocr::RecognizeAsync(snipimg::ToPng(part.get()), wnd_, WM_OCR_TEXT);
    }

    bool HasTranslation() const {
        for (const Shape& s : doc_.Shapes())
            if (s.tool == Tool::Translation) return true;
        return false;
    }

    // Перевод снимка. Читается только то, что видно: с нанесённым размытием и без обрезанного, поэтому скрытое не
    // попадёт ни в перевод, ни тем более наружу. Уже переведён - кнопка возвращает оригинал
    void TranslateShot() {
        if (!working_.empty()) return;
        if (HasTranslation()) {
            std::vector<Shape> rest;
            for (const Shape& s : doc_.Shapes())
                if (s.tool != Tool::Translation) rest.push_back(s);
            selected_ = -1;
            doc_.SetShapes(std::move(rest));
            note_ = L"Оригинал. Вернуть перевод можно отменой";
            Rebuild();
            return;
        }
        selected_ = -1;
        readView_ = CurrentView();
        std::unique_ptr<Bitmap> part = CutOut(shown_.get(), readView_);
        working_ = L"Читаю текст на снимке";
        note_.clear();
        ocr::RecognizeLinesAsync(snipimg::ToPng(part.get()), wnd_, WM_OCR_LINES);
        Refresh();
    }

    void OnLines(std::unique_ptr<std::vector<ocr::Line>> lines, bool ok) {
        pending_.clear();
        std::wstring all;
        skippedSecrets_ = 0;
        occupied_.clear();
        if (ok && lines) {
            for (ocrtext::Block& b : ocrtext::MakeBlocks(*lines)) {
                occupied_.push_back({b.x + readView_.x, b.y + readView_.y, b.w, b.h});  // сюда перевод не залезает
                if (b.code || ocrtext::IsNoise(b.text) || ocrtext::IsGarbageBlock(b.text)) continue;
                if (translate::ContainsSecret(b.text)) {
                    ++skippedSecrets_;
                    continue;
                }
                b.x += readView_.x;
                b.y += readView_.y;
                all += b.text + L"\n";
                pending_.push_back(std::move(b));
            }
        }
        if (pending_.empty()) {
            working_.clear();
            note_ = ok ? L"Здесь нечего переводить" : L"Распознать не получилось";
            Refresh();
            return;
        }
        // куда - по большинству букв на снимке; что уже на нужном языке, не закрываем
        const std::wstring source = translate::SourceOf(all), target = translate::TargetFor(source);
        std::vector<ocrtext::Block> keep;
        for (ocrtext::Block& b : pending_)
            if (translate::SourceOf(b.text) == source) keep.push_back(std::move(b));
        pending_ = std::move(keep);
        translate::Job job;
        for (const ocrtext::Block& b : pending_) job.texts.push_back(b.text);
        job.source = source;
        job.target = target;
        job.wnd = wnd_;
        job.msgDone = WM_TR_DONE;
        job.msgStep = WM_TR_STEP;
        working_ = L"Перевожу на этом компьютере";
        Refresh();
        translate::Start(std::move(job));
    }

    void OnTranslated(std::unique_ptr<translate::Result> r) {
        working_.clear();
        if (!r || !r->ok) {
            note_ = r && !r->error.empty() ? r->error : L"Перевод не получился";
            if (r && r->problem == translate::Problem::NoModel) note_ += L": её можно скачать в настройках, Помощники - Перевод";
            pending_.clear();
            Refresh();
            return;
        }
        // цвет фона берём здесь: под блоком может быть то, что нарисовали, пока шёл перевод
        Rect all(0, 0, imageW_, imageH_);
        BitmapData d{};
        const bool locked = shown_->LockBits(&all, ImageLockModeRead, PixelFormat32bppRGB, &d) == Ok;
        std::vector<Shape> shapes = doc_.Shapes();
        int done = 0;
        for (size_t i = 0; i < pending_.size() && i < r->texts.size(); ++i) {
            const ocrtext::Block& b = pending_[i];
            const std::wstring& text = r->texts[i];
            if (Trim(text).empty() || translate::SameText(text, b.text)) continue;
            // по бокам почти без запаса: рядом может стоять край панели, закрывать его нельзя
            const float padX = 1, padY = std::max(1.0f, b.lineH * 0.12f);
            const float x = std::max(0.0f, b.x - padX), y = std::max(0.0f, b.y - padY);
            const float w = b.w + 2 * padX, h = b.h + 2 * padY;
            // расширять можно только подпись в одну строку: абзац рядом с панелью остаётся в своих границах и мельчает.
            // Рамка слова без хвостов букв ниже настоящего шрифта: у одной строки это заметно
            const Rc view = CurrentView();
            std::vector<Rc> others;
            for (const Rc& o : occupied_)
                if (o.x != b.x || o.y != b.y) others.push_back(o);
            const float maxW = b.lines == 1 ? RoomRight({x, y, w, h}, others, view.Right()) - x : w;
            TextFit fit = FitText(text, w, h, b.lineH * (b.lines == 1 ? 1.15f : 1.0f), maxW, MeasureTranslation, nullptr);
            Shape s;
            s.tool = Tool::Translation;
            s.pts = {{x, y}, {x + fit.w, y + fit.h}};
            s.width = fit.size;
            s.text = text;
            s.align = fit.w > w ? 0 : b.align;
            s.rgb = locked ? ocrtext::EdgeColor((const uint32_t*)d.Scan0, imageW_, imageH_, d.Stride / 4, b.x, b.y, b.w, b.h) : 0x202028;
            s.ink = IsLight(s.rgb) ? 0x14141A : 0xF4F4F8;
            shapes.push_back(std::move(s));
            ++done;
        }
        if (locked) shown_->UnlockBits(&d);
        pending_.clear();
        if (!done) {
            note_ = L"Перевод ничего не изменил";
            Refresh();
            return;
        }
        doc_.SetShapes(std::move(shapes));
        const int secret = skippedSecrets_ + r->blocked;
        note_ = (r->via == translate::Via::Google ? L"Переведено через Google: " : L"Переведено на этом компьютере: ") +
                Plural(done, L"блок", L"блока", L"блоков");
        if (secret) note_ += L". Похоже на секрет и не тронуто: " + std::to_wstring(secret);
        Rebuild();
    }

    void HideSecrets() {
        for (const Rc& r : secretBoxes_) {
            Shape s;
            s.tool = Tool::Blur;
            s.pts = {{r.x, r.y}, {r.Right(), r.Bottom()}};
            s.width = WidthFor(Tool::Blur);
            doc_.Add(std::move(s));
        }
        note_ = L"Скрыто. Просмотрите снимок сами: распознавание может пропустить часть текста";
        secretBoxes_.clear();
        Rebuild();
    }

    // Подпись: настоящее поле ввода прямо на снимке; Enter или щелчок мимо - готово, Esc - передумали
    void StartText(int x, int y, Pt where) {
        FinishText(true);
        textAt_ = where;
        const int px = (int)std::lround(20 * kThick[thick_] * k_);
        if (textFont_) DeleteObject(textFont_);
        textFont_ = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, kFontName);
        RECT rc;
        GetClientRect(wnd_, &rc);
        int width = std::max(60, std::min((int)(fit_.x + fit_.w) - x, (int)rc.right - x - 4));
        text_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, width,
                                px + (int)(8 * k_), wnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(text_, WM_SETFONT, (WPARAM)textFont_, TRUE);
        SendMessageW(text_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Подпись");
        SetWindowLongPtrW(text_, GWLP_USERDATA, (LONG_PTR)this);
        textProc_ = (WNDPROC)SetWindowLongPtrW(text_, GWLP_WNDPROC, (LONG_PTR)TextProc);
        SetFocus(text_);
    }

    static LRESULT CALLBACK TextProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
            PostMessageW(self->wnd_, WM_TEXT_END, wp == VK_RETURN, 0);
            return 0;
        }
        if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  // без системного писка
        if (msg == WM_KILLFOCUS) PostMessageW(self->wnd_, WM_TEXT_END, 1, 0);
        return CallWindowProcW(self->textProc_, h, msg, wp, lp);
    }

    void FinishText(bool keep) {
        if (!text_) return;
        HWND edit = text_;
        text_ = nullptr;  // до DestroyWindow: потеря фокуса не должна прийти сюда второй раз
        int n = GetWindowTextLengthW(edit);
        std::wstring s(n, L'\0');
        GetWindowTextW(edit, s.data(), n + 1);
        DestroyWindow(edit);
        SetFocus(wnd_);
        Shape label;
        label.tool = Tool::Label;
        label.pts = {textAt_};
        label.text = s;
        label.rgb = CurrentColor();
        label.width = WidthFor(Tool::Label);
        MeasureLabel(label);
        if (keep && Meaningful(label)) {
            doc_.Add(std::move(label));
            Rebuild();
        } else {
            Refresh();
        }
    }

    void DoUndo() {
        selected_ = -1;
        if (doc_.Undo()) Rebuild();
    }
    void DoRedo() {
        selected_ = -1;
        if (doc_.Redo()) Rebuild();
    }
    void DeleteSelected() {
        if (selected_ < 0) return;
        doc_.Remove((size_t)selected_);
        selected_ = -1;
        Rebuild();
    }

    void Act(int act) {
        FinishText(true);
        switch (act) {
            case ACT_COPY: {
                auto flat = Flatten();
                note_ =
                    snipimg::ToClipboard(wnd_, flat.get()) ? L"Скопировано" : L"Буфер обмена занят, попробуйте ещё раз";
                Refresh();
                break;
            }
            case ACT_PIN: {
                auto flat = Flatten();
                pin::Show(*flat);
                break;
            }
            case ACT_OPEN: {
                // файл с правками: исходник открывать незачем, размытое на нём ещё видно
                auto flat = Flatten();
                std::wstring file = snipimg::TempFile(L"Разметка.png");
                std::vector<uint8_t> png = snipimg::ToPng(flat.get());
                if (!file.empty() && snipimg::WriteBytes(file, png))
                    ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            case ACT_TRANSLATE:
                TranslateShot();
                break;
            case ACT_APPLY:
            case ACT_SAVE: {
                bool save = act == ACT_SAVE;
                auto flat = Flatten();
                // Win+Shift+S положил в буфер исходник: после «Готово» вставляться должен снимок с правками
                if (!doc_.Empty() && !save) snipimg::ToClipboard(wnd_, flat.get());
                if ((!doc_.Empty() || save) && callbacks_.done)
                    callbacks_.done(std::move(flat), save ? Finish::ApplyAndSave : Finish::Apply);
                CloseNow();
                break;
            }
        }
    }

    bool AskClose() {
        if (confirmed_ || doc_.Empty()) return true;
        return MessageBoxW(wnd_, L"Закрыть без правок? Нарисованное пропадёт.", L"Разметка снимка",
                           MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    }

    bool OnKey(WPARAM vk) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (vk == VK_ESCAPE) {
            if (drawing_ || dragging_)
                CancelGesture();
            else if (picking_) {
                picking_ = false;
                Refresh();
            } else if (selected_ >= 0) {
                selected_ = -1;
                Refresh();
            } else {
                PostMessageW(wnd_, WM_CLOSE, 0, 0);
            }
            return true;
        }
        if (vk == VK_DELETE) {
            DeleteSelected();
            return true;
        }
        if (!ctrl) return false;
        if (vk == 'Z') {
            shift ? DoRedo() : DoUndo();
            return true;
        }
        if (vk == 'Y') {
            DoRedo();
            return true;
        }
        if (vk == 'C') {
            Act(ACT_COPY);
            return true;
        }
        if (vk == 'S') {
            Act(ACT_SAVE);
            return true;
        }
        return false;
    }

    // Цвет и толщина нужны только тому, что их использует: размытию, обрезке и распознаванию они лишние
    bool WantsStyle() const {
        if (tool_ == Tool::Select) {
            if (selected_ < 0 || selected_ >= (int)doc_.Shapes().size()) return false;
            const Tool t = doc_.Shapes()[(size_t)selected_].tool;
            return t != Tool::Blur && t != Tool::Translation;
        }
        return tool_ != Tool::Blur && tool_ != Tool::Crop && tool_ != Tool::Ocr;
    }

    // Панель инструментов: три группы (выбор; рисование; действия со снимком). Плашка переезжает к выбранному
    void Toolbar(float pad, float rowH) {
        const UiColors& c = ui_.C();
        Graphics& g = ui_.G();
        static const wchar_t* kNames[] = {L"Выбор", L"Стрелка", L"Рамка", L"Маркер", L"Текст", L"Цифры", L"Размытие", L"Обрезка", L"Распознать"};
        static const wchar_t* kTips[] = {L"Двигать и менять то, что уже нарисовано",
                                         L"Показать на важное",
                                         L"Обвести",
                                         L"Выделить строку или участок",
                                         L"Подпись на снимке",
                                         L"Шаги по порядку: 1, 2, 3",
                                         L"Скрыть личное без возможности восстановить",
                                         L"Оставить только нужную часть",
                                         L"Скопировать текст из выделенного куска"};
        const float bw = ui_.S(88), groupGap = ui_.S(10);
        RectF rects[9];
        int current = 1;
        float x = pad;
        for (int i = 0; i < 9; ++i) {
            if (i == 1 || i == 6) {
                Pen sep(c.border, 1);
                g.DrawLine(&sep, x + groupGap / 2, pad + ui_.S(8), x + groupGap / 2, pad + rowH - ui_.S(8));
                x += groupGap;
            }
            rects[i] = RectF(x, pad, bw, rowH);
            x += bw;
            if (kToolOrder[i] == tool_) current = i;
        }
        for (int i = 0; i < 9; ++i) {
            const int id = ID_TOOL_BUTTON + i;
            if (ui_.Click(id, rects[i])) {
                tool_ = kToolOrder[i];
                if (tool_ != Tool::Select) selected_ = -1;
                picking_ = false;
                note_.clear();
                current = i;
                PostMessageW(wnd_, WM_TEXT_END, 1, 0);
            }
            ui_.Tip(id, kTips[i]);
            float hover = ui_.Anim(id + 2000, ui_.Hot(id) && i != current, 18);
            if (hover > 0.01f) {
                Color h = c.cardHi;
                ui_.Fill(rects[i], Color((BYTE)(h.GetA() * hover), h.GetR(), h.GetG(), h.GetB()), ui_.S(8));
            }
        }
        const RectF pill = ui_.Glide(-7000, rects[current]);
        ui_.Fill(pill, c.accent, ui_.S(8));
        const float icon = ui_.S(15), gap = ui_.S(5);
        for (int i = 0; i < 9; ++i) {
            // тёмная подпись - только там, где под ней уже плашка: пока та едет, текст не пропадает
            const bool onPill = pill.Contains(rects[i].X + rects[i].Width / 2, rects[i].Y + rects[i].Height / 2);
            const Color ink = onPill ? c.onAccent : i == current || ui_.Hot(ID_TOOL_BUTTON + i) ? c.text : c.muted;
            const float tw = ui_.Measure(kNames[i], 11.5f, i == current);
            const float sx = rects[i].X + (rects[i].Width - icon - gap - tw) / 2;
            ToolIcon(g, kToolOrder[i], RectF(sx, rects[i].Y + (rowH - icon) / 2, icon, icon), ink, ui_.S(1));
            ui_.Text(kNames[i], RectF(sx + icon + gap, rects[i].Y, tw + ui_.S(4), rowH), ink, 11.5f, i == current);
        }
    }

    // Отмена и возврат: значки; когда нечего отменять, кнопка гаснет и не нажимается
    void HistoryButton(int id, const RectF& r, bool mirror, bool enabled, const wchar_t* tip) {
        const UiColors& c = ui_.C();
        if (enabled && ui_.Click(id, r)) {
            mirror ? DoRedo() : DoUndo();
        }
        if (enabled) ui_.Tip(id, tip);
        const float hover = enabled ? ui_.Anim(id + 2000, ui_.Hot(id), 18) : 0;
        if (hover > 0.01f) {
            Color h = c.cardHi;
            ui_.Fill(r, Color((BYTE)(h.GetA() * hover), h.GetR(), h.GetG(), h.GetB()), ui_.S(8));
        }
        Color ink = enabled ? c.text : Color(70, c.muted.GetR(), c.muted.GetG(), c.muted.GetB());
        ui_.Stroke(r, enabled ? c.border : Color(50, c.border.GetR(), c.border.GetG(), c.border.GetB()), ui_.S(8));
        const float s = ui_.S(17);
        TurnIcon(ui_.G(), RectF(r.X + (r.Width - s) / 2, r.Y + (r.Height - s) / 2, s, s), ink, ui_.S(1), mirror);
    }

    void Frame(float W, float H) {
        const UiColors& c = ui_.C();
        Graphics& g = ui_.G();
        ui_.Fill(RectF(0, 0, W, H), c.win);
        const float pad = ui_.S(12), rowH = ui_.S(36), optH = ui_.S(32), optY = pad + rowH + ui_.S(8);

        // первый ряд: инструменты и отмена
        Toolbar(pad, rowH);
        HistoryButton(ID_REDO, RectF(W - pad - rowH, pad, rowH, rowH), true, doc_.CanRedo(), L"Вернуть (Ctrl+Y)");
        HistoryButton(ID_UNDO, RectF(W - pad - 2 * rowH - ui_.S(6), pad, rowH, rowH), false, doc_.CanUndo(), L"Отменить (Ctrl+Z)");

        // предложение скрыть секрет: только предлагает, решает человек
        float rightEdge = W - pad;
        if (!secretBoxes_.empty()) {
            float rx = W - pad - ui_.S(88);
            if (ui_.Button(ID_KEEP_SECRETS, RectF(rx, optY, ui_.S(88), optH), L"Оставить", ui::Kind::Ghost)) {
                secretBoxes_.clear();
                Refresh();
            }
            rx -= ui_.S(84);
            if (ui_.Button(ID_HIDE_SECRETS, RectF(rx, optY, ui_.S(78), optH), L"Скрыть", ui::Kind::Primary))
                PostMessageW(wnd_, WM_ACT, 100, 0);
            ui_.Tip(ID_HIDE_SECRETS, L"Закрасить отмеченное на снимке");
            std::wstring say = L"Похоже на секрет: " + std::to_wstring(secretBoxes_.size());
            float tw = ui_.Measure(say, 12, true) + ui_.S(6);
            ui_.Text(say, RectF(rx - tw, optY, tw, optH), c.warn, 12, true);
            rightEdge = rx - tw - ui_.S(10);
        }

        // второй ряд: цвет, пипетка, толщина - только когда они что-то меняют. Рядом - подсказка, там её и увидят
        float x = pad + ui_.S(2);
        if (WantsStyle()) {
            g.SetSmoothingMode(SmoothingModeHighQuality);
            const float d = ui_.S(20), gap = ui_.S(8);
            const int swatches = custom_ == 0xFFFFFFFF ? kCustomColor : kCustomColor + 1;
            for (int i = 0; i < swatches; ++i) {
                RectF r(x, optY + (optH - d) / 2, d, d);
                if (ui_.Click(ID_COLOR + i, r)) {
                    colorIndex_ = i;
                    ApplyStyleToSelected();
                }
                SolidBrush b(Opaque(i == kCustomColor ? custom_ : kColors[i]));
                g.FillEllipse(&b, r);
                Pen rim(Color(70, 255, 255, 255), 1);
                g.DrawEllipse(&rim, r);  // тёмный цвет с пипетки не должен сливаться с фоном
                if (i == colorIndex_) {
                    Pen ring(c.text, ui_.S(2));
                    g.DrawEllipse(&ring, r.X - ui_.S(3), r.Y - ui_.S(3), d + ui_.S(6), d + ui_.S(6));
                }
                x += d + gap;
            }
            x += ui_.S(4);
            if (ui_.Button(ID_PICK, RectF(x, optY, ui_.S(82), optH), L"Пипетка", picking_ ? ui::Kind::Primary : ui::Kind::Ghost)) {
                picking_ = !picking_;
                note_.clear();
            }
            ui_.Tip(ID_PICK, L"Взять цвет со снимка");
            x += ui_.S(90);
            int thick = thick_;
            if (ui_.Segmented(ID_THICK, RectF(x, optY, ui_.S(204), optH), {L"Тонко", L"Средне", L"Толсто"}, thick)) {
                thick_ = thick;
                ApplyStyleToSelected();
            }
            x += ui_.S(212);
        }
        if (selected_ >= 0) {
            if (ui_.Button(ID_DELETE, RectF(x, optY, ui_.S(84), optH), L"Удалить", ui::Kind::Danger))
                PostMessageW(wnd_, WM_KEYDOWN, VK_DELETE, 0);
            ui_.Tip(ID_DELETE, L"Убрать выбранную фигуру (Delete)");
            x += ui_.S(92);
        }
        std::wstring hint = !working_.empty() ? working_ + std::wstring((size_t)(GetTickCount64() / 350 % 4), L'.') : note_;
        if (!working_.empty()) SetTimer(wnd_, 1, 350, nullptr);  // точки бегут, пока идёт работа
        if (hint.empty()) hint = picking_ ? L"Щёлкните по снимку, чтобы взять цвет" : Hint();
        ui_.Text(hint, RectF(x + ui_.S(4), optY, std::max(0.0f, rightEdge - x - ui_.S(4)), optH), note_.empty() && working_.empty() ? c.muted : c.accent, 12);

        // снимок: чуть приподнят над фоном, чтобы тёмный не сливался с окном
        const float top = optY + optH + ui_.S(8), bottomH = rowH + 2 * pad;
        RectF area(pad, top, W - 2 * pad, H - top - bottomH);
        ui_.Fill(area, c.field, ui_.S(8));
        const float inset = ui_.S(8);
        const Rc view = CurrentView();
        fit_ = FitImage((int)view.w, (int)view.h, area.X + inset, area.Y + inset, area.Width - 2 * inset,
                        area.Height - 2 * inset, EditZoom((int)view.w, (int)view.h));
        RectF dst(fit_.x, fit_.y, fit_.w, fit_.h);
        for (int i = 3; i >= 1; --i) {
            SolidBrush shade(Color((BYTE)(30 + 14 * (3 - i)), 0, 0, 0));
            float grow = ui_.S((float)i * 2);
            g.FillRectangle(&shade, dst.X - grow, dst.Y - grow + ui_.S(1), dst.Width + 2 * grow, dst.Height + 2 * grow);
        }
        g.SetInterpolationMode(fit_.scale >= 1 ? InterpolationModeNearestNeighbor : InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(shown_.get(), dst, view.x, view.y, view.w, view.h, UnitPixel);
        g.SetPixelOffsetMode(PixelOffsetModeDefault);
        {
            Pen frame(Color(46, 255, 255, 255), 1);
            g.DrawRectangle(&frame, dst.X - 1, dst.Y - 1, dst.Width + 1, dst.Height + 1);
        }
        {
            GraphicsState st = g.Save();
            g.SetClip(dst);
            g.TranslateTransform(fit_.x - view.x * fit_.scale, fit_.y - view.y * fit_.scale);
            g.ScaleTransform(fit_.scale, fit_.scale);
            if (drawing_) {
                int number = 1;
                for (const Shape& s : doc_.Shapes()) number += s.tool == Tool::Step;
                DrawShape(g, current_, number);
            }
            // места, предложенные к скрытию: лёгкая заливка и пунктир, видно и на светлом снимке
            for (const Rc& r : secretBoxes_) {
                Color w = c.warn;
                SolidBrush tint(Color(60, w.GetR(), w.GetG(), w.GetB()));
                g.FillRectangle(&tint, ToRectF(r));
                Pen dash(w, 2 * ImageUnit());
                dash.SetDashStyle(DashStyleDash);
                g.DrawRectangle(&dash, ToRectF(r));
            }
            g.Restore(st);
        }
        // выбранная фигура: рамка с зазором от самой фигуры и круглые ручки; размер не зависит от масштаба снимка
        if (selected_ >= 0 && selected_ < (int)doc_.Shapes().size()) {
            const Shape& s = dragging_ ? live_ : doc_.Shapes()[(size_t)selected_];
            Rc b = Bounds(s);
            Pt a = ToWindow(fit_, {b.x, b.y}, view);
            const float off = ui_.S(5);
            RectF outline(a.x - off, a.y - off, b.w * fit_.scale + 2 * off, b.h * fit_.scale + 2 * off);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            if (s.tool != Tool::Arrow && s.tool != Tool::Box) {
                Pen dark(Color(140, 0, 0, 0), 1);
                g.DrawRectangle(&dark, outline.X + 1, outline.Y + 1, outline.Width, outline.Height);
                Pen dash(Color(240, 255, 255, 255), 1);
                dash.SetDashStyle(DashStyleDash);
                g.DrawRectangle(&dash, outline);
            }
            const float hs = ui_.S(10);
            SolidBrush white(Color(255, 255, 255, 255));
            Pen ring(c.accent, ui_.S(2));
            for (const Pt& h : Handles(s)) {
                Pt p = ToWindow(fit_, h, view);
                RectF r(p.x - hs / 2, p.y - hs / 2, hs, hs);
                g.FillEllipse(&white, r);
                g.DrawEllipse(&ring, r);
            }
        }

        // низ: размер и действия. У каждой кнопки подсказка: чем «Готово» отличается от «Сохранить» - неочевидно
        const float by = H - pad - rowH;
        float rx = W - pad;
        auto action = [&](int id, const wchar_t* label, float width, ui::Kind kind, int act, const wchar_t* tip) {
            rx -= width;
            if (ui_.Button(id, RectF(rx, by, width, rowH), label, kind)) PostMessageW(wnd_, WM_ACT, act, 0);
            ui_.Tip(id, tip);
            rx -= ui_.S(8);
        };
        action(ID_APPLY, L"Готово", ui_.S(96), ui::Kind::Primary, ACT_APPLY, L"Применить правки и положить снимок в буфер обмена");
        action(ID_SAVE, L"Сохранить", ui_.S(104), ui::Kind::Secondary, ACT_SAVE, L"Сохранить файлом и закрыть (Ctrl+S)");
        action(ID_COPY, L"Копировать", ui_.S(112), ui::Kind::Secondary, ACT_COPY, L"В буфер обмена, окно остаётся открытым (Ctrl+C)");
        action(ID_PIN, L"Закрепить", ui_.S(104), ui::Kind::Secondary, ACT_PIN, L"Поверх всех окон");
        action(ID_OPEN, L"Открыть", ui_.S(92), ui::Kind::Ghost, ACT_OPEN, L"Посмотреть в обычном просмотрщике");
        const bool translated = HasTranslation();
        action(ID_TRANSLATE, !working_.empty() ? L"Перевожу" : translated ? L"Оригинал" : L"Перевести", ui_.S(104), ui::Kind::Ghost, ACT_TRANSLATE,
               translated ? L"Убрать перевод с снимка"
               : translate::GoogleAllowed()
                   ? L"Заменить текст на снимке переводом. На этом компьютере, а если здесь нельзя - через Google (только текст)"
                   : L"Заменить текст на снимке переводом. Всё на этом компьютере, наружу ничего не уходит");
        std::wstring size = std::to_wstring((int)view.w) + L" × " + std::to_wstring((int)view.h) + L" px";
        if (fit_.scale != 1) size += L"  ·  " + std::to_wstring((int)std::lround(fit_.scale * 100)) + L"%";
        ui_.Text(size, RectF(pad, by, std::max(0.0f, rx - pad), rowH), c.muted, 12);
        ui_.DrawTip(W, H);
    }

    const wchar_t* Hint() const {
        switch (tool_) {
            case Tool::Select:
                return selected_ >= 0 ? L"Тяните фигуру или её ручки" : L"Щёлкните по фигуре, чтобы её менять";
            case Tool::Arrow:
                return L"Тяните от начала к острию";
            case Tool::Box:
                return L"Тяните от угла к углу";
            case Tool::Marker:
                return L"Ведите по нужному месту";
            case Tool::Label:
                return L"Щёлкните по снимку и пишите";
            case Tool::Step:
                return L"Щёлкайте по шагам: номера идут по порядку";
            case Tool::Blur:
                return L"Закрасьте то, что нельзя показывать: восстановить будет нельзя";
            case Tool::Crop:
                return L"Обведите, что оставить. Вернуть всё можно отменой";
            case Tool::Ocr:
                return L"Обведите текст: он попадёт в буфер обмена";
            case Tool::Translation:
                break;
        }
        return L"";
    }

    void Paint() {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd_, &ps);
        RECT rc;
        GetClientRect(wnd_, &rc);
        int w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
        if (!memDC_ || w != bw_ || h != bh_) {
            if (memDC_) {
                SelectObject(memDC_, oldBmp_);
                DeleteObject(dib_);
                DeleteDC(memDC_);
            }
            memDC_ = CreateCompatibleDC(dc);
            dib_ = CreateCompatibleBitmap(dc, w, h);
            oldBmp_ = SelectObject(memDC_, dib_);
            bw_ = w;
            bh_ = h;
        }
        {
            Graphics g(memDC_);
            ui_.Begin(g, colors_, k_);
            Frame((float)w, (float)h);
            ui_.End();
        }
        BitBlt(dc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
        EndPaint(wnd_, &ps);
        if (text_) UpdateWindow(text_);
        if (ui_.TakeRepaint())
            Refresh();
        else if (ui_.TakeTick())
            SetTimer(wnd_, 1, 33, nullptr);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_PAINT:
                Paint();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_TIMER:
                KillTimer(wnd_, wp);
                Refresh();
                return 0;
            case WM_OCR_LINES: {
                std::unique_ptr<std::vector<ocr::Line>> lines((std::vector<ocr::Line>*)lp);
                OnLines(std::move(lines), wp == 0);
                return 0;
            }
            case WM_TR_STEP:
                if (!working_.empty()) {
                    working_ = wp == translate::kStarting
                                   ? std::wstring(L"Запускаю переводчик. В первый раз это до 30 с")
                                   : L"Перевожу: " + std::to_wstring((int)wp + 1) + L" из " + std::to_wstring((int)lp);
                    Refresh();
                }
                return 0;
            case WM_TR_DONE:
                OnTranslated(std::unique_ptr<translate::Result>((translate::Result*)lp));
                return 0;
            case WM_SIZE:
                Refresh();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* m = (MINMAXINFO*)lp;
                m->ptMinTrackSize.x = (LONG)(980 * k_);
                m->ptMinTrackSize.y = (LONG)(460 * k_);
                return 0;
            }
            case WM_DPICHANGED: {
                k_ = HIWORD(wp) / 96.0;
                ui_.SetScale(k_);
                auto* r = (RECT*)lp;
                SetWindowPos(wnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_MOUSEMOVE: {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd_, 0};
                TrackMouseEvent(&tme);
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                ui_.OnMouseMove(x, y);
                if (ui_.HotChanged()) SetTimer(wnd_, 1, 650, nullptr);  // подсказка, если задержаться
                if (drawing_ || dragging_) MoveAt(x, y);
                After();
                return 0;
            }
            case WM_MOUSELEAVE:
                ui_.OnLeave();
                After();
                return 0;
            case WM_LBUTTONDOWN: {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                note_.clear();
                FinishText(true);
                SetFocus(wnd_);
                ui_.OnDown(x, y);
                if (picking_ && InImage(x, y))
                    PickColor(x, y);
                else if (!ui_.HotId() && !picking_)
                    BeginAt(x, y);
                After();
                return 0;
            }
            case WM_LBUTTONUP: {
                // до OnUp: он отпускает захват мыши, а это отменяет жест
                if (drawing_) EndDrawing();
                if (dragging_) EndDrag();
                ui_.OnUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                After();
                return 0;
            }
            case WM_CAPTURECHANGED:
                if (drawing_ || dragging_) CancelGesture();  // мышь забрало другое окно: незаконченного не остаётся
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT && (HWND)wp == wnd_) {
                    if (ui_.OnSetCursor()) return TRUE;
                    POINT p;
                    GetCursorPos(&p);
                    ScreenToClient(wnd_, &p);
                    if (InImage(p.x, p.y)) {
                        const wchar_t* cursor = IDC_CROSS;
                        if (tool_ == Tool::Label && !picking_) cursor = IDC_IBEAM;
                        if (tool_ == Tool::Select && !picking_) {
                            Pt at = ToImage(fit_, (float)p.x, (float)p.y, CurrentView());
                            cursor = TopShape(doc_.Shapes(), at, 6 * ImageUnit()) >= 0 ? IDC_SIZEALL : IDC_ARROW;
                        }
                        SetCursor(LoadCursorW(nullptr, cursor));
                        return TRUE;
                    }
                }
                break;
            case WM_KEYDOWN:
                if (OnKey(wp)) return 0;
                break;
            case WM_ACT:
                if (wp == 100)
                    HideSecrets();
                else
                    Act((int)wp);
                return 0;
            case WM_TEXT_END:
                FinishText(wp != 0);
                return 0;
            case WM_OCR_TEXT: {
                std::unique_ptr<std::wstring> text((std::wstring*)lp);
                if (wp == 0 && text && !text->empty() && snipimg::TextToClipboard(wnd_, *text)) {
                    note_ = L"В буфере: " + text->substr(0, text->find_first_of(L"\r\n"));
                } else {
                    note_ = L"Здесь текста не нашлось";
                }
                Refresh();
                return 0;
            }
            case WM_OCR_WORDS: {
                std::unique_ptr<std::vector<ocr::Word>> words((std::vector<ocr::Word>*)lp);
                if (wp == 0 && words) {
                    std::vector<OcrWord> list;
                    list.reserve(words->size());
                    for (const ocr::Word& w : *words) {
                        OcrWord o;
                        o.text = w.text;
                        o.box = {w.x, w.y, w.w, w.h};
                        o.line = w.line;
                        list.push_back(std::move(o));
                    }
                    secretBoxes_ = SecretBoxes(list, 4);
                    Refresh();
                }
                return 0;
            }
            case WM_CTLCOLOREDIT: {
                HDC dc = (HDC)wp;
                const uint32_t rgb = CurrentColor();
                SetTextColor(dc, RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
                SetBkColor(dc, RGB(20, 20, 24));
                if (!textBack_) textBack_ = CreateSolidBrush(RGB(20, 20, 24));
                return (LRESULT)textBack_;
            }
            case WM_COMMAND:
                return 0;
            case WM_CLOSE:
                FinishText(true);
                if (AskClose()) DestroyWindow(wnd_);
                return 0;
            case WM_DESTROY:
                SavePrefs();
                if (memDC_) {
                    SelectObject(memDC_, oldBmp_);
                    DeleteObject(dib_);
                    DeleteDC(memDC_);
                    memDC_ = nullptr;
                }
                if (textFont_) DeleteObject(textFont_);
                if (textBack_) DeleteObject(textBack_);
                if (standalone_) PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY: {
                Callbacks cb = std::move(callbacks_);
                if (g_editor == this) g_editor = nullptr;
                SetWindowLongPtrW(wnd_, GWLP_USERDATA, 0);
                delete this;
                if (cb.closed) cb.closed();
                return 0;
            }
        }
        return DefWindowProcW(wnd_, msg, wp, lp);
    }

    UiColors colors_;
    std::unique_ptr<Bitmap> base_, shown_;
    Callbacks callbacks_;
    bool keepPrefs_ = true;
    int imageW_ = 0, imageH_ = 0;
    HWND wnd_ = nullptr;
    ui::Ui ui_;
    double k_ = 1;
    Doc doc_;
    Tool tool_ = Tool::Arrow, lastDrawTool_ = Tool::Arrow;
    int colorIndex_ = 0, thick_ = 1;
    uint32_t custom_ = 0xFFFFFFFF;  // 0xFFFFFFFF - пипеткой ещё не брали
    Fit fit_{};
    bool drawing_ = false, confirmed_ = false, picking_ = false;
    Shape current_;
    std::wstring note_;
    std::vector<Rc> secretBoxes_;
    // перевод снимка: что идёт сейчас и какие блоки ждут перевода
    std::wstring working_;
    std::vector<ocrtext::Block> pending_;
    std::vector<Rc> occupied_;
    Rc readView_;
    int skippedSecrets_ = 0;
    // выбранная фигура и её перетаскивание
    int selected_ = -1, dragHandle_ = -1;
    bool dragging_ = false, dragMoved_ = false;
    Shape live_;
    Pt dragFrom_{};
    ULONGLONG composedAt_ = 0;
    // поле подписи
    HWND text_ = nullptr;
    WNDPROC textProc_ = nullptr;
    HFONT textFont_ = nullptr;
    HBRUSH textBack_ = nullptr;
    Pt textAt_{};
    // кадр
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int bw_ = 0, bh_ = 0;
};

// Показательная картинка для снимков разработки: «окно» с секретом, который надо скрыть
std::unique_ptr<Bitmap> DemoImage(int w, int h) {
    auto b = std::make_unique<Bitmap>(w, h, PixelFormat32bppRGB);
    Graphics g(b.get());
    LinearGradientBrush fill(PointF(0, 0), PointF((REAL)w, (REAL)h), Color(255, 38, 52, 84), Color(255, 96, 62, 92));
    g.FillRectangle(&fill, 0, 0, w, h);
    const float s = std::min(w / 1000.0f, h / 600.0f);
    FontFamily fam(kFontName);
    Font f(&fam, std::max(8.0f, 26 * s), FontStyleBold, UnitPixel);
    SolidBrush white(Color(255, 240, 240, 245));
    g.DrawString(L"token: ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK", -1, &f, PointF(90 * s, 90 * s), &white); // выдуманный образец, gitleaks:allow
    g.DrawString(L"Всё работает как надо", -1, &f, PointF(90 * s, 320 * s), &white);
    SolidBrush panel(Color(255, 28, 30, 40));
    g.FillRectangle(&panel, 560 * s, 90 * s, 350 * s, 150 * s);
    return b;
}

bool SaveFile(Bitmap* b, const std::wstring& file) {
    return !file.empty() && snipimg::WriteBytes(file, snipimg::ToPng(b));
}

}  // namespace

void Open(const UiColors& colors, const std::vector<uint8_t>& png, Callbacks callbacks) {
    if (g_editor) {  // одно окно на всех: открытые правки не теряются
        g_editor->ShowWindowToFront();
        return;
    }
    std::unique_ptr<Bitmap> image = snipimg::FromPng(png);
    if (!image) return;
    auto* e = new Editor(colors, std::move(image), std::move(callbacks), true);
    g_editor = e;
    if (!e->Create(true)) {
        g_editor = nullptr;
        delete e;
        return;
    }
    e->ScanForSecrets(png);
}

bool IsOpen() { return g_editor != nullptr; }

void Close() {
    if (g_editor) g_editor->CloseNow();
}

int DemoSnap(const UiColors& colors, const std::wstring& frameFile, const std::wstring& flatFile, int imageW,
             int imageH) {
    auto* e = new Editor(colors, DemoImage(imageW, imageH), {}, false);
    g_editor = e;
    if (!e->Create(false)) return 1;
    Bitmap frame(1180, 780, PixelFormat32bppRGB);
    e->PaintInto(frame);  // сначала кадр: фигурам нужен масштаб снимка в окне
    e->AddDemoShapes();
    for (int i = 0; i < 40; ++i) {  // анимации считаются по времени: панель должна успеть встать на место
        e->PaintInto(frame);
        Sleep(20);
    }
    auto flat = e->Flatten();
    bool ok = SaveFile(&frame, frameFile) && SaveFile(flat.get(), flatFile);
    e->CloseNow();
    return ok ? 0 : 1;
}

int DemoTranslate(const UiColors& colors, const std::wstring& imageFile, const std::wstring& frameFile, const std::wstring& flatFile) {
    Bitmap src(imageFile.c_str());
    if (src.GetLastStatus() != Ok) return 2;
    auto copy = std::make_unique<Bitmap>((int)src.GetWidth(), (int)src.GetHeight(), PixelFormat32bppRGB);
    {
        Graphics g(copy.get());
        g.DrawImage(&src, 0, 0, (int)src.GetWidth(), (int)src.GetHeight());
    }
    auto* e = new Editor(colors, std::move(copy), {}, false);
    g_editor = e;
    if (!e->Create(false)) return 1;
    Bitmap frame(1280, 860, PixelFormat32bppRGB);
    e->PaintInto(frame);
    e->DemoStartTranslate();
    const ULONGLONG stop = GetTickCount64() + 200000;
    while (e->DemoWorking() && GetTickCount64() < stop) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(15);
    }
    for (int i = 0; i < 30; ++i) {
        e->PaintInto(frame);
        Sleep(20);
    }
    auto flat = e->Flatten();
    bool ok = SaveFile(&frame, frameFile) && SaveFile(flat.get(), flatFile) && !e->DemoWorking();
    snipimg::WriteBytes(flatFile + L".note.txt", [&] {
        std::string u = "\xEF\xBB\xBF" + std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, e->DemoNote().c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string body((size_t)std::max(0, n - 1), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, e->DemoNote().c_str(), -1, body.data(), n, nullptr, nullptr);
        u += body;
        return std::vector<uint8_t>(u.begin(), u.end());
    }());
    e->CloseNow();
    return ok ? 0 : 1;
}

int DemoWindow(const UiColors& colors, const std::wstring& flatFile) {
    Callbacks cb;
    cb.done = [flatFile](std::unique_ptr<Bitmap> b, Finish) { SaveFile(b.get(), flatFile); };
    auto* e = new Editor(colors, DemoImage(1000, 600), std::move(cb), false);
    e->standalone_ = true;
    g_editor = e;
    if (!e->Create(true)) return 1;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

}  // namespace markup
