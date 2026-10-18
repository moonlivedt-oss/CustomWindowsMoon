// «Кольца / Программы»: кольца команд под программу (первым сектором, когда она впереди) с заготовками для
// VS Code, браузеров, Blender, Unreal, Проводника и Терминала; команда - сочетание (keys:), макрос (macro:),
// программа или папка. Сочетание можно записать кнопкой «Записать», как у горячих углов.
#include <windows.h>

#include <algorithm>

#include "app/worksets.h"
#include "core/files.h"
#include "core/macro.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

struct Cmd {
    const wchar_t* name;
    const wchar_t* path;
};
struct Preset {
    const wchar_t* name;
    const wchar_t* exes;
    std::vector<Cmd> cmds;
};

const std::vector<Preset>& Presets() {
    static const std::vector<Preset> list = {
        {L"VS Code",
         L"Code.exe|Code - Insiders.exe|Cursor.exe|VSCodium.exe",
         {{L"Палитра команд", L"keys:ctrl+shift+p"},
          {L"Открыть файл", L"keys:ctrl+p"},
          {L"Терминал", L"keys:ctrl+`"},
          {L"Поиск по проекту", L"keys:ctrl+shift+f"},
          {L"Боковая панель", L"keys:ctrl+b"},
          {L"Форматировать", L"keys:shift+alt+f"},
          {L"Разделить", L"keys:ctrl+\\"},
          {L"Git", L"keys:ctrl+shift+g"}}},
        {L"Браузер",
         L"chrome.exe|msedge.exe|brave.exe|firefox.exe|browser.exe|opera.exe",
         {{L"Новая вкладка", L"keys:ctrl+t"},
          {L"Вернуть вкладку", L"keys:ctrl+shift+t"},
          {L"Адрес", L"keys:ctrl+l"},
          {L"Загрузки", L"keys:ctrl+j"},
          {L"История", L"keys:ctrl+h"},
          {L"Инкогнито", L"keys:ctrl+shift+n"},
          {L"Без кэша", L"keys:ctrl+shift+r"},
          {L"Инструменты", L"keys:f12"}}},
        {L"Blender",
         L"blender.exe",
         {{L"Поиск", L"keys:f3"},
          {L"Рендер", L"keys:f12"},
          {L"Правка", L"keys:tab"},
          {L"Камера", L"keys:vk60"},
          {L"Сохранить", L"keys:ctrl+s"},
          {L"Отменить", L"keys:ctrl+z"}}},
        {L"Unreal",
         L"UnrealEditor.exe|UE4Editor.exe",
         {{L"Играть", L"keys:alt+p"},
          {L"Сохранить всё", L"keys:ctrl+shift+s"},
          {L"Live Coding", L"keys:ctrl+alt+f11"},
          {L"Найти ассет", L"keys:ctrl+p"},
          {L"Консоль", L"keys:`"},
          {L"Выход из игры", L"keys:shift+f1"}}},
        {L"Проводник",
         L"explorer.exe",
         {{L"Новая папка", L"keys:ctrl+shift+n"},
          {L"Адрес", L"keys:alt+d"},
          {L"Свойства", L"keys:alt+enter"},
          {L"Новая вкладка", L"keys:ctrl+t"},
          {L"Терминал здесь", L"macro:alt+d; wait 150; text wt -d .; enter"},
          {L"Скопировать путь", L"macro:alt+d; wait 100; ctrl+c; esc"}}},
        {L"Терминал",
         L"WindowsTerminal.exe|wt.exe",
         {{L"Новая вкладка", L"keys:ctrl+shift+t"},
          {L"Разделить", L"keys:alt+shift+="},
          {L"Поиск", L"keys:ctrl+shift+f"},
          {L"Закрыть панель", L"keys:ctrl+shift+w"}}},
    };
    return list;
}

constexpr int kMaxCmds = 20;
int AppId(int a, int f) { return ID_APP + a * 100 + f; }  // f: 0 имя, 1 exe, 2 + команда, 3 удалить, 10+ - команды

}  // namespace

void Studio::AppsPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Кольца программ",
           L"Открыли кольцо над программой - первым сектором идут её команды: сочетания, макросы, файлы.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));

    // заготовки: те, что уже есть, не повторяются
    std::vector<const Preset*> left;
    for (const Preset& p : Presets()) {
        bool have = false;
        for (const AppRing& a : cfg.apps)
            if (a.exes == p.exes || a.ring.name == p.name) have = true;
        if (!have) left.push_back(&p);
    }
    cols.Wide(L"Добавить кольцо", left.empty() ? 1.6f : 2.3f, [&](const RectF& b) {
        float x = b.X, bw = ui.S(128);
        for (size_t i = 0; i < left.size(); ++i) {
            const Preset* p = left[i];
            if (x + bw > b.GetRight()) break;
            if (ui.Button(ID_APP_PRESET + (int)i, RectF(x, b.Y, bw, ui.S(36)), p->name, Kind::Secondary)) {
                Change();
                AppRing a;
                a.exes = p->exes;
                a.ring.name = p->name;
                for (const Cmd& cm : p->cmds) {
                    Item ch;
                    ch.name = cm.name;
                    ch.path = cm.path;
                    a.ring.children.push_back(ch);
                }
                Resolve(a.ring);
                cfg.apps.push_back(std::move(a));
                Log(L"Кольцо программы: " + std::wstring(p->name));
                Save();
            }
            x += bw + ui.S(8);
        }
        float y = b.Y + (left.empty() ? 0 : ui.S(46));
        RectF fr(b.X, y, b.Width - ui.S(180), ui.S(36));
        FieldOrText(
            ID_APP_NEW, fr, view, appNewExe_, [this](const std::wstring& s) { appNewExe_ = s; },
            L"Своя программа: имя exe, например obs64.exe или Photoshop.exe");
        if (ui.Button(ID_APP_NEW_BTN, RectF(fr.GetRight() + ui.S(10), y, ui.S(170), ui.S(36)), L"Добавить",
                      Kind::Primary) &&
            !Trim(appNewExe_).empty()) {
            Change();
            AppRing a;
            a.exes = Trim(appNewExe_);
            std::wstring n = a.exes.substr(0, a.exes.find(L'|'));
            size_t dot = n.find_last_of(L'.');
            a.ring.name = dot != std::wstring::npos && dot > 0 ? n.substr(0, dot) : n;
            Item ch;
            ch.name = L"Сохранить";
            ch.path = L"keys:ctrl+s";
            a.ring.children.push_back(ch);
            Resolve(a.ring);
            cfg.apps.push_back(std::move(a));
            appNewExe_.clear();
            Save();
        }
    });

    for (size_t ai = 0; ai < cfg.apps.size() && ai < 40; ++ai) {
        AppRing& a = cfg.apps[ai];
        int A = (int)ai;
        int n = (int)std::min<size_t>(a.ring.children.size(), kMaxCmds);
        float rows = 1.0f + n * 0.88f + 1.0f;
        cols.Wide(a.ring.name.c_str(), rows, [&](const RectF& b) {
            float rh = cols.RowH() * 0.88f;
            RectF nr(b.X, b.Y, b.Width * 0.32f, ui.S(36));
            FieldOrText(
                AppId(A, 0), nr, view, a.ring.name,
                [this, A](const std::wstring& s) {
                    FieldEdit(AppId(A, 0));
                    if (A < (int)cfg.apps.size()) cfg.apps[A].ring.name = s;
                },
                L"Подпись сектора");
            RectF er(nr.GetRight() + ui.S(10), b.Y, b.Width - nr.Width - ui.S(10), ui.S(36));
            FieldOrText(
                AppId(A, 1), er, view, a.exes,
                [this, A](const std::wstring& s) {
                    FieldEdit(AppId(A, 1));
                    if (A < (int)cfg.apps.size()) cfg.apps[A].exes = s;
                },
                L"Имена exe через |");
            for (int j = 0; j < n; ++j) {
                Item& ch = a.ring.children[j];
                float y = b.Y + cols.RowH() + j * rh;
                bool capturing = captureApp_ == A && captureCmd_ == j;
                RectF cn(b.X, y, b.Width * 0.28f, ui.S(34));
                FieldOrText(
                    AppId(A, 10 + j * 4), cn, view, ch.name,
                    [this, A, j](const std::wstring& s) {
                        FieldEdit(AppId(A, 10 + j * 4));
                        if (A < (int)cfg.apps.size() && j < (int)cfg.apps[A].ring.children.size())
                            cfg.apps[A].ring.children[j].name = s;
                    },
                    L"Подпись");
                float btnW = ui.S(112), delW = ui.S(40);
                RectF cp(cn.GetRight() + ui.S(8), y, b.Width - cn.Width - btnW - delW - ui.S(24), ui.S(34));
                FieldOrText(
                    AppId(A, 11 + j * 4), cp, view, ch.path,
                    [this, A, j](const std::wstring& s) {
                        FieldEdit(AppId(A, 11 + j * 4));
                        if (A < (int)cfg.apps.size() && j < (int)cfg.apps[A].ring.children.size()) {
                            cfg.apps[A].ring.children[j].path = s;
                            Resolve(cfg.apps[A].ring.children[j]);
                        }
                    },
                    L"keys:ctrl+shift+p, macro:..., путь или ссылка");
                if (macro::Is(ch.path)) ui.Tip(AppId(A, 11 + j * 4), macro::Describe(ch.path));
                if (ui.Button(AppId(A, 12 + j * 4), RectF(cp.GetRight() + ui.S(8), y, btnW, ui.S(34)),
                              capturing ? L"Нажмите..." : L"Записать", capturing ? Kind::Primary : Kind::Secondary)) {
                    captureApp_ = capturing ? -1 : A;
                    captureCmd_ = capturing ? -1 : j;
                    captureCorner_ = -1;
                    SetFocus(wnd);  // сочетание ловит окно студии, а не поле ввода
                }
                if (ui.Button(AppId(A, 13 + j * 4), RectF(b.GetRight() - delW, y, delW, ui.S(34)), L"×", Kind::Ghost)) {
                    Defer([this, A, j] {
                        if (A >= (int)cfg.apps.size() || j >= (int)cfg.apps[A].ring.children.size()) return;
                        Change();
                        auto& v = cfg.apps[A].ring.children;
                        v.erase(v.begin() + j);
                        Save();
                    });
                }
            }
            float y = b.Y + cols.RowH() + n * rh + ui.S(4);
            if (n < kMaxCmds && ui.Button(AppId(A, 2), RectF(b.X, y, ui.S(150), ui.S(36)), L"+ Команда")) {
                Change();
                Item ch;
                ch.name = L"Команда";
                ch.path = L"keys:";
                a.ring.children.push_back(ch);
                Save();
            }
            if (ui.Button(AppId(A, 3), RectF(b.GetRight() - ui.S(160), y, ui.S(160), ui.S(36)), L"Удалить кольцо",
                          Kind::Danger)) {
                Defer([this, A] {
                    if (A >= (int)cfg.apps.size()) return;
                    Change();
                    Log(L"Удалено кольцо программы: " + cfg.apps[A].ring.name);
                    cfg.apps.erase(cfg.apps.begin() + A);
                    Save();
                });
            }
        });
    }
    SetsCards(cols, view);
    if (cfg.apps.empty())
        cols.Wide(L"Как это работает", 2.0f, [&](const RectF& b) {
            cols.Note(b, 0,
                      L"Добавьте кольцо заготовкой или для своей программы. Когда впереди окно этой программы, Alt + "
                      L"край экрана откроет кольцо, где первым сектором - её команды. Команда - сочетание "
                      L"(keys:ctrl+shift+p), макрос (macro:ctrl+l; wait 200; text адрес; enter), программа или папка.",
                      c.muted);
        });
    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

// Рабочие наборы: под кольцами программ, по карточке на набор.
void Studio::SetsCards(CardColumns& cols, const RectF& view) {
    const UiColors& c = ui.C();
    cols.Wide(L"Рабочие наборы", 1.9f, [&](const RectF& b) {
        if (ui.Button(ID_SET_NEW, RectF(b.GetRight() - ui.S(170), b.Y, ui.S(170), ui.S(36)), L"Новый набор",
                      Kind::Primary)) {
            Change();
            WorkSet w;
            w.ring.name = L"Проект " + std::to_wstring(cfg.sets.size() + 1);
            w.layout = L"l:2";
            cfg.sets.push_back(w);
            Save();
        }
        ui.Text(L"Одним щелчком - VS Code с папкой, терминал с окружением, ссылки и программы, окна - по раскладке.",
                RectF(b.X, b.Y, b.Width - ui.S(190), ui.S(36)), c.muted, 12);
        cols.Note(b, 0.9f,
                  L"Открыть - в кольце «Наборы» (действие угла sets или пункт action:sets) и в палитре («Набор: ...»). "
                  L"«Закрыть набор» вежливо закрывает только те окна, что открылись при его запуске.",
                  c.faint);
    });
    static const std::vector<std::wstring> kLays = {L"Как откроются", L"Два", L"Три", L"Главное и два", L"Четыре"};
    static const wchar_t* kLayKeys[] = {L"", L"l:2", L"l:3", L"l:main2", L"l:4"};
    for (size_t si = 0; si < cfg.sets.size() && si < 20; ++si) {
        WorkSet& w = cfg.sets[si];
        int S = (int)si;
        auto id = [S](int f) { return ID_SET + S * 100 + f; };
        int n = (int)std::min<size_t>(w.ring.children.size(), 16);
        cols.Wide(w.ring.name.c_str(), 2.0f + n * 0.88f + 1.0f, [&](const RectF& b) {
            RectF nr(b.X, b.Y, b.Width * 0.4f, ui.S(36));
            FieldOrText(
                id(0), nr, view, w.ring.name,
                [this, S, id](const std::wstring& s) {
                    FieldEdit(id(0));
                    if (S < (int)cfg.sets.size()) cfg.sets[S].ring.name = s;
                },
                L"Имя набора");
            int sel = 0;
            for (int k = 0; k < 5; ++k)
                if (w.layout == kLayKeys[k]) sel = k;
            if (ui.Segmented(id(1), RectF(nr.GetRight() + ui.S(12), b.Y, b.Width - nr.Width - ui.S(12), ui.S(36)),
                             kLays, sel)) {
                Change();
                w.layout = kLayKeys[sel];
                Save();
            }
            float rh = cols.RowH() * 0.88f;
            for (int j = 0; j < n; ++j) {
                Item& ch = w.ring.children[j];
                float y = b.Y + cols.RowH() + j * rh;
                std::wstring kind = worksets::KindOf(ch.path);
                if (!kind.empty() && kind[0] != L'V') kind[0] = towupper(kind[0]);
                ui.Text(kind, RectF(b.X, y, ui.S(170), ui.S(34)), c.muted, 12);
                RectF pr(b.X + ui.S(176), y, b.Width - ui.S(176) - ui.S(48), ui.S(34));
                FieldOrText(
                    id(10 + j * 2), pr, view, ch.path,
                    [this, S, j, id](const std::wstring& s) {
                        FieldEdit(id(10 + j * 2));
                        if (S < (int)cfg.sets.size() && j < (int)cfg.sets[S].ring.children.size())
                            cfg.sets[S].ring.children[j].path = s;
                    },
                    L"code:папка, term:папка, https://..., программа или файл");
                if (ui.Button(id(11 + j * 2), RectF(b.GetRight() - ui.S(40), y, ui.S(40), ui.S(34)), L"×", Kind::Ghost))
                    Defer([this, S, j] {
                        if (S >= (int)cfg.sets.size() || j >= (int)cfg.sets[S].ring.children.size()) return;
                        Change();
                        auto& v = cfg.sets[S].ring.children;
                        v.erase(v.begin() + j);
                        Save();
                    });
            }
            float y = b.Y + cols.RowH() + n * rh + ui.S(4);
            auto add = [&](int f, const wchar_t* label, int kind, float x, float bw) {
                if (n >= 16 || !ui.Button(id(f), RectF(x, y, bw, ui.S(36)), label)) return;
                Defer([this, S, kind] {
                    if (S >= (int)cfg.sets.size()) return;
                    std::wstring path;
                    if (kind == 0 || kind == 1) {
                        std::wstring dir = PickPath(true);
                        if (dir.empty()) return;
                        path = (kind == 0 ? L"code:" : L"term:") + dir;
                    } else if (kind == 2) {
                        path = L"https://";
                    } else {
                        path = PickPath(false);
                        if (path.empty()) return;
                    }
                    Change();
                    Item it;
                    it.path = path;
                    std::wstring leaf = path.substr(path.find_last_of(L"\\/:") + 1);
                    it.name = leaf.empty() ? L"Ссылка" : leaf;
                    cfg.sets[S].ring.children.push_back(it);
                    Save();
                });
            };
            float bw = ui.S(118), gx = ui.S(8);
            add(2, L"+ VS Code", 0, b.X, bw);
            add(3, L"+ Терминал", 1, b.X + (bw + gx), bw);
            add(4, L"+ Ссылка", 2, b.X + 2 * (bw + gx), bw);
            add(5, L"+ Программа", 3, b.X + 3 * (bw + gx), bw);
            int open = worksets::OpenedCount(S);
            if (ui.Button(id(6), RectF(b.GetRight() - ui.S(290), y, ui.S(140), ui.S(36)),
                          open ? L"Закрыть" : L"Открыть", open ? Kind::Secondary : Kind::Primary))
                Defer([S, open] { open ? worksets::Close(S) : worksets::Open(S); });
            if (ui.Button(id(7), RectF(b.GetRight() - ui.S(140), y, ui.S(140), ui.S(36)), L"Удалить", Kind::Danger))
                Defer([this, S] {
                    if (S >= (int)cfg.sets.size()) return;
                    Change();
                    Log(L"Удалён набор: " + cfg.sets[S].ring.name);
                    cfg.sets.erase(cfg.sets.begin() + S);
                    Save();
                });
        });
    }
}

// Сочетание для команды кольца программы (кнопка «Записать»): как у углов, модификаторы копятся.
bool Studio::CaptureAppKey(WPARAM vk) {
    if (captureApp_ < 0) return false;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return true;
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000,
         alt = GetKeyState(VK_MENU) & 0x8000, win = (GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000;
    int a = captureApp_, j = captureCmd_;
    captureApp_ = captureCmd_ = -1;
    InvalidateRect(wnd, nullptr, FALSE);
    if (vk == VK_ESCAPE && !ctrl && !shift && !alt && !win) return true;
    if (a >= (int)cfg.apps.size() || j < 0 || j >= (int)cfg.apps[a].ring.children.size()) return true;
    std::wstring v = L"keys:";
    if (ctrl) v += L"ctrl+";
    if (shift) v += L"shift+";
    if (alt) v += L"alt+";
    if (win) v += L"win+";
    v += macro::KeyName((UINT)vk);
    Change();
    cfg.apps[a].ring.children[j].path = v;
    Resolve(cfg.apps[a].ring.children[j]);
    Log(cfg.apps[a].ring.name + L": " + cfg.apps[a].ring.children[j].name + L" = " + macro::Describe(v));
    Save();
    return true;
}

}  // namespace studio
