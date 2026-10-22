// «Настройки / Помощники»: то, что работает рядом с кольцами, - палитра команд, история буфера, раскладки окон,
// росчерк, сторожа процессов, резервная копия, безопасный запуск.
#include <windows.h>

#include <algorithm>
#include <cmath>

#include "app/palette.h"
#include "app/toolsui.h"
#include "core/files.h"
#include "desk/appvol.h"
#include "desk/cliphist.h"
#include "core/text.h"
#include "desk/gamepad.h"
#include "tools/guard.h"
#include "app/rules.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

void Studio::AidsPage(const RectF& r) {
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Помощники", L"Что работает рядом с кольцами. Всё выключается.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));
    const UiColors& c = ui.C();
    auto toggle = [&](const RectF& rr, int id, bool& v) {
        bool val = v;
        if (ui.Toggle(id, RectF(rr.X + rr.Width - ui.S(42), rr.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };

    cols.Card(L"Командная палитра", 3.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Открывать по сочетанию");
        toggle(rr, ID_AID + 0, cfg.palette);
        rr = cols.Row(b, 1, L"Сочетание", 0.4f);
        FieldOrText(
            ID_AID + 1, RectF(rr.X + rr.Width * 0.42f, rr.Y + ui.S(4), rr.Width * 0.58f, ui.S(36)), view,
            cfg.paletteKey,
            [this](const std::wstring& s) {
                FieldEdit(ID_AID + 1);
                cfg.paletteKey = s;
            },
            L"alt+space");
        bool live = !palette::Status().empty();  // в снимке студии палитра не запускалась
        if (live) cols.Row(b, 2, L"Сейчас: " + palette::Status(), 0.95f);
        cols.Note(b, live ? 3.0f : 2.0f,
                  L"Строка поиска по всем пунктам колец, действиям утилит и сети, разделам студии и открытым окнам; "
                  L"Enter - выполнить. Буквы можно пропускать, раскладка не важна: «gc» найдёт «Проверить сеть».",
                  c.faint);
    });
    cols.Card(L"Росчерк", 2.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Махнуть с Alt - выбрать без ожидания");
        toggle(rr, ID_AID + 2, cfg.stroke);
        cols.Note(b, 1.0f,
                  L"Зажали Alt у края и сразу махнули мышью в сторону сектора: отпустили Alt над пунктом - он "
                  L"открывается, кольцо можно не ждать. Над группой или папкой кольцо остаётся, дальше - ломаной во "
                  L"второе кольцо. Медленный подход к кольцу работает как раньше.",
                  c.faint);
    });
    {
        std::wstring bad;
        std::vector<guard::ProcRule> rules = guard::ParseRules(cfg.procRules, &bad);
        float rows = 3.0f + rules.size() * 0.6f + (bad.empty() ? 0 : 0.6f);
        cols.Card(L"Правила процессов", rows, [&](const RectF& b) {
            RectF rr = cols.Row(b, 0, L"Правила", 0.2f);
            FieldOrText(
                ID_AID + 41, RectF(rr.X + rr.Width * 0.22f, rr.Y + ui.S(4), rr.Width * 0.78f, ui.S(36)), view,
                cfg.procRules,
                [this](const std::wstring& s) {
                    FieldEdit(ID_AID + 41);
                    cfg.procRules = s;
                },
                L"chrome.exe=below,eco | blender.exe=high");
            float y = b.Y + cols.RowH();
            for (const guard::ProcRule& r : rules) {
                ui.Text(guard::DescribeRule(r), RectF(b.X, y, b.Width, ui.S(24)), c.text, 11.5f);
                y += cols.RowH() * 0.6f;
            }
            if (!bad.empty()) {
                ui.Text(L"Не понял: " + bad, RectF(b.X, y, b.Width, ui.S(24)), c.warn, 11.5f);
                y += cols.RowH() * 0.6f;
            }
            cols.Note(b, (y - b.Y) / cols.RowH() + 0.1f,
                      L"Каждому новому процессу с таким именем - сразу свой приоритет, ядра или экономия (EcoQoS). Слова: "
                      L"idle, below, normal, above, high, eco, noeco, cores:0-3+6. Правила через «|», маски как "
                      L"*Editor*.exe. Игровой режим и баланс продолжают работать поверх.",
                      c.faint);
        });
    }
    {
        std::wstring bad;
        rules::Parse(cfg.rules, &bad);
        std::wstring active = rules::ActiveText();
        float rows = 2.6f + cfg.rules.size() * 0.8f + (active.empty() ? 0 : 0.6f);
        cols.Card(L"Если - то", rows, [&](const RectF& b) {
            float y = b.Y;
            for (size_t i = 0; i < cfg.rules.size() && i < 20; ++i) {
                bool ok = bad.find(cfg.rules[i]) == std::wstring::npos;
                ui.Text(cfg.rules[i], RectF(b.X, y, b.Width - ui.S(110), ui.S(30)), ok ? c.text : c.warn, 11.5f);
                if (ui.Button(ID_AID + 50 + (int)i, RectF(b.GetRight() - ui.S(100), y, ui.S(100), ui.S(30)), L"Убрать",
                              Kind::Ghost)) {
                    Change();
                    cfg.rules.erase(cfg.rules.begin() + i);
                    Save();
                    break;
                }
                y += cols.RowH() * 0.8f;
            }
            if (!active.empty()) {
                ui.Text(L"Сейчас держится: " + active, RectF(b.X, y, b.Width, ui.S(24)), c.accent, 11.5f);
                y += cols.RowH() * 0.6f;
            }
            RectF rr(b.X, y, b.Width, ui.S(44));
            FieldOrText(
                ID_AID + 49, RectF(rr.X, rr.Y + ui.S(4), rr.Width - ui.S(130), ui.S(36)), view, ruleDraft_,
                [this](const std::wstring& s) { ruleDraft_ = s; }, L"новое: app:blender.exe -> power:perf, volume:40");
            std::wstring draftBad;
            bool draftOk = !rules::Parse({ruleDraft_}, &draftBad).empty();
            if (ui.Button(ID_AID + 48, RectF(rr.GetRight() - ui.S(120), rr.Y + ui.S(4), ui.S(120), ui.S(36)),
                          L"Добавить", draftOk ? Kind::Primary : Kind::Ghost) &&
                draftOk) {
                Change();
                cfg.rules.push_back(Trim(ruleDraft_));
                ruleDraft_.clear();
                SetFocus(wnd);  // поле без фокуса покажет пустой черновик
                Save();
            }
            cols.Note(b, (y + ui.S(48) - b.Y) / cols.RowH(),
                      L"Условия: app:имя.exe, ac, battery, battery<30, time:23:00-07:00, weekend, idle>15 (минут "
                      L"простоя), net:имя сети, «!» перед условием - «не» (вместе - через &). "
                      L"Действия: power:eco|balanced|perf, volume:30, mute, theme:<тема>, run:<действие угла>. Правило "
                      L"кончилось - режим, громкость и тема возвращаются. Первое подходящее главнее.",
                      c.faint);
        });
    }
    cols.Card(L"Геймпад", 2.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, gamepad::Connected() ? L"Кольцо с геймпада (подключён)" : L"Кольцо с геймпада");
        toggle(rr, ID_AID + 40, cfg.gamepad);
        cols.Note(b, 1.0f,
                  L"Зажать View на полсекунды - целое кольцо в середине экрана. Левый стик - сектор (до упора - "
                  L"второе кольцо), A - открыть, B - назад или закрыть, LB - назад, RB - войти в папку. Xbox и "
                  L"совместимые (XInput).",
                  c.faint);
    });
    cols.Card(L"Буфер обмена", 5.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"История: 30 последних записей");
        toggle(rr, ID_AID + 3, cfg.clipHistory);
        rr = cols.Row(b, 1, L"Секреты не запоминать и стирать");
        toggle(rr, ID_AID + 4, cfg.clipSecrets);
        rr = cols.Row(b, 2, L"Стирать через " + std::to_wstring(cfg.clipSecretSec) + L" с", 0.5f);
        int sec = cfg.clipSecretSec;
        if (ui.Slider(ID_AID + 5, RectF(rr.GetRight() - ui.S(170), rr.Y + ui.S(8), ui.S(170), ui.S(28)), sec, 10, 300,
                      10)) {
            Change();
            cfg.clipSecretSec = sec;
            Save();
        }
        rr = cols.Row(b, 3, L"Сейчас: " + cliphist::Status(), 0.62f);
        if (ui.Button(ID_AID + 6, RectF(rr.GetRight() - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)), L"Забыть всё",
                      Kind::Secondary))
            cliphist::Clear();
        cols.Note(
            b, 4.0f,
            L"Кольцо «Буфер» - действие угла или пункт кольца action:clip, записи есть и в палитре: выбрали - "
            L"снова в буфере и вставлены. Хранится только в памяти; закреплённое (сектор «Закрепить») - на диске, "
            L"зашифровано, «Забыть всё» его не трогает. Токены (GitHub, AWS, Google, ключи API, "
            L"JWT), закрытые ключи, пароли строкой и номера карт не запоминаются и стираются из буфера, если "
            L"его не заменили. Что менеджеры паролей помечают «не для истории» - не трогаю.",
            c.faint);
    });
    cols.Card(L"Окна: раскладка и Alt + мышь", 3.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Alt + мышь: тащить и размер");
        toggle(rr, ID_AID + 7, cfg.winDrag);
        rr = cols.Row(b, 1, L"Кольцо «Раскладка» на угол", 0.6f);
        bool onCorner = false;
        for (int i = 0; i < 4; ++i) onCorner |= cfg.corners[i] == L"layout" || cfg.corners2[i] == L"layout";
        if (ui.Button(ID_AID + 8, RectF(rr.GetRight() - ui.S(170), rr.Y + ui.S(4), ui.S(170), ui.S(36)),
                      onCorner ? L"Уже на углу" : L"Выбрать угол", Kind::Secondary))
            Defer([this] { GoPage(PAGE_DESK); });
        cols.Note(b, 2.0f,
                  L"Тащить окно можно за любое место, развёрнутое встаёт обычным под курсором; размер тянется "
                  L"ближним краем или углом. Не в играх, окнах без рамки и там, где Alt + мышь - своя камера (Unreal, "
                  L"Blender, Maya, 3ds Max, Houdini, ZBrush, Krita, Photoshop, Unity, Godot). Кольцо «Раскладка» "
                  L"(угол layout или пункт action:layout, строки «Окно:» в палитре): половины, трети, четверти, "
                  L"середина и несколько последних окон монитора рядом, колонками, «главное и два», четвертями.",
                  c.faint);
    });
    auto field = [&](const RectF& b, float row, int id, std::wstring& v, const wchar_t* ph) {
        RectF fr(b.X, b.Y + row * cols.RowH(), b.Width, ui.S(36));
        FieldOrText(
            id, fr, view, v,
            [this, id, &v](const std::wstring& s) {
                FieldEdit(id);
                v = s;
            },
            ph);
    };
    RepaintIn(2000);  // сторожа живут своей жизнью: строки «Сейчас» обновляются
    cols.Card(L"Тихий баланс процессора", 3.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Придерживать фоновых обжор");
        toggle(rr, ID_AID + 9, cfg.cpuBalance);
        std::vector<std::wstring> low = toolsui::BalanceLowered();
        std::wstring now;
        for (const std::wstring& n : low) now += (now.empty() ? L"" : L", ") + n;
        rr = cols.Row(b, 1, low.empty() ? L"Сейчас никто не понижен" : L"Понижены: " + now, 0.62f);
        if (!low.empty() &&
            ui.Button(ID_AID + 10, RectF(rr.GetRight() - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Вернуть всем", Kind::Secondary))
            toolsui::BalanceRestoreAll();
        cols.Note(
            b, 2.0f,
            L"Как ProBalance: процесс не на переднем плане полторы минуты ест больше четверти процессора, пока вы "
            L"работаете, - его приоритет «ниже обычного». Вернётся сам, когда успокоится, когда вы отойдёте или "
            L"переключитесь на него. Системное, своё, игру, OBS и то, что из boost_keep_fast, не трогаю; в игре "
            L"баланс спит (там свой Boost). Каждое действие - в журнале автоматики.",
            c.faint);
    });
    cols.Card(L"Не спать, пока работает", 3.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Сборка, копирование, загрузка Steam");
        toggle(rr, ID_AID + 11, cfg.noSleep);
        field(b, 1, ID_AID + 12, cfg.noSleepApps, L"Имена exe через |");
        std::wstring a = toolsui::AwakeReason();
        cols.Row(b, 2, a.empty() ? L"Сейчас: ничего не идёт, сон как обычно" : L"Сейчас не сплю: идёт " + a, 0.95f);
        cols.Note(b, 3.0f,
                  L"Пока работает что-то из списка, компьютер не уходит в сон (экран гаснет как обычно). Причина - в "
                  L"подсказке значка в трее.",
                  c.faint);
    });
    cols.Card(L"Сторож долгих команд", 4.2f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Карточка «готово за N мин»");
        toggle(rr, ID_AID + 13, cfg.longWatch);
        rr = cols.Row(b, 1, L"Долгая - от " + Plural(cfg.longMin, L"минуты", L"минут", L"минут"), 0.5f);
        int m = cfg.longMin;
        if (ui.Slider(ID_AID + 14, RectF(rr.GetRight() - ui.S(170), rr.Y + ui.S(8), ui.S(170), ui.S(28)), m, 1, 30)) {
            Change();
            cfg.longMin = m;
            Save();
        }
        field(b, 2, ID_AID + 15, cfg.longApps, L"Имена exe через |");
        cols.Note(b, 3.0f,
                  L"Сборка Unreal, шейдеры, упаковка, ninja, cmake, долгий git clone закончились - карточка «готово» "
                  L"или «упало» с кодом выхода, если вы в это время в другом окне (в терминале, откуда запускали, - "
                  L"молчу). Вложенные команды (ninja под cmake) не дублируются; сборки и шейдеры Unreal в режиме "
                  L"разработки сообщает он сам.",
                  c.faint);
    });
    cols.Card(L"Громкость по программам", 3.6f, [&](const RectF& b) {
        RectF rr = cols.Row(b, 0, L"Помнить уровень каждой программы");
        toggle(rr, ID_AID + 51, cfg.appVolume);
        std::wstring list;
        int shown = 0;
        for (const auto& [exe, v] : appvol::Saved()) {
            if (shown++ == 6) break;
            list += (list.empty() ? L"" : L", ") + exe + L" " + std::to_wstring((int)std::lround(v * 100)) + L"%";
        }
        cols.Row(b, 1, list.empty() ? L"Пока ничего: поменяйте громкость программы в микшере" : L"Помню: " + list,
                 0.95f);
        cols.Note(b, 2.0f,
                  L"Поставили в микшере Windows Discord тише, а игру громче - уровень запоминается по имени программы "
                  L"и возвращается при её следующем запуске. Windows помнит это не для всех: браузеры и многие игры "
                  L"начинают со 100%. Проверка раз в 4 с, без прав.",
                  c.faint);
    });
    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
