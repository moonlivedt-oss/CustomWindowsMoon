// Раздел «Красота»: скорость анимаций, рамки и заголовки окон, игровой режим, экран блокировки.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include "core/themes.h"
#include "desk/boost.h"
#include "desk/lockscreen.h"
#include "desk/whreg.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

void CopyText(HWND owner, const std::wstring& s) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

std::wstring AvatarFile() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\avatar.png", ad, MAX_PATH);
    return ad;
}

}  // namespace

void Studio::StylePage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(in, L"Красота", L"Движение, окна и экран блокировки. В играх всё это само выключается.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    float x = view.X + ui.S(4), w = std::min(view.Width - ui.S(8), ui.S(900)), y0 = view.Y - off;
    float gap = ui.S(16);
    bool two = w > ui.S(820);
    float cw = two ? (w - gap) / 2 : w;
    float colY[2] = {y0, y0};
    float rowH = ui.S(50);

    auto card = [&](const wchar_t* title, float rows, auto&& body) {
        int col = two && colY[1] < colY[0] ? 1 : 0;
        float h = ui.S(52) + rows * rowH;
        RectF cr(x + col * (cw + gap), colY[col], cw, h);
        Card(cr);
        ui.Text(title, RectF(cr.X + ui.S(18), cr.Y + ui.S(14), cr.Width, ui.S(24)), c.text, 14, true);
        body(RectF(cr.X + ui.S(18), cr.Y + ui.S(48), cr.Width - ui.S(36), rows * rowH));
        colY[col] += h + gap;
    };
    auto row = [&](const RectF& b, float n, const wchar_t* label) {
        RectF rr(b.X, b.Y + n * rowH, b.Width, ui.S(44));
        ui.Text(label, RectF(rr.X, rr.Y, rr.Width * 0.62f, rr.Height), c.text, 12.5f);
        return rr;
    };
    auto toggle = [&](const RectF& rr, int id, bool& v) {
        bool val = v;
        if (ui.Toggle(id, RectF(rr.X + rr.Width - ui.S(42), rr.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };
    auto note = [&](const RectF& b, float n, float lines, const std::wstring& s, Color col) {
        ui.Text(s, RectF(b.X, b.Y + n * rowH, b.Width, ui.S(20) * lines), col, 11.5f, false, ui::Left, true);
    };

    // скорость всех анимаций программы
    card(L"Движение", 1.9f, [&](const RectF& b) {
        int m = cfg.motion;
        if (ui.Segmented(ID_STYLE_MOTION, RectF(b.X, b.Y + ui.S(3), b.Width, ui.S(38)),
                         {L"Спокойно", L"Обычно", L"Живо"}, m) &&
            m != cfg.motion) {
            Change();
            cfg.motion = m;
            Save();
        }
        note(b, 1, 2, L"Кольца, углы, панель «Сейчас играет», карточки и рамки окон: медленнее и мягче или быстрее.",
             c.muted);
    });

    // окна
    card(L"Окна", 3.3f, [&](const RectF& b) {
        toggle(row(b, 0, L"Рамка активного окна загорается"), ID_STYLE_FRAME, cfg.windowFrame);
        toggle(row(b, 1, L"Заголовки окон в цветах темы"), ID_STYLE_CAPTIONS, cfg.windowCaptions);
        note(b, 2, 3,
             L"При переключении рамка вспыхивает светлым акцентом и оседает в цвет темы, у прежнего окна гаснет. "
             L"Окна со своим заголовком (браузер, VS Code) меняют только рамку.",
             c.muted);
    });

    // игровой режим
    card(L"Игровой режим", 6.6f, [&](const RectF& b) {
        toggle(row(b, 0, L"Включать сам, когда запущена игра"), ID_GAME_AUTO, cfg.gameAuto);
        toggle(row(b, 1, L"Выключать анимации Windows"), ID_GAME_ANIM, cfg.gameWinAnim);
        toggle(row(b, 2, L"Выключать прозрачность Windows"), ID_GAME_TRANSP, cfg.gameTransparency);
        toggle(row(b, 3, L"Пауза Wallpaper Engine"), ID_GAME_WALL, cfg.gameWallpaper);
        RectF fr(b.X, b.Y + 4 * rowH + ui.S(4), b.Width, ui.S(36));
        FieldOrText(
            ID_GAME_LIST, fr, view, cfg.games,
            [this](const std::wstring& s) {
                FieldEdit(ID_GAME_LIST);
                cfg.games = s;
            },
            L"Маски игр через |, например *\\steamapps\\common\\*|NFS14.exe");
        std::wstring now = GameStatus();
        note(b, 5, 3,
             (now.empty() ? L"" : L"Сейчас: " + now + L". ") +
                 L"Игрой считается программа из этих папок или всё, что идёт в эксклюзивном полноэкранном режиме. "
                 L"Без анимаций, стекла, свечений и рамок; включить вручную - в меню значка в трее.",
             c.muted);
    });

    // экран блокировки
    card(L"Экран блокировки", 7.6f, [&](const RectF& b) {
        bool busy = lockJob_ && !lockJob_->done;
        RectF rr = row(b, 0, busy ? L"Собираю картинку..." : L"Картинка в цветах темы");
        if (busy) {
            ui.Invalidate();
        } else if (ui.Button(ID_LOCK_APPLY, RectF(rr.X + rr.Width - ui.S(150), rr.Y + ui.S(4), ui.S(150), ui.S(36)),
                             L"Применить", Kind::Primary)) {
            lockJob_ = lockscreen::ApplyAsync(cfg, wnd);
        }
        RectF sr = row(b, 1, cfg.lockSource.empty() ? L"Источник: обои" : L"Источник: своя картинка");
        if (ui.Button(ID_LOCK_PICK, RectF(sr.X + sr.Width - ui.S(150), sr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Выбрать...")) {
            Defer([this] {
                std::wstring p = PickPath(false);
                if (p.empty()) return;
                Change();
                cfg.lockSource = p;
                Save();
            });
        }
        if (!cfg.lockSource.empty() &&
            ui.Button(ID_LOCK_WALL, RectF(sr.X + sr.Width - ui.S(250), sr.Y + ui.S(4), ui.S(92), ui.S(36)), L"Обои",
                      Kind::Ghost)) {
            Change();
            cfg.lockSource.clear();
            Save();
        }
        toggle(row(b, 2, L"Плавный переход после входа"), ID_LOCK_REVEAL, cfg.lockReveal);
        RectF ar = row(b, 3, L"Как открывается стол");
        int anim = cfg.lockPortal ? 0 : 1;
        if (ui.Segmented(ID_LOCK_ANIM, RectF(ar.X + ar.Width - ui.S(250), ar.Y + ui.S(3), ui.S(250), ui.S(38)),
                         {L"Портал", L"Растворение"}, anim) &&
            (anim == 0) != cfg.lockPortal) {
            Change();
            cfg.lockPortal = anim == 0;
            Save();
        }
        RectF tr = row(b, 4, L"Узор стекла");
        int pat = cfg.lockPattern == 0 ? 2 : cfg.lockPattern - 1;
        if (ui.Segmented(ID_LOCK_PATTERN, RectF(tr.X + tr.Width - ui.S(300), tr.Y + ui.S(3), ui.S(300), ui.S(38)),
                         {L"Соты", L"Осколки", L"Без узора"}, pat)) {
            int v = pat == 2 ? 0 : pat + 1;
            if (v != cfg.lockPattern) {
                Change();
                cfg.lockPattern = v;
                Save();
            }
        }
        RectF pr = row(b, 5, L"Посмотреть переход");
        if (ui.Button(ID_LOCK_DEMO, RectF(pr.X + pr.Width - ui.S(150), pr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Показать"))
            Preview(PREVIEW_LOCK_REVEAL);
        std::wstring s =
            L"Тени уходят в графит темы, края темнеют, сверху вуаль под часы, у нижнего края световой шов. "
            L"Портал: после входа стекло покрывается узором и рассыпается от курсора, открывая стол; "
            L"блокировка из угла схлопывает его обратно.";
        Color col = c.muted;
        if (lockJob_ && lockJob_->done) {
            AcquireSRWLockShared(&lockJob_->lock);
            s = lockJob_->message;
            col = lockJob_->ok ? c.accent : c.warn;
            ReleaseSRWLockShared(&lockJob_->lock);
        }
        note(b, 6, 3, s, col);
    });

    // сотовое стекло: общий стиль для углов, кольца, заряда и игрового режима; портал в других местах
    card(L"Сотовое стекло", 5.6f, [&](const RectF& b) {
        toggle(row(b, 0, L"Соты в углах, кольце, заряде"), ID_HEX_GLASS, cfg.hexGlass);
        toggle(row(b, 1, L"Портал при входе после включения"), ID_HEX_BOOT, cfg.bootPortal);
        toggle(row(b, 2, L"Портал после сна и выключения экрана"), ID_HEX_WAKE, cfg.wakePortal);
        RectF wr = row(b, 3, L"Волна игрового режима");
        if (ui.Button(ID_HEX_WAVE, RectF(wr.X + wr.Width - ui.S(150), wr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Показать"))
            Preview(PREVIEW_GAME_WAVE);
        note(b, 4, 3,
             L"Угол загорается веером сот, в кольце под курсором приподнимается сота, зарядка пробегает волной сот. "
             L"Выключение экрана из угла схлопывает портал в угол.",
             c.muted);
    });

    // Boost: память перед игрой
    card(L"Память перед игрой", 6.7f, [&](const RectF& b) {
        std::wstring dir = boost::Dir(cfg.boostDir);
        boost::Settings bs = boost::Load(dir);
        toggle(row(b, 0, L"Предлагать освободить память"), ID_BOOST_OFFER, cfg.boostOffer);
        toggle(row(b, 1, L"После игры вернуть закрытое"), ID_BOOST_RESTORE, cfg.boostRestore);
        toggle(row(b, 2, L"После игры почистить ещё раз"), ID_BOOST_AFTER, cfg.boostAfter);
        if (boostTask_ < 0) boostTask_ = boost::PurgeTaskExists() ? 1 : 0;
        RectF tr = row(b, 3, boostTask_ == 1 ? L"Очистка кэша: без запросов" : L"Очистка кэша без запросов");
        if (boostTask_ == 2) {
            ui.Text(L"жду разрешения...", RectF(tr.X + tr.Width - ui.S(150), tr.Y, ui.S(150), tr.Height), c.muted, 12,
                    false, ui::Right);
            ui.Invalidate();
        } else if (ui.Button(ID_BOOST_TASK, RectF(tr.X + tr.Width - ui.S(150), tr.Y + ui.S(4), ui.S(150), ui.S(36)),
                             boostTask_ == 1 ? L"Пересоздать" : L"Настроить", boostTask_ == 1 ? Kind::Ghost : Kind::Primary)) {
            boostTask_ = 2;
            struct Ctx {
                Studio* st;
                HWND wnd;
            };
            auto* ctx = new Ctx{this, wnd};
            HANDLE th = CreateThread(
                nullptr, 0,
                [](void* p) -> DWORD {
                    std::unique_ptr<Ctx> c((Ctx*)p);
                    bool ok = boost::SetupPurgeTask();
                    c->st->boostTask_ = ok ? 1 : 0;
                    InvalidateRect(c->wnd, nullptr, FALSE);
                    return 0;
                },
                ctx, 0, nullptr);
            if (th)
                CloseHandle(th);
            else {
                delete ctx;
                boostTask_ = 0;
            }
        }
        RectF nr = row(b, 4, L"");
        if (ui.Button(ID_BOOST_NOW, RectF(nr.X + nr.Width - ui.S(150), nr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Освободить..."))
            Preview(PREVIEW_BOOST);
        if (ui.Button(ID_BOOST_LIST, RectF(nr.X, nr.Y + ui.S(4), ui.S(150), ui.S(36)),
                      L"Что закрывать", Kind::Ghost) &&
            !dir.empty())
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + dir + L"\\boost.config.json\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
        int used = 0;
        double avail = boost::AvailMb(&used);
        wchar_t mem[64];
        swprintf(mem, 64, L"Сейчас занято %d%%, свободно %.1f ГБ. ", used, avail / 1024);
        std::wstring s = dir.empty()
                             ? L"Папка Boost не найдена: положи её рядом с RadialMenu или укажи boost_dir в radial.cfg."
                             : std::wstring(mem) + L"Списки и что чистить - в boost.config.json, общем со скриптами Boost" +
                                   (bs.purge ? L"." : L"; очистка кэша там выключена (purgeStandby).");
        note(b, 5, 3, s, dir.empty() ? c.warn : c.muted);
    });

    // экран ввода пароля: аватар, чёткий фон
    card(L"Ввод пароля", 5.0f, [&](const RectF& b) {
        std::wstring avatar = AvatarFile();
        // превью аватара кругом слева от кнопок
        float ps = ui.S(84);
        RectF pr(b.X, b.Y + ui.S(4), ps, ps);
        if (Bitmap* img = GetFileAttributesW(avatar.c_str()) != INVALID_FILE_ATTRIBUTES ? Img(avatar, 168) : nullptr) {
            GraphicsPath clip;
            clip.AddEllipse(pr);
            GraphicsState s2 = ui.G().Save();
            ui.G().SetClip(&clip, CombineModeIntersect);
            ui.G().DrawImage(img, pr);
            ui.G().Restore(s2);
        } else {
            ui.Fill(pr, c.field, ps / 2);
        }
        ui.Stroke(pr, c.border, ps / 2);
        float bx = b.X + ps + ui.S(16), bw = b.Width - ps - ui.S(16);
        ui.Text(L"Аватар в стиле колец", RectF(bx, b.Y, bw, ui.S(22)), c.text, 12.5f);
        ui.Text(cfg.avatarSource.empty() ? L"в середине полумесяц" : L"в середине своя картинка",
                RectF(bx, b.Y + ui.S(22), bw, ui.S(20)), c.muted, 11.5f);
        if (ui.Button(ID_LOGON_AVATAR, RectF(bx, b.Y + ui.S(48), ui.S(150), ui.S(36)), L"Сделать и поставить",
                      Kind::Primary)) {
            std::wstring out, msg;
            if (lockscreen::BuildAvatar(cfg, out, msg)) {
                // старое превью из кэша выбросить
                for (auto it = imgs_.begin(); it != imgs_.end();)
                    it = it->first.find(out) != std::wstring::npos ? imgs_.erase(it) : std::next(it);
                CopyText(wnd, out);
                ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL);
                logonStatus_ = L"Путь к аватару уже в буфере: в Параметрах «Обзор файлов», Ctrl + V, Enter.";
            } else {
                logonStatus_ = msg;
            }
        }
        if (ui.Button(ID_LOGON_AVATAR_PICK, RectF(bx + ui.S(158), b.Y + ui.S(48), ui.S(110), ui.S(36)),
                      L"Своя...")) {
            Defer([this] {
                std::wstring p = PickPath(false);
                if (p.empty()) return;
                Change();
                cfg.avatarSource = p;
                Save();
            });
        }
        if (!cfg.avatarSource.empty() &&
            ui.Button(ID_LOGON_AVATAR_EMBLEM, RectF(bx + ui.S(276), b.Y + ui.S(48), ui.S(110), ui.S(36)),
                      L"Полумесяц", Kind::Ghost)) {
            Change();
            cfg.avatarSource.clear();
            Save();
        }
        // чёткий фон: политика в HKLM, поэтому .reg с подтверждением Windows
        RectF cr = row(b, 2, L"Чёткий фон вместо размытого");
        bool crisp = whreg::LogonCrisp();
        if (ui.Toggle(ID_LOGON_CRISP, RectF(cr.X + cr.Width - ui.S(42), cr.Y + ui.S(7), ui.S(42), ui.S(30)), crisp)) {
            whreg::Run(whreg::LogonReg(crisp));
            logonStatus_ = L"Подтверди изменение реестра - проверь на Win + L.";
        }
        std::wstring s = logonStatus_.empty()
                             ? L"Под аватаром и полем пароля картинка блокировки мягко затемнена, чтобы они читались. "
                               L"Чёткий фон показывает её как есть, без матового стекла Windows; переход после входа "
                               L"подстраивается сам. После смены картинки нажми «Применить» в карточке выше."
                             : logonStatus_;
        note(b, 3, 4, s, logonStatus_.empty() ? c.muted : c.accent);
    });

    float bottom = std::max(colY[0], colY[1]);
    pageContentH_ = bottom - y0;
    ui.G().Restore(st);
}

}  // namespace studio
