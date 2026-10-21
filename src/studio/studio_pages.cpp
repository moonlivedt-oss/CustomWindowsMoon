// Разделы «Поведение», «Стол», «Клавиши».
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "core/files.h"
#include "core/edgecheck.h"
#include "core/fx.h"
#include "core/perf.h"
#include "app/autostart.h"
#include "app/settingsio.h"
#include "core/text.h"
#include "desk/corners.h"
#include "gfx/accent.h"
#include "gfx/draw.h"
#include "gfx/icons.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

void Studio::BehaviorPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Поведение", L"Где и как открывается меню. Всё применяется сразу.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));
    auto card = [&](const wchar_t* title, float rows, auto&& body) { cols.Card(title, rows, body); };
    // справа только переключатель или короткий ползунок - подписи можно почти всю строку
    auto row = [&](const RectF& b, float n, const wchar_t* label) { return cols.Row(b, n, label, 0.86f); };
    auto toggle = [&](const RectF& rr, int id, bool& v) {
        bool val = v;
        if (ui.Toggle(id, RectF(rr.X + rr.Width - ui.S(42), rr.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };
    std::wstring edgeWarn = edgecheck::Conflict();
    card(L"Где открывать", edgeWarn.empty() ? 3.0f : 3.4f, [&](const RectF& b) {
        toggle(row(b, 0, L"Только когда активен рабочий стол"), ID_DESKTOP_ONLY, cfg.desktopOnly);
        toggle(row(b, 1, L"Не мешать играм и видео на весь экран"), ID_SKIP_FULL, cfg.skipFullscreen);
        toggle(row(b, 2, L"Край между мониторами: задержать там курсор с Alt"), ID_INNER_EDGE, cfg.innerEdges);
        if (!edgeWarn.empty()) cols.Note(b, 3, edgeWarn, c.warn);
    });
    card(L"Вид", 3, [&](const RectF& b) {
        toggle(row(b, 0, L"Стекло под кольцом"), ID_BLUR, cfg.blur);
        RectF s = row(b, 1, L"Размер колец");
        int sc = (int)std::lround(cfg.scale * 100);
        ui.Text(std::to_wstring(sc) + L"%", RectF(s.X + s.Width - ui.S(52), s.Y, ui.S(52), s.Height), c.muted, 12,
                false, ui::Right);
        if (ui.Slider(ID_SCALE, RectF(s.X + s.Width * 0.5f, s.Y, s.Width * 0.5f - ui.S(60), s.Height), sc, 60, 200,
                      5)) {
            FieldEdit(ID_SCALE);
            cfg.scale = sc / 100.0;
        }
        RectF m = row(b, 2, L"Пунктов во втором кольце");
        RectF plus(m.X + m.Width - ui.S(36), m.Y + ui.S(5), ui.S(36), ui.S(34));
        RectF minus(plus.X - ui.S(90), plus.Y, ui.S(36), ui.S(34));
        if (ui.Button(ID_MAXCH_MINUS, minus, L"−") && cfg.maxChildren > 4) {
            Change();
            --cfg.maxChildren;
            Save();
        }
        ui.Text(std::to_wstring(cfg.maxChildren), RectF(minus.X + minus.Width, m.Y, ui.S(54), m.Height), c.text, 14,
                true, ui::Center);
        if (ui.Button(ID_MAXCH_PLUS, plus, L"+") && cfg.maxChildren < 24) {
            Change();
            ++cfg.maxChildren;
            Save();
        }
    });
    card(L"Звук", 2, [&](const RectF& b) {
        toggle(row(b, 0, L"Тихие звуки при открытии и наведении"), ID_SOUNDS, cfg.sounds);
        RectF s = row(b, 1, L"Громкость");
        int v = cfg.volume;
        ui.Text(std::to_wstring(v) + L"%", RectF(s.X + s.Width - ui.S(52), s.Y, ui.S(52), s.Height), c.muted, 12, false,
                ui::Right);
        if (ui.Slider(ID_VOLUME, RectF(s.X + s.Width * 0.5f, s.Y, s.Width * 0.5f - ui.S(60), s.Height), v, 0, 100, 5)) {
            FieldEdit(ID_VOLUME);
            cfg.volume = v;
        }
    });
    card(L"Действия", 1, [&](const RectF& b) {
        RectF s = row(b, 0, L"Терминал по Ctrl");
        int sel = _wcsicmp(cfg.terminal.c_str(), L"powershell") == 0 ? 1
                  : _wcsicmp(cfg.terminal.c_str(), L"cmd") == 0      ? 2
                                                                     : 0;
        if (ui.Segmented(ID_TERMINAL, RectF(s.X + s.Width * 0.45f, s.Y + ui.S(3), s.Width * 0.55f, ui.S(38)),
                         {L"Terminal", L"PowerShell", L"cmd"}, sel)) {
            Change();
            cfg.terminal = sel == 1 ? L"powershell" : sel == 2 ? L"cmd" : L"wt";
            Save();
        }
    });
    card(L"Перенос на другой ПК", 2.4f, [&](const RectF& b) {
        RectF a = row(b, 0, L"Все настройки одним файлом");
        auto pick = [this](bool save) {
            wchar_t file[MAX_PATH] = L"RadialMenu.moonradial";
            OPENFILENAMEW of{sizeof(of)};
            of.hwndOwner = wnd;
            of.lpstrFilter = L"Настройки RadialMenu (*.moonradial)\0*.moonradial\0Все файлы\0*.*\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH;
            of.lpstrDefExt = L"moonradial";
            of.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
            if (!(save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of))) return std::wstring();
            return std::wstring(file);
        };
        if (ui.Button(ID_SETTINGS_EXPORT, RectF(a.X + a.Width - ui.S(290), a.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Сохранить"))
            Defer([this, pick] {
                std::wstring f = pick(true), msg;
                if (f.empty()) return;
                settingsio::Export(f, msg);
                Toast(msg);
            });
        if (ui.Button(ID_SETTINGS_IMPORT, RectF(a.X + a.Width - ui.S(132), a.Y + ui.S(4), ui.S(132), ui.S(36)),
                      L"Загрузить"))
            Defer([this, pick] {
                std::wstring f = pick(false), msg;
                if (f.empty()) return;
                Save();  // несохранённое - сначала в файл, чтобы оно попало в копию перед загрузкой
                if (settingsio::Import(f, msg)) {
                    Config fresh;
                    if (LoadConfig(cfgPath, fresh)) {
                        Change();
                        cfg = std::move(fresh);
                        Save();  // основная программа перечитает
                    }
                }
                Toast(msg);
            });
        cols.Note(b, 1.0f,
                  L"Кольца, правила, профили Boost и «Это моё» - в файл .moonradial. Загрузка кладёт нынешние в "
                  L"backups. Пути старого ПК, которых здесь нет, кольцо покажет серым.",
                  c.faint);
    });
    card(L"Система", 2, [&](const RectF& b) {
        RectF a = row(b, 0, L"Запускать вместе с Windows");
        bool on = AutostartOn();
        if (ui.Toggle(ID_AUTOSTART, RectF(a.X + a.Width - ui.S(42), a.Y + ui.S(7), ui.S(42), ui.S(30)), on)) {
            SetAutostart(on);
            Log(!on                ? L"Автозапуск выключен"
                : AutostartEarly() ? L"Автозапуск включён: задача планировщика, сразу после входа"
                                   : L"Автозапуск включён (через Run: планировщик недоступен)");
        }
        RectF f = row(b, 1, L"Файлы");
        if (ui.Button(ID_OPEN_CFG, RectF(f.X + f.Width - ui.S(290), f.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"radial.cfg")) {
            std::wstring p = cfgPath;
            Defer([p] {
                ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            });
        }
        if (ui.Button(ID_OPEN_DIR, RectF(f.X + f.Width - ui.S(132), f.Y + ui.S(4), ui.S(132), ui.S(36)), L"Папка"))
            Defer([] { ShellExecuteW(nullptr, nullptr, ExeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    });
    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

void Studio::DeskPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Стол", L"Горячие углы и панель «Сейчас играет». Всё применяется сразу.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    float w = std::min(view.Width - ui.S(8), ui.S(900)), x = view.X + (view.Width - w) / 2, y = view.Y - off, y0 = y;

    // горячие углы: слева схема экрана, справа по три действия на угол
    float rowH = ui.S(50);
    RectF card(x, y, w, ui.S(52) + ui.S(26) + 5 * rowH + ui.S(8));
    Card(card);
    ui.Text(L"Горячие углы", RectF(card.X + ui.S(18), card.Y + ui.S(14), w, ui.S(24)), c.text, 14, true);
    ui.Text(L"Задержи курсор в углу, толкни в него дважды или покрути колесо. Alt + край по-прежнему открывает кольцо.",
            RectF(card.X + ui.S(150), card.Y + ui.S(16), w - ui.S(170), ui.S(20)), c.muted, 11);
    // схема экрана живая: курсор едет в угол строки под мышью и показывает жест - задержку,
    // двойной толчок или колесо; точка угла сама открывает выбор действия
    RectF scr(card.X + ui.S(22), card.Y + ui.S(96), ui.S(104), ui.S(66));
    ui.Fill(scr, c.field, ui.S(6));
    ui.Stroke(scr, c.border, ui.S(6), ui.S(1.5f));
    ui.Fill(RectF(scr.X + ui.S(40), scr.Y + scr.Height, ui.S(24), ui.S(10)), c.border);
    ui.Fill(RectF(scr.X + ui.S(30), scr.Y + scr.Height + ui.S(9), ui.S(44), ui.S(4)), c.border, ui.S(2));
    // и нижний край, где выезжает «Сейчас играет»
    if (cfg.media)
        ui.Fill(RectF(scr.X + ui.S(22), scr.Y + scr.Height - ui.S(4), scr.Width - ui.S(44), ui.S(3)), c.accent2,
                ui.S(1.5f));
    RectF body(card.X + ui.S(150), card.Y + ui.S(48), w - ui.S(168), ui.S(26) + 5 * rowH);
    float labelW = ui.S(132), colGap = ui.S(8);
    float colW = (body.Width - labelW - colGap * 2) / 3;
    const wchar_t* heads[3] = {L"Задержать", L"Толкнуть дважды", L"Колесо"};
    for (int k2 = 0; k2 < 3; ++k2)
        ui.Text(heads[k2], RectF(body.X + labelW + k2 * (colW + colGap) + ui.S(4), body.Y, colW, ui.S(20)), c.faint,
                11);
    const wchar_t* names[4] = {L"Левый верхний", L"Правый верхний", L"Левый нижний", L"Правый нижний"};
    POINT m = ui.Mouse();
    int showCorner = -1, showKind = 0;
    for (int i = 0; i < 4; ++i) {
        bool hot = false, set = !cfg.corners[i].empty() || !cfg.corners2[i].empty() || !cfg.cornerWheel[i].empty();
        RectF rr(body.X, body.Y + ui.S(26) + i * rowH, body.Width, ui.S(44));
        RectF band(rr.X - ui.S(8), rr.Y - ui.S(2), rr.Width + ui.S(16), rr.Height + ui.S(4));
        bool over = ui.interactive && m.x >= band.X && m.x < band.GetRight() && m.y >= band.Y && m.y < band.GetBottom();
        float rh = ui.Anim(ID_CORNER + 16 + i, over || ui.Hot(ID_CORNER + i * 4 + 3), 16);
        if (rh > 0.01f) ui.Fill(band, WithAlpha(c.cardHi, (BYTE)(150 * rh)), ui.S(10));
        ui.Text(names[i], RectF(rr.X, rr.Y, labelW, rr.Height), c.text, 12.5f);
        std::wstring* vals[3] = {&cfg.corners[i], &cfg.corners2[i], &cfg.cornerWheel[i]};
        for (int k2 = 0; k2 < 3; ++k2) {
            int id = ID_CORNER + i * 4 + k2;
            RectF b(rr.X + labelW + k2 * (colW + colGap), rr.Y + ui.S(4), colW, ui.S(36));
            bool capturing = captureCorner_ == i && captureKind_ == k2;
            std::wstring text = capturing ? L"Нажми сочетание" : corners::ActionName(*vals[k2]) + L"  ▾";
            if (ui.Button(id, b, text, capturing ? Kind::Primary : Kind::Secondary, L"", capturing) && !capturing)
                Defer([this, i, k2] { CornerMenu(i, k2); });
            ui.Tip(id, capturing ? L"Esc - отмена" : L"Выбрать действие");
            if (ui.Hot(id)) showKind = k2;
            hot |= ui.Hot(id);
        }
        if (over || hot || ui.Hot(ID_CORNER + i * 4 + 3)) showCorner = i;
        (void)set;
    }
    // точки углов: наведение растит, щелчок - выбор действия «задержать»
    auto cornerPt = [&](int i) {
        return PointF(i % 2 ? scr.X + scr.Width : scr.X, i < 2 ? scr.Y : scr.Y + scr.Height);
    };
    Graphics& g = ui.G();
    for (int i = 0; i < 4; ++i) {
        int did = ID_CORNER + i * 4 + 3;
        PointF p = cornerPt(i);
        if (ui.Click(did, RectF(p.X - ui.S(13), p.Y - ui.S(13), ui.S(26), ui.S(26))))
            Defer([this, i] { CornerMenu(i, 0); });
        ui.Tip(did, std::wstring(names[i]) + L": задержать - " + corners::ActionName(cfg.corners[i]));
        bool set = !cfg.corners[i].empty() || !cfg.corners2[i].empty() || !cfg.cornerWheel[i].empty();
        float hv = ui.Anim(did, showCorner == i, 16);
        float d = ui.S(12) + ui.S(5) * hv;
        if (hv > 0.01f) {
            SolidBrush halo(WithAlpha(c.accent, (BYTE)(60 * hv)));
            float hd = d + ui.S(10) * hv;
            g.FillEllipse(&halo, p.X - hd / 2, p.Y - hd / 2, hd, hd);
        }
        ui.Fill(RectF(p.X - d / 2, p.Y - d / 2, d, d), set ? c.accent : c.faint, d / 2);
        ui.Ripple(did, RectF(p.X - d / 2, p.Y - d / 2, d, d), d / 2);
    }
    // курсор: без наведения отдыхает в середине экрана
    PointF aim(scr.X + scr.Width * 0.46f, scr.Y + scr.Height * 0.42f);
    if (showCorner >= 0) {
        PointF p = cornerPt(showCorner);
        aim = PointF(p.X + (showCorner % 2 ? -ui.S(3) : ui.S(3)), p.Y + (showCorner < 2 ? ui.S(3) : -ui.S(3)));
    }
    RectF cur = ui.Glide(-300, RectF(aim.X - scr.X, aim.Y - scr.Y, 0, 0));  // внутри экрана: прокрутка не тянет
    float px = scr.X + cur.X, py = scr.Y + cur.Y;
    ULONGLONG now = GetTickCount64();
    bool arrived = showCorner >= 0 && std::fabs(px - aim.X) + std::fabs(py - aim.Y) < ui.S(2);
    if (arrived && !fx::Game()) {
        ui.Tick();
        PointF p = cornerPt(showCorner);
        float sx = showCorner % 2 ? 1.0f : -1.0f, sy = showCorner < 2 ? -1.0f : 1.0f;
        if (showKind == 0) {
            // задержка: вокруг угла набегает кольцо ровно за cornerDelay, потом вспышка
            double delay = std::max(100, cfg.cornerDelay), period = delay + 700;
            double t = std::fmod((double)now, period);
            float prog = (float)std::min(1.0, t / delay);
            Pen ring(c.gold, ui.S(2.5f));
            ring.SetStartCap(LineCapRound);
            ring.SetEndCap(LineCapRound);
            float rr = ui.S(13);
            g.DrawArc(&ring, p.X - rr, p.Y - rr, rr * 2, rr * 2, -90, 360 * prog);
            if (t > delay) {
                float f = (float)((t - delay) / 700);
                float fr = rr + ui.S(14) * f;
                Pen flash(WithAlpha(c.accent, (BYTE)(220 * (1 - f))), ui.S(2) * (1 - f) + ui.S(0.5f));
                g.DrawEllipse(&flash, p.X - fr, p.Y - fr, fr * 2, fr * 2);
            }
        } else if (showKind == 1) {
            // двойной толчок: курсор дважды упирается в угол
            double t = std::fmod((double)now, 1100.0) / 1100.0;
            float bump = 0;
            for (double at : {0.1, 0.36})
                if (t > at && t < at + 0.14) bump = (float)std::sin((t - at) / 0.14 * 3.14159265);
            px += sx * ui.S(4) * bump;
            py += sy * ui.S(4) * bump;
            if (bump > 0.05f) {
                Pen hit(WithAlpha(c.accent, (BYTE)(200 * bump)), ui.S(2));
                float hr = ui.S(9) + ui.S(6) * bump;
                g.DrawEllipse(&hit, p.X - hr, p.Y - hr, hr * 2, hr * 2);
            }
        } else {
            // колесо: две дуги крутятся вокруг угла
            float a = (float)std::fmod(now / 4.0, 360.0);
            Pen arc(c.gold, ui.S(2.2f));
            arc.SetStartCap(LineCapRound);
            arc.SetEndCap(LineCapArrowAnchor);
            float rr = ui.S(14);
            g.DrawArc(&arc, p.X - rr, p.Y - rr, rr * 2, rr * 2, a, 70);
            g.DrawArc(&arc, p.X - rr, p.Y - rr, rr * 2, rr * 2, a + 180, 70);
        }
    }
    // стрелка курсора остриём в (px, py)
    {
        float k = ui.S(0.95f);
        PointF arrow[7] = {{0, 0}, {0, 15}, {4, 11.5f}, {6.8f, 17.5f}, {9.2f, 16.4f}, {6.4f, 10.6f}, {11.5f, 10.6f}};
        for (PointF& q : arrow) q = PointF(px + q.X * k, py + q.Y * k);
        SolidBrush fillA(Color(255, 250, 250, 250));
        Pen edge(Color(255, 20, 20, 24), ui.S(1.2f));
        edge.SetLineJoin(LineJoinRound);
        g.FillPolygon(&fillA, arrow, 7);
        g.DrawPolygon(&edge, arrow, 7);
    }
    RectF s(body.X, body.Y + ui.S(26) + 4 * rowH, body.Width, ui.S(44));
    ui.Text(L"Задержка в углу", RectF(s.X, s.Y, labelW, s.Height), c.text, 12.5f);
    int dl = cfg.cornerDelay;
    ui.Text(std::to_wstring(dl) + L" мс", RectF(s.X + s.Width - ui.S(64), s.Y, ui.S(64), s.Height), c.muted, 12, false,
            ui::Right);
    if (ui.Slider(ID_CORNER_DELAY, RectF(s.X + labelW, s.Y, s.Width - labelW - ui.S(72), s.Height), dl, 100, 1000,
                  50)) {
        FieldEdit(ID_CORNER_DELAY);
        cfg.cornerDelay = dl;
    }
    y += card.Height + ui.S(16);

    // панель «Сейчас играет»
    const wchar_t* mediaNote =
        L"Доведи курсор до нижнего края экрана (не в угол): снизу выедет карточка с треком из любого плеера. "
        L"Колесо над ней - громкость плеера, кнопка со строками - текст песни (lrclib.net), клик по строке "
        L"перематывает. Если играют сразу несколько программ, сверху появится переключатель.";
    float noteH = ui.MeasureWrapped(mediaNote, 11.5f, false, w - ui.S(36)) + ui.S(4);  // высота по тексту
    RectF mc(x, y, w, ui.S(52) + 2 * rowH + noteH + ui.S(20));
    Card(mc);
    ui.Text(L"Сейчас играет", RectF(mc.X + ui.S(18), mc.Y + ui.S(14), w, ui.S(24)), c.text, 14, true);
    RectF mb(mc.X + ui.S(18), mc.Y + ui.S(48), w - ui.S(36), 2 * rowH);
    auto toggle = [&](const RectF& base, int n, const wchar_t* label, int id, bool& v) {
        RectF t(base.X, base.Y + n * rowH, base.Width, ui.S(44));
        ui.Text(label, RectF(t.X, t.Y, t.Width * 0.7f, t.Height), c.text, 12.5f);
        bool val = v;
        if (ui.Toggle(id, RectF(t.X + t.Width - ui.S(42), t.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };
    toggle(mb, 0, L"Панель у нижнего края", ID_MEDIA, cfg.media);
    toggle(mb, 1, L"Показывать трек при смене", ID_MEDIA_TOAST, cfg.mediaToast);
    ui.Text(mediaNote, RectF(mb.X, mb.Y + 2 * rowH, mb.Width, noteH), c.muted, 11.5f, false, ui::Left, true);
    y += mc.Height + ui.S(16);

    // окна: прозрачность и «поверх всех»
    RectF wc(x, y, w, ui.S(52) + rowH + ui.S(118));
    Card(wc);
    ui.Text(L"Окна", RectF(wc.X + ui.S(18), wc.Y + ui.S(14), w, ui.S(24)), c.text, 14, true);
    RectF wb(wc.X + ui.S(18), wc.Y + ui.S(48), w - ui.S(36) - ui.S(150), rowH);
    toggle(wb, 0, L"Прозрачность и «поверх всех»", ID_WINFX, cfg.winFx);
    // шпаргалка: клавиши слева, что делают - справа; рядом маленькое окно с рамкой цвета темы
    struct Hint {
        std::vector<std::wstring> keys;
        const wchar_t* text;
    } hints[] = {{{L"Alt", L"+", L"колесо"}, L"над верхней полосой окна - прозрачность, от 20% до 100%"},
                 {{L"Alt", L"+", L"средняя кнопка"}, L"там же - закрепить поверх всех, окно получит рамку цвета темы"}};
    float hy = wc.Y + ui.S(48) + rowH + ui.S(6);
    for (auto& h : hints) {
        float kx = wc.X + ui.S(18);
        for (const auto& k2 : h.keys) {
            if (k2 == L"+") {
                ui.Text(k2, RectF(kx, hy, ui.S(16), ui.S(32)), c.faint, 13, true, ui::Center);
                kx += ui.S(18);
                continue;
            }
            float kw = ui.KeycapWidth(k2);
            ui.Keycap(RectF(kx, hy, kw, ui.S(32)), k2);
            kx += kw + ui.S(6);
        }
        ui.Text(h.text, RectF(wc.X + ui.S(250), hy, w - ui.S(400), ui.S(32)), c.muted, 11.5f);
        hy += ui.S(42);
    }
    // набросок закреплённого окна
    RectF win(wc.X + w - ui.S(130), wc.Y + ui.S(70), ui.S(104), ui.S(70));
    ui.Fill(win, c.field, ui.S(8));
    ui.Fill(RectF(win.X, win.Y, win.Width, ui.S(14)), c.cardHi, ui.S(8));
    ui.Stroke(win, c.accent, ui.S(8), ui.S(2));
    ui.Fill(RectF(win.X + ui.S(10), win.Y + ui.S(24), ui.S(60), ui.S(5)), c.border, ui.S(2.5f));
    ui.Fill(RectF(win.X + ui.S(10), win.Y + ui.S(36), ui.S(80), ui.S(5)), c.border, ui.S(2.5f));
    ui.Fill(RectF(win.X + ui.S(10), win.Y + ui.S(48), ui.S(44), ui.S(5)), c.border, ui.S(2.5f));
    ui.Fill(RectF(win.X + win.Width - ui.S(14), win.Y - ui.S(6), ui.S(16), ui.S(16)), c.accent, ui.S(8));
    y += wc.Height + ui.S(10);

    pageContentH_ = y - y0;
    ui.G().Restore(st);
}

void Studio::CornerMenu(int i, int kind) {
    int n = 0;
    const corners::Action* acts = kind == 2 ? corners::WheelActions(n) : corners::Actions(n);
    std::wstring& slot = kind == 0 ? cfg.corners[i] : kind == 1 ? cfg.corners2[i] : cfg.cornerWheel[i];
    std::wstring cur = slot;
    bool custom = !cur.empty();
    HMENU m = CreatePopupMenu();
    for (int a = 0; a < n; ++a) {
        bool chk = cur == acts[a].key;
        if (chk) custom = false;
        AppendMenuW(m, MF_STRING | (chk ? MF_CHECKED : 0), 1 + a, acts[a].name);
        if (a == 0) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    if (kind != 2) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (custom) AppendMenuW(m, MF_STRING | MF_CHECKED | MF_DISABLED, 999, corners::ActionName(cur).c_str());
        AppendMenuW(m, MF_STRING, 1001, L"Сочетание клавиш...");
        AppendMenuW(m, MF_STRING, 1000, L"Программа или файл...");
    }
    POINT p;
    GetCursorPos(&p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, wnd, nullptr);
    DestroyMenu(m);
    std::wstring val;
    if (cmd >= 1 && cmd <= n) {
        val = acts[cmd - 1].key;
    } else if (cmd == 1000) {
        val = PickPath(false);
        if (val.empty()) return;
    } else if (cmd == 1001) {
        captureCorner_ = i;  // следующее сочетание в окне студии запишется в этот угол
        captureKind_ = kind;
        SetFocus(wnd);
        ui.Invalidate();
        return;
    } else {
        return;
    }
    SetCorner(i, kind, val);
}

void Studio::SetCorner(int i, int kind, const std::wstring& val) {
    std::wstring& slot = kind == 0 ? cfg.corners[i] : kind == 1 ? cfg.corners2[i] : cfg.cornerWheel[i];
    if (val == slot) return;
    static const wchar_t* names[4] = {L"левый верхний", L"правый верхний", L"левый нижний", L"правый нижний"};
    static const wchar_t* kinds[3] = {L"", L", дважды", L", колесо"};
    Change();
    slot = val;
    Log(std::wstring(L"Угол ") + names[i] + kinds[kind] + L": " + corners::ActionName(val));
    Save();
}

// Запись сочетания для угла: модификаторы копятся, первая обычная клавиша завершает запись.
bool Studio::CaptureKey(WPARAM vk) {
    if (captureCorner_ < 0) return false;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return true;
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000,
         alt = GetKeyState(VK_MENU) & 0x8000, win = (GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000;
    int i = captureCorner_, kind = captureKind_;
    captureCorner_ = -1;
    InvalidateRect(wnd, nullptr, FALSE);
    if (vk == VK_ESCAPE && !ctrl && !shift && !alt && !win) return true;  // отмена
    std::wstring v = L"keys:";
    if (ctrl) v += L"ctrl+";
    if (shift) v += L"shift+";
    if (alt) v += L"alt+";
    if (win) v += L"win+";
    v += corners::KeyName((UINT)vk);
    SetCorner(i, kind, v);
    return true;
}

void Studio::KeysPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(in, L"Клавиши", L"Всё, что умеет меню, на одной странице.");
    ui.Icon(ExeDir() + L"\\icons\\mascot-read.webp",
            RectF(in.X + in.Width - ui.S(84), in.Y - ui.S(6), ui.S(80), ui.S(80)));
    RectF view(in.X - ui.S(4), in.Y + ui.S(76), in.Width + ui.S(8), in.Height - ui.S(76));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    struct K {
        std::vector<std::wstring> keys;
        const wchar_t* title;
        const wchar_t* text;
    } list[] = {
        {{L"Alt", L"+", L"край экрана"}, L"Открыть кольцо", L"левый и правый край, у каждого свой набор"},
        {{L"клик", L"+", L"клик"}, L"Открыть", L"двойной клик или Enter; один клик только выбирает"},
        {{L"тянуть"}, L"Вращать кольцо", L"первое крутится по кругу, второе прокручивается, если не влезло"},
        {{L"Shift", L"+", L"клик"}, L"Открыть в VS Code", L"папку или файл"},
        {{L"Ctrl", L"+", L"клик"}, L"Терминал в папке", L"Windows Terminal, PowerShell или cmd"},
        {{L"Ctrl", L"+", L"Shift", L"+", L"клик"}, L"Показать в Проводнике", L"с выделенным файлом"},
        {{L"ПКМ"}, L"Зайти в папку", L"сверху появится путь, ПКМ мимо - назад"},
        {{L"Колесо"}, L"Прокрутить второе кольцо", L"когда пунктов больше, чем влезает"},
        {{L"абв"}, L"Поиск", L"просто печатай, раскладка не важна"},
        {{L"Enter"}, L"Открыть найденное", L"стрелки выбирают из результатов"},
        {{L"Esc"}, L"Назад или закрыть", L"сначала очищает поиск"},
        {{L"файл", L"→", L"сектор"}, L"Перенести файл", L"на папку - перенос, на программу - открыть в ней"},
        {{L"угол"}, L"Кольцо сети", L"действие угла: починка, DNS, пинг, прокси-процессы"},
    };
    float x = view.X + ui.S(4), w = view.Width - ui.S(8), y0 = view.Y - off;
    float gap = ui.S(14);
    int cols = w > ui.S(760) ? 2 : 1;
    float cw = (w - gap * (cols - 1)) / cols, ch = ui.S(104);
    int n = (int)(sizeof(list) / sizeof(list[0]));
    for (int i = 0; i < n; ++i) {
        RectF cr(x + (i % cols) * (cw + gap), y0 + (i / cols) * (ch + gap), cw, ch);
        Card(cr);
        float kx = cr.X + ui.S(16), ky = cr.Y + ui.S(16);
        for (const auto& k2 : list[i].keys) {
            if (k2 == L"+" || k2 == L"→") {
                ui.Text(k2, RectF(kx, ky, ui.S(18), ui.S(34)), c.faint, 14, true, ui::Center);
                kx += ui.S(20);
                continue;
            }
            float kw = ui.KeycapWidth(k2);
            ui.Keycap(RectF(kx, ky, kw, ui.S(36)), k2);
            kx += kw + ui.S(6);
        }
        ui.Text(list[i].title, RectF(cr.X + ui.S(16), cr.Y + ui.S(58), cr.Width - ui.S(32), ui.S(22)), c.text, 13.5f,
                true);
        ui.Text(list[i].text, RectF(cr.X + ui.S(16), cr.Y + ui.S(78), cr.Width - ui.S(32), ui.S(20)), c.muted, 11.5f);
    }
    pageContentH_ = ((n + cols - 1) / cols) * (ch + gap);
    ui.G().Restore(st);
}

}  // namespace studio
