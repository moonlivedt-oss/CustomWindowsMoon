// Раздел «Система»: значки папок и центр уведомлений (через моды Windhawk), фон Пуска, курсор,
// свечение заряда, ночной режим, двойной клик по столу и карточка снимка.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/bg.h"
#include "core/files.h"
#include "core/themes.h"
#include "desk/icontheme.h"
#include "desk/startbg.h"
#include "desk/whreg.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

// задача держит своё владение (shared_ptr в bg::Run): студию могут закрыть, пока тема собирается
void IconWork(IconJob* job) {
    std::wstring out, msg;
    bool ok = icontheme::Build(job->body, job->accent, job->name, out, msg, [job](int pct) {
        job->progress = pct;
        InvalidateRect(job->wnd, nullptr, FALSE);
    });
    AcquireSRWLockExclusive(&job->lock);
    job->ok = ok;
    job->out = out;
    job->message = msg;
    ReleaseSRWLockExclusive(&job->lock);
    job->done = true;
    InvalidateRect(job->wnd, nullptr, FALSE);
}

std::wstring HexColor(COLORREF c) {
    wchar_t b[8];
    swprintf(b, 8, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

}  // namespace

void Studio::SystemPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Система",
           L"Чтобы Windows выглядела одной темой с кольцами: значки, Пуск, курсор и мелочи.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));
    float rowH = cols.RowH();
    auto card = [&](const wchar_t* title, float rows, auto&& body) { cols.Card(title, rows, body); };
    auto row = [&](const RectF& b, float n, const wchar_t* label) { return cols.Row(b, n, label, 0.55f); };
    ThemeInfo t = CurrentTheme(cfg);
    auto toggle = [&](const RectF& rr, int id, bool& v) {
        bool val = v;
        if (ui.Toggle(id, RectF(rr.X + rr.Width - ui.S(42), rr.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };
    auto note = [&](const RectF& b, float n, float, const std::wstring& s, Color col) { cols.Note(b, n, s, col); };

    // всё оформление системы одной кнопкой: моды Windhawk одним .reg и Пуск Start11
    card(L"Тема везде", 1.9f, [&](const RectF& b) {
        RectF rr = row(b, 0, L"Применить тему ко всему");
        if (ui.Button(ID_SYS_ALL, RectF(rr.X + rr.Width - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Применить")) {
            std::wstring mods, msg;
            std::wstring reg = whreg::AllReg(t.bg, t.panel, t.ac, &mods);
            whreg::Run(reg);
            if (startbg::Installed()) startbg::Apply(t, msg);
            sysStatus_ =
                (reg.empty() ? L"Модов Windhawk для оформления нет" : L"Подтверди изменение реестра: " + mods) +
                (startbg::Installed() ? L". Пуск обновлён, Проводник перезапущен" : L"");
        }
        note(b, 1, 2,
             L"Проводник, центр уведомлений, Параметры и поиск (если стоят их моды Windhawk) и Пуск Start11. "
             L"Проводник перезапустится, открытые папки закроются.",
             c.muted);
    });

    // значки папок: собрать копию текущей темы мода и перекрасить
    card(L"Значки папок", 2.9f, [&](const RectF& b) {
        bool busy = iconJob_ && !iconJob_->done;
        RectF rr = row(b, 0, busy ? L"Собираю тему..." : L"Папки в цвет темы");
        if (busy) {
            RectF bar(rr.X + rr.Width * 0.5f, rr.Y + ui.S(19), rr.Width * 0.5f, ui.S(6));
            ui.Fill(bar, c.field, ui.S(3));
            ui.Fill(RectF(bar.X, bar.Y, bar.Width * iconJob_->progress / 100.0f, bar.Height), c.accent, ui.S(3));
            ui.Invalidate();
        } else if (ui.Button(ID_SYS_ICONS, RectF(rr.X + rr.Width - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                             L"Собрать")) {
            auto job = std::make_shared<IconJob>();
            job->body = MixColor(ToColor(t.muted), ToColor(t.bg), 0.32).ToCOLORREF();
            job->accent = t.ac;
            job->name = std::wstring(L"RadialMenu ") + t.name;
            job->previous = icontheme::SourceFolder();
            job->wnd = wnd;
            iconJob_ = job;
            if (!bg::Run([job] { IconWork(job.get()); })) job->done = true;
        }
        bool ready = iconJob_ && iconJob_->done && iconJob_->ok;
        RectF ar(b.X, b.Y + rowH, b.Width, ui.S(44));  // без подписи: обе кнопки целиком
        if (ui.Button(ID_SYS_ICONS_APPLY, RectF(ar.X + ar.Width - ui.S(150), ar.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Применить", ready ? Kind::Primary : Kind::Secondary) &&
            ready) {
            whreg::Run(whreg::IconThemeReg(iconJob_->out));
            sysStatus_ = L"Подтверди изменение реестра, затем выключи и включи мод Resource Redirect в Windhawk";
        }
        if (ui.Button(ID_SYS_ICONS_BACK, RectF(ar.X, ar.Y + ui.S(4), ui.S(160), ui.S(36)), L"Вернуть прежние",
                      Kind::Ghost) &&
            iconJob_ && !iconJob_->previous.empty()) {
            whreg::Run(whreg::IconThemeRestoreReg(iconJob_->previous));
        }
        std::wstring s =
            L"Берётся тема из мода Resource Redirect, сиреневые папки становятся графитовыми, рисунки на них - "
            L"цвета акцента.";
        if (iconJob_ && iconJob_->done) s = iconJob_->message + (iconJob_->ok ? L". Готово - жми «Применить»." : L"");
        note(b, 2, 2, s, iconJob_ && iconJob_->done && !iconJob_->ok ? c.warn : c.muted);
    });

    // центр уведомлений
    card(L"Центр уведомлений", 1.9f, [&](const RectF& b) {
        RectF rr = row(b, 0, L"Стекло в цвет темы");
        if (ui.Button(ID_SYS_NOTIFY, RectF(rr.X + rr.Width - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Применить")) {
            whreg::Run(whreg::NotificationReg(t.bg, t.ac));
            sysStatus_ = L"Подтверди изменение реестра, затем выключи и включи мод Notification Center Styler";
        }
        note(b, 1, 2,
             L"Тема TintedGlass с графитовым стеклом и скруглением как у карточек; акценты берёт из цвета Windows.",
             c.muted);
    });

    // Проводник
    card(L"Проводник", 1.9f, [&](const RectF& b) {
        RectF rr = row(b, 0, L"Стекло в цвет темы");
        if (ui.Button(ID_SYS_EXPLORER, RectF(rr.X + rr.Width - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Применить")) {
            whreg::Run(whreg::ExplorerReg(t.bg, t.ac));
            sysStatus_ = L"Подтверди изменение реестра, затем выключи и включи мод File Explorer Styler";
        }
        note(b, 1, 2,
             L"Вкладки и панель - стекло с каплей акцента, адресная строка и меню с тонкой кромкой цвета темы.",
             c.muted);
    });

    // меню Пуск
    card(L"Меню Пуск", 1.9f, [&](const RectF& b) {
        RectF rr = row(b, 0, L"Фон из обоев");
        bool has = startbg::Installed();
        if (ui.Button(ID_SYS_START, RectF(rr.X + rr.Width - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Обновить") &&
            has) {
            std::wstring msg;
            startbg::Apply(t, msg);
            sysStatus_ = msg;
        }
        note(b, 1, 2,
             has ? L"Размытые обои под вуалью темы, подложка и кнопка Пуск в её цветах. Проводник перезапустится."
                 : L"Нужен Stardock Start11.",
             c.muted);
    });

    // курсор
    card(L"Курсор", 3.2f, [&](const RectF& b) {
        struct Swatch {
            const wchar_t* name;
            std::wstring value;
        } sw[] = {{L"Системный", L""},
                  {L"Акцент", HexColor(t.ac)},
                  {L"Серебро", HexColor(t.gold)},
                  {L"Сталь", HexColor(t.glow)},
                  {L"Свой...", L"?"}};
        float sx = b.X, swW = (b.Width - ui.S(8) * 4) / 5;
        for (int i = 0; i < 5; ++i) {
            RectF sr(sx + i * (swW + ui.S(8)), b.Y, swW, ui.S(58));
            int id = ID_SYS_CURSOR + i;
            bool on = sw[i].value == L"?" ? (!cfg.cursorColor.empty() && cfg.cursorColor != HexColor(t.ac) &&
                                             cfg.cursorColor != HexColor(t.gold) && cfg.cursorColor != HexColor(t.glow))
                                          : cfg.cursorColor == sw[i].value;
            if (ui.Click(id, sr)) {
                if (sw[i].value == L"?") {
                    Defer([this] {
                        COLORREF v = RGB(255, 255, 255);
                        if (!PickColor(v)) return;
                        Change();
                        cfg.cursorColor = HexColor(v);
                        Save();
                    });
                } else if (!on) {
                    Change();
                    cfg.cursorColor = sw[i].value;
                    Save();
                }
            }
            ui.Fill(sr, on ? c.cardHi : ui.Hot(id) ? c.card : c.field, ui.S(10));
            ui.Stroke(sr, on ? c.accent : c.border, ui.S(10), on ? ui.S(2) : 1);
            // маленькая стрелка-курсор нужного цвета
            COLORREF col = RGB(255, 255, 255);
            if (sw[i].value.size() == 7) {
                unsigned long v = wcstoul(sw[i].value.c_str() + 1, nullptr, 16);
                col = RGB(v >> 16 & 255, v >> 8 & 255, v & 255);
            } else if (sw[i].value == L"?" && on) {
                unsigned long v = wcstoul(cfg.cursorColor.c_str() + 1, nullptr, 16);
                col = RGB(v >> 16 & 255, v >> 8 & 255, v & 255);
            }
            float ax = sr.X + sr.Width / 2 - ui.S(6), ay = sr.Y + ui.S(8);
            PointF arrow[] = {{ax, ay},
                              {ax, ay + ui.S(18)},
                              {ax + ui.S(4.5f), ay + ui.S(14)},
                              {ax + ui.S(8), ay + ui.S(21)},
                              {ax + ui.S(10.5f), ay + ui.S(20)},
                              {ax + ui.S(7.2f), ay + ui.S(13)},
                              {ax + ui.S(12.5f), ay + ui.S(13)}};
            SolidBrush ab(ToColor(col));
            Pen ap(Color(255, 10, 10, 12), ui.S(1.2f));
            ui.G().FillPolygon(&ab, arrow, 7);
            ui.G().DrawPolygon(&ap, arrow, 7);
            ui.Text(sw[i].name, RectF(sr.X, sr.Y + ui.S(34), sr.Width, ui.S(20)), on ? c.text : c.muted, 11, on,
                    ui::Center);
        }
        RectF s = row(b, 1.35f, L"Размер");
        int sz = cfg.cursorSize;
        ui.Text(sz ? std::to_wstring(sz) + L" px" : L"как в системе",
                RectF(s.X + s.Width - ui.S(110), s.Y, ui.S(110), s.Height), c.muted, 12, false, ui::Right);
        if (ui.Slider(ID_SYS_CURSOR_SIZE, RectF(s.X + s.Width * 0.3f, s.Y, s.Width * 0.7f - ui.S(118), s.Height), sz, 0,
                      96, 8)) {
            FieldEdit(ID_SYS_CURSOR_SIZE);
            cfg.cursorSize = sz < 24 ? 0 : sz;
        }
        note(b, 2.25f, 2, L"Курсоры перекрашиваются на время работы программы; без неё Windows вернёт свои.", c.faint);
    });

    // заряд
    card(L"Заряд", 2.9f, [&](const RectF& b) {
        toggle(row(b, 0, L"Свечение у нижнего края"), ID_SYS_BATTERY, cfg.batteryGlow);
        RectF s = row(b, 1, L"Мало заряда, если меньше");
        int lo = cfg.batteryLow;
        ui.Text(std::to_wstring(lo) + L"%", RectF(s.X + s.Width - ui.S(52), s.Y, ui.S(52), s.Height), c.muted, 12,
                false, ui::Right);
        if (ui.Slider(ID_SYS_BATTERY_LOW, RectF(s.X + s.Width * 0.5f, s.Y, s.Width * 0.5f - ui.S(60), s.Height), lo, 5,
                      40, 5)) {
            FieldEdit(ID_SYS_BATTERY_LOW);
            cfg.batteryLow = lo;
        }
        RectF p = row(b, 2, L"Посмотреть");
        if (ui.Button(ID_SYS_BATTERY_TEST, RectF(p.X + p.Width - ui.S(250), p.Y + ui.S(4), ui.S(120), ui.S(36)),
                      L"Мало заряда"))
            Preview(PREVIEW_BATTERY_LOW);
        if (ui.Button(ID_SYS_BATTERY_WAVE, RectF(p.X + p.Width - ui.S(122), p.Y + ui.S(4), ui.S(122), ui.S(36)),
                      L"Зарядка"))
            Preview(PREVIEW_BATTERY_CHARGE);
    });

    // ночь
    card(L"Ночной режим", 2.0f, [&](const RectF& b) {
        toggle(row(b, 0, L"Стекло темнее, звуки тише"), ID_SYS_NIGHT, cfg.night);
        RectF h = row(b, 1, L"Часы");
        auto stepper = [&](float rx, int idm, int idp, int& v) {
            if (ui.Button(idm, RectF(rx, h.Y + ui.S(5), ui.S(32), ui.S(34)), L"−")) {
                Change();
                v = (v + 23) % 24;
                Save();
            }
            ui.Text(std::to_wstring(v) + L":00", RectF(rx + ui.S(32), h.Y, ui.S(58), h.Height), c.text, 13, true,
                    ui::Center);
            if (ui.Button(idp, RectF(rx + ui.S(90), h.Y + ui.S(5), ui.S(32), ui.S(34)), L"+")) {
                Change();
                v = (v + 1) % 24;
                Save();
            }
        };
        float rx = h.X + h.Width - ui.S(122) * 2 - ui.S(30);
        ui.Text(L"с", RectF(rx - ui.S(20), h.Y, ui.S(20), h.Height), c.muted, 12);
        stepper(rx, ID_SYS_NIGHT_FROM_M, ID_SYS_NIGHT_FROM_P, cfg.nightFrom);
        ui.Text(L"до", RectF(rx + ui.S(126), h.Y, ui.S(26), h.Height), c.muted, 12);
        stepper(rx + ui.S(152), ID_SYS_NIGHT_TO_M, ID_SYS_NIGHT_TO_P, cfg.nightTo);
    });

    // стол и снимки
    card(L"Стол и снимки", 3.0f, [&](const RectF& b) {
        toggle(row(b, 0, L"Двойной клик по столу - кольцо"), ID_SYS_DESK_DOUBLE, cfg.deskDouble);
        toggle(row(b, 1, L"Alt + Ё - кольцо открытых окон"), ID_SYS_WIN_RING, cfg.winRing);
        toggle(row(b, 2, L"Карточка после снимка области"), ID_SYS_SNIP, cfg.snipCard);
    });

    float bottom = cols.Bottom();
    if (!sysStatus_.empty()) {
        ui.Text(sysStatus_, RectF(cols.X(), bottom, cols.Width(), ui.S(40)), c.accent, 12, false, ui::Left, true);
        bottom += ui.S(44);
    }
    pageContentH_ = bottom - cols.Top();
    ui.G().Restore(st);
}

}  // namespace studio
