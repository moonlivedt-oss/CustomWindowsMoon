// RadialShutUp.exe: приватность Windows одним окном, в духе O&O ShutUp10 - переключатели по группам с уровнями
// «рекомендуется / с оговорками / не рекомендуется», откат каждого из бэкапа, точка восстановления, профили.
// Рисуется тем же набором элементов, что и студия RadialMenu (ui/ui), в цветах её темы из radial.cfg рядом.
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "shutup/catalog.h"
#include "ui/ui.h"

using namespace Gdiplus;
using ui::Kind;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialShutUp";
constexpr UINT_PTR kTickTimer = 1, kMsgTimer = 2;
constexpr UINT WM_RUN = WM_APP + 1;  // отложенные действия: после кадра, не внутри WM_PAINT

// id элементов: у каждого свой диапазон
enum {
    ID_ELEVATE = 1,
    ID_SEARCH,
    ID_FILTER,
    ID_RECOMMENDED,
    ID_LIMITED,
    ID_REVERT,
    ID_RESTORE,
    ID_EXPORT,
    ID_IMPORT,
    ID_LIST,
    ID_CHROME,        // шапка и строка состояния: щелчок по ним не проходит к строкам списка под ними
    ID_GROUP = 100,   // + номер группы: свернуть
    ID_TOGGLE = 1000  // + номер настройки
};

std::wstring ExeFolder() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L'\\'));
}

Color LevelColor(shutup::Level l) {
    switch (l) {
        case shutup::Level::Recommended:
            return Color(255, 92, 196, 120);
        case shutup::Level::Limited:
            return Color(255, 232, 178, 72);
        default:
            return Color(255, 226, 84, 84);
    }
}

const wchar_t* LevelName(shutup::Level l) {
    switch (l) {
        case shutup::Level::Recommended:
            return L"рекомендуется";
        case shutup::Level::Limited:
            return L"с оговорками";
        default:
            return L"не рекомендуется";
    }
}

class App {
public:
    bool Create(HINSTANCE inst, int show);
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);
    void Bind(HWND h) {
        if (!wnd_) wnd_ = h;
    }

private:
    void Reload();
    void Paint();
    void Frame(float w, float h);
    void Say(const std::wstring& s);
    void Toggle(size_t i, bool on);
    void Bulk(shutup::Level upTo);
    void RevertAll();
    void Restore();
    void Export();
    void Import();
    void Elevate();
    bool AskRestorePoint();
    bool Visible(size_t i) const;
    // действие с окнами (диалоги, MessageBox) - после кадра: внутри WM_PAINT модальный цикл рисовал бы поверх
    void Defer(std::function<void()> f) {
        deferred_.push_back(std::move(f));
        PostMessageW(wnd_, WM_RUN, 0, 0);
    }

    HWND wnd_ = nullptr;
    ui::Ui ui_;
    Config cfg_;
    double k_ = 1;
    bool admin_ = false, askedRestore_ = false;
    std::vector<shutup::Status> st_;
    std::vector<std::wstring> groups_;
    std::vector<bool> folded_;
    std::wstring query_, msg_;
    int filter_ = 0;  // все, рекомендуемые, с оговорками, не рекомендуемые, изменённые здесь
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int bw_ = 0, bh_ = 0;
    std::vector<std::function<void()>> deferred_;
};

App* g_app = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) {
        g_app->Bind(h);
        return g_app->Proc(msg, wp, lp);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool App::Create(HINSTANCE inst, int show) {
    LoadConfig(ExeFolder() + L"\\radial.cfg", cfg_);  // нет файла - тема по умолчанию
    admin_ = shutup::IsAdmin();
    groups_ = shutup::Groups();
    folded_.assign(groups_.size(), false);
    Reload();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kClass;
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    k_ = dpi / 96.0;
    int w = (int)(1040 * k_), h = (int)(780 * k_);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    w = std::min(w, (int)(wa.right - wa.left));
    h = std::min(h, (int)(wa.bottom - wa.top));
    std::wstring title = std::wstring(L"RadialShutUp") + (admin_ ? L" (администратор)" : L"");
    wnd_ = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr,
                           nullptr, inst, nullptr);
    if (!wnd_) return false;
    k_ = GetDpiForWindow(wnd_) / 96.0;
    ui_.Attach(wnd_);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(wnd_, 20, &dark, sizeof(dark));  // тёмный заголовок
    UiColors c = UiPalette(cfg_);
    COLORREF cap = RGB(c.rail.GetR(), c.rail.GetG(), c.rail.GetB());
    DwmSetWindowAttribute(wnd_, 35, &cap, sizeof(cap));
    ShowWindow(wnd_, show);
    return true;
}

void App::Reload() {
    shutup::Backup user = shutup::LoadBackup(HKEY_CURRENT_USER), machine = shutup::LoadBackup(HKEY_LOCAL_MACHINE);
    st_.clear();
    for (const shutup::Setting& s : shutup::Catalog()) st_.push_back(shutup::StatusOf(s, user, machine));
}

void App::Say(const std::wstring& s) {
    msg_ = s;
    SetTimer(wnd_, kMsgTimer, 9000, nullptr);
    InvalidateRect(wnd_, nullptr, FALSE);
}

bool App::Visible(size_t i) const {
    const shutup::Setting& s = shutup::Catalog()[i];
    if (filter_ == 1 && s.level != shutup::Level::Recommended) return false;
    if (filter_ == 2 && s.level != shutup::Level::Limited) return false;
    if (filter_ == 3 && s.level != shutup::Level::NotRecommended) return false;
    if (filter_ == 4 && !st_[i].ours) return false;
    if (query_.empty()) return true;
    std::wstring q = Lower(query_);
    return Lower(s.title).find(q) != std::wstring::npos || Lower(s.why).find(q) != std::wstring::npos ||
           Lower(s.group).find(q) != std::wstring::npos;
}

// ShutUp10 перед первой правкой предлагает точку восстановления - и мы так же, раз за запуск
bool App::AskRestorePoint() {
    if (askedRestore_ || !admin_) return true;
    askedRestore_ = true;
    int r = MessageBoxW(wnd_,
                        L"Создать точку восстановления системы перед изменениями?\n\n"
                        L"Каждую настройку и так можно вернуть кнопкой, но точка восстановления спасёт, даже если "
                        L"Windows перестанет загружаться.",
                        L"RadialShutUp", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) Restore();
    return true;
}

void App::Toggle(size_t i, bool on) {
    const shutup::Setting& s = shutup::Catalog()[i];
    if (s.Machine() && !admin_) {
        Say(L"«" + std::wstring(s.title) +
            L"» - для всего компьютера: нужны права. Нажмите «Перезапустить с правами».");
        return;
    }
    if (!on && !st_[i].ours) {
        Say(L"Это включено не RadialShutUp - возвращать нечего: так настроена сама Windows или другая программа.");
        return;
    }
    if (on && !AskRestorePoint()) return;
    std::wstring line;
    shutup::Apply(s, on, [&](const std::wstring& m) { line = m; });
    Reload();
    Say(line);
}

void App::Bulk(shutup::Level upTo) {
    if (!AskRestorePoint()) return;
    int done = 0, skipped = 0, failed = 0;
    const auto& all = shutup::Catalog();
    for (size_t i = 0; i < all.size(); ++i) {
        if ((int)all[i].level > (int)upTo || st_[i].state == shutup::State::On) continue;
        if (all[i].Machine() && !admin_) {
            ++skipped;
            continue;
        }
        (shutup::Apply(all[i], true, nullptr) ? done : failed)++;
    }
    Reload();
    std::wstring s = L"Включено: " + std::to_wstring(done);
    if (skipped) s += L". Без прав пропущено: " + std::to_wstring(skipped) + L" (для всего компьютера)";
    if (failed) s += L". Не получилось: " + std::to_wstring(failed);
    Say(s + L".");
}

void App::RevertAll() {
    int done = 0, skipped = 0, failed = 0;
    const auto& all = shutup::Catalog();
    for (size_t i = 0; i < all.size(); ++i) {
        if (!st_[i].ours) continue;
        if (all[i].Machine() && !admin_) {
            ++skipped;
            continue;
        }
        (shutup::Apply(all[i], false, nullptr) ? done : failed)++;
    }
    Reload();
    if (!done && !skipped && !failed) {
        Say(L"Возвращать нечего: RadialShutUp ничего не менял.");
        return;
    }
    std::wstring s = L"Как было: " + std::to_wstring(done);
    if (skipped) s += L". Для всего компьютера нужны права: " + std::to_wstring(skipped);
    if (failed) s += L". Не получилось: " + std::to_wstring(failed);
    Say(s + L".");
}

void App::Restore() {
    if (!admin_) {
        Say(L"Точку восстановления создаёт только администратор - перезапустите с правами.");
        return;
    }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring why;
    bool ok = shutup::CreateRestorePoint(L"RadialShutUp: до изменения приватности", why);
    Say(ok ? L"Точка восстановления создана (Windows делает не больше одной в сутки - свежая могла не добавиться)."
           : L"Точку восстановления создать не удалось: " + why + L".");
}

void App::Export() {
    wchar_t file[MAX_PATH] = L"RadialShutUp.txt";
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = wnd_;
    of.lpstrFilter = L"Профиль RadialShutUp (*.txt)\0*.txt\0Все файлы\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"txt";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&of)) return;
    std::vector<std::wstring> ids;
    const auto& all = shutup::Catalog();
    for (size_t i = 0; i < all.size(); ++i)
        if (st_[i].state == shutup::State::On) ids.push_back(all[i].id);
    std::string bytes = Narrow(shutup::EncodeProfile(ids));
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    Say(ok ? L"Профиль сохранён: " + std::to_wstring(ids.size()) + L" включённых." : L"Не удалось записать файл.");
}

void App::Import() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = wnd_;
    of.lpstrFilter = L"Профиль RadialShutUp (*.txt)\0*.txt\0Все файлы\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Say(L"Не удалось открыть файл.");
        return;
    }
    std::string bytes;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got && bytes.size() < (1 << 20)) bytes.append(buf, got);
    CloseHandle(f);
    std::vector<std::wstring> ids = shutup::DecodeProfile(Widen(bytes));
    if (ids.empty()) {
        Say(L"В файле нет настроек RadialShutUp.");
        return;
    }
    if (!AskRestorePoint()) return;
    int done = 0, skipped = 0;
    for (const std::wstring& id : ids) {
        const shutup::Setting* s = shutup::Find(id);
        if (!s) continue;
        if (s->Machine() && !admin_) {
            ++skipped;
            continue;
        }
        done += shutup::Apply(*s, true, nullptr);
    }
    Reload();
    Say(L"Из профиля включено: " + std::to_wstring(done) +
        (skipped ? L", без прав пропущено: " + std::to_wstring(skipped) : L"") + L".");
}

void App::Elevate() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW si{sizeof(si)};
    si.lpVerb = L"runas";
    si.lpFile = exe;
    si.nShow = SW_SHOWNORMAL;
    si.hwnd = wnd_;
    if (ShellExecuteExW(&si))
        PostMessageW(wnd_, WM_CLOSE, 0, 0);
    else
        Say(L"Права не получены.");
}

void App::Paint() {
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
        ui_.Begin(g, UiPalette(cfg_), k_);
        Frame((float)w, (float)h);
        ui_.End();
    }
    BitBlt(dc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    EndPaint(wnd_, &ps);
    if (ui_.TakeRepaint())
        InvalidateRect(wnd_, nullptr, FALSE);
    else if (ui_.TakeTick())
        SetTimer(wnd_, kTickTimer, 33, nullptr);
}

void App::Frame(float w, float h) {
    ui::Ui& u = ui_;
    const UiColors& c = u.C();
    u.Fill(RectF(0, 0, w, h), c.win);
    const auto& all = shutup::Catalog();
    const float pad = u.S(24), top = u.S(178), bottom = u.S(40);

    // ---------------- список по группам ----------------
    RectF view(0, top, w, std::max(0.0f, h - top - bottom));
    const float rowH = u.S(66), headH = u.S(48), gap = u.S(14), cardW = std::min(w - 2 * pad, u.S(1100));
    const float cardX = (w - cardW) / 2;
    float content = u.S(6);
    std::vector<std::vector<size_t>> rows(groups_.size());
    for (size_t i = 0; i < all.size(); ++i)
        if (Visible(i)) {
            size_t gi = std::find(groups_.begin(), groups_.end(), all[i].group) - groups_.begin();
            rows[gi].push_back(i);
        }
    for (size_t gi = 0; gi < groups_.size(); ++gi)
        if (!rows[gi].empty()) content += headH + (folded_[gi] ? 0 : rows[gi].size() * rowH) + gap;
    float off = u.Scroll(ID_LIST, view, content + u.S(10));
    GraphicsState clip = u.G().Save();
    u.G().SetClip(view);
    float y = top + u.S(6) - off;
    bool any = false;
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
        if (rows[gi].empty()) continue;
        any = true;
        float folded = u.Anim(ID_GROUP + (int)gi, folded_[gi], 18);
        float bodyH = rows[gi].size() * rowH * (1 - folded);
        RectF card(cardX, y, cardW, headH + bodyH);
        if (card.Y + card.Height >= view.Y && card.Y <= view.Y + view.Height) {
            u.Fill(card, c.card, u.S(14));
            u.Stroke(card, c.border, u.S(14));
            // заголовок группы: имя, сколько включено и полоска доли
            int on = 0;
            for (size_t i : rows[gi]) on += st_[i].state == shutup::State::On;
            RectF head(cardX, y, cardW, headH);
            if (u.Click(ID_GROUP + (int)gi, head)) folded_[gi] = !folded_[gi];
            float hot = u.Anim(ID_GROUP + 50 + (int)gi, u.Hot(ID_GROUP + (int)gi), 16);
            if (hot > 0.01f) u.Fill(head, WithAlpha(c.text, (BYTE)(10 * hot)), u.S(14));
            {  // галочка свёртки: вниз - открыта, поворачивается вправо, когда группа свёрнута
                GraphicsState st = u.G().Save();
                u.G().TranslateTransform(cardX + u.S(24), y + headH / 2);
                u.G().RotateTransform(-90.0f * folded);
                Pen pen(MixColor(c.muted, c.text, hot), u.S(1.8f));
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                pen.SetLineJoin(LineJoinRound);
                PointF pts[3] = {{-u.S(4.5f), -u.S(2.2f)}, {0, u.S(2.6f)}, {u.S(4.5f), -u.S(2.2f)}};
                u.G().DrawLines(&pen, pts, 3);
                u.G().Restore(st);
            }
            u.Text(groups_[gi], RectF(cardX + u.S(36), y + u.S(13), cardW * 0.5f, u.S(24)), c.text, 14.5f, true);
            std::wstring count = L"включено " + std::to_wstring(on) + L" из " + std::to_wstring(rows[gi].size());
            float bw = u.S(120), bx = cardX + cardW - u.S(20) - bw;
            u.Text(count, RectF(bx - u.S(170), y + u.S(15), u.S(160), u.S(20)), c.muted, 11.0f, false, ui::Right);
            RectF track(bx, y + u.S(22), bw, u.S(5));
            u.Fill(track, c.field, u.S(3));
            float share = (float)on / rows[gi].size();
            if (share > 0) u.Fill(RectF(bx, track.Y, bw * share, track.Height), c.accent, u.S(3));
        }
        y += headH;
        if (bodyH > 0.5f) {
            GraphicsState inner = u.G().Save();
            u.G().SetClip(RectF(cardX, y, cardW, bodyH), CombineModeIntersect);
            for (size_t n = 0; n < rows[gi].size(); ++n) {
                size_t i = rows[gi][n];
                const shutup::Setting& s = all[i];
                RectF row(cardX, y + n * rowH, cardW, rowH);
                if (row.Y + row.Height < view.Y || row.Y > view.Y + view.Height || row.Y > y + bodyH) continue;
                u.Fill(RectF(cardX + u.S(16), row.Y, cardW - u.S(32), 1), c.border);  // разделитель
                bool on = st_[i].state == shutup::State::On;
                RectF tg(cardX + u.S(20), row.Y + u.S(14), u.S(44), u.S(26));
                bool v = on;
                if (u.Toggle(ID_TOGGLE + (int)i, tg, v) && folded < 0.01f) Defer([this, i, v] { Toggle(i, v); });
                // уровень - цветная точка у заголовка, как светофор ShutUp10
                Color lc = LevelColor(s.level);
                float tx = cardX + u.S(80);
                u.Fill(RectF(tx, row.Y + u.S(19), u.S(8), u.S(8)), lc, u.S(4));
                u.Tip(ID_TOGGLE + (int)i, LevelName(s.level));
                float titleW = u.Measure(s.title, 13.0f, true);
                u.Text(s.title, RectF(tx + u.S(16), row.Y + u.S(11), cardW * 0.62f, u.S(22)), on ? c.text : c.muted,
                       13.0f, true);
                // метки справа от заголовка: частично, для компьютера, изменено здесь
                float cx = tx + u.S(16) + std::min(titleW, cardW * 0.62f) + u.S(10);
                auto chip = [&](const std::wstring& t, Color bg, Color fg) {
                    float cw = u.Measure(t, 10.0f) + u.S(14);
                    u.Chip(RectF(cx, row.Y + u.S(12), cw, u.S(19)), t, bg, fg);
                    cx += cw + u.S(6);
                };
                if (st_[i].state == shutup::State::Partial) chip(L"частично", c.warnBg, c.warn);
                if (s.Machine()) chip(L"для всех", c.field, c.muted);
                if (st_[i].ours) chip(L"изменено здесь", WithAlpha(c.accent, 40), c.accent);
                u.Text(s.why, RectF(tx + u.S(16), row.Y + u.S(34), cardW - u.S(140), u.S(30)), c.muted, 11.0f, false,
                       ui::Left, true);
            }
            u.G().Restore(inner);
        }
        y += bodyH + gap;
    }
    if (!any)
        u.EmptyState(ID_LIST + 1, RectF(cardX, top + u.S(40), cardW, u.S(200)), L"Ничего не найдено - смените фильтр.");
    u.G().Restore(clip);

    // ---------------- шапка поверх списка ----------------
    u.Fill(RectF(0, 0, w, top), c.win);
    u.Hover(ID_CHROME, RectF(0, 0, w, top));
    u.Text(L"RadialShutUp", RectF(pad, u.S(18), u.S(400), u.S(34)), c.text, 22.0f, true);
    u.Text(L"Приватность Windows: понятные переключатели, каждый можно вернуть как было.",
           RectF(pad, u.S(52), w * 0.6f, u.S(22)), c.muted, 12.0f);
    if (admin_) {
        std::wstring t = L"С правами администратора";
        float cw = u.Measure(t, 10.5f) + u.S(20);
        u.Chip(RectF(w - pad - cw, u.S(26), cw, u.S(24)), t, WithAlpha(c.accent, 46), c.accent);
    } else if (u.Button(ID_ELEVATE, RectF(w - pad - u.S(230), u.S(20), u.S(230), u.S(36)), L"Перезапустить с правами",
                        Kind::Primary)) {
        Defer([this] { Elevate(); });
    }
    // поиск и фильтр
    u.Field(
        ID_SEARCH, RectF(pad, u.S(84), u.S(260), u.S(34)), query_, [this](const std::wstring& v) { query_ = v; },
        L"Поиск настройки");
    u.Segmented(ID_FILTER, RectF(pad + u.S(274), u.S(84), std::min(u.S(620), w - pad * 2 - u.S(274)), u.S(34)),
                {L"Все", L"Рекомендуемые", L"С оговорками", L"Не рекомендуемые", L"Изменённые"}, filter_);
    // действия
    float bx = pad, by = u.S(130), bh = u.S(34);
    auto button = [&](int id, const std::wstring& t, Kind kind) {
        float bw = u.Measure(t, 12.0f) + u.S(32);
        bool hit = u.Button(id, RectF(bx, by, bw, bh), t, kind);
        bx += bw + u.S(8);
        return hit;
    };
    if (button(ID_RECOMMENDED, L"Включить рекомендуемые", Kind::Primary))
        Defer([this] { Bulk(shutup::Level::Recommended); });
    if (button(ID_LIMITED, L"+ с оговорками", Kind::Secondary)) Defer([this] { Bulk(shutup::Level::Limited); });
    if (button(ID_REVERT, L"Вернуть всё как было", Kind::Secondary)) Defer([this] { RevertAll(); });
    if (button(ID_RESTORE, L"Точка восстановления", Kind::Ghost)) Defer([this] { Restore(); });
    if (button(ID_EXPORT, L"Сохранить профиль", Kind::Ghost)) Defer([this] { Export(); });
    if (button(ID_IMPORT, L"Загрузить профиль", Kind::Ghost)) Defer([this] { Import(); });
    u.Fill(RectF(0, top - 1, w, 1), c.border);

    // ---------------- строка состояния ----------------
    RectF bar(0, h - bottom, w, bottom);
    u.Fill(bar, c.rail);
    u.Hover(ID_CHROME, bar);
    u.Fill(RectF(0, bar.Y, w, 1), c.border);
    int on = 0, ours = 0;
    for (const shutup::Status& s : st_) {
        on += s.state == shutup::State::On;
        ours += s.ours;
    }
    std::wstring left = msg_.empty() ? L"Включено " + std::to_wstring(on) + L" из " + std::to_wstring(all.size()) +
                                           L", изменено RadialShutUp: " + std::to_wstring(ours)
                                     : msg_;
    u.Text(left, RectF(pad, bar.Y + u.S(10), w * 0.62f, u.S(20)), msg_.empty() ? c.muted : c.text, 11.0f);
    float lx = w - pad;
    for (int l = 2; l >= 0; --l) {  // легенда уровней справа
        auto lev = (shutup::Level)l;
        float tw = u.Measure(LevelName(lev), 10.5f);
        lx -= tw;
        u.Text(LevelName(lev), RectF(lx, bar.Y + u.S(11), tw + u.S(4), u.S(18)), c.muted, 10.5f);
        lx -= u.S(14);
        u.Fill(RectF(lx, bar.Y + u.S(16), u.S(8), u.S(8)), LevelColor(lev), u.S(4));
        lx -= u.S(18);
    }
    u.DrawTip(w, h);
}

LRESULT App::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    auto after = [&] {
        if (ui_.TakeRepaint()) InvalidateRect(wnd_, nullptr, FALSE);
    };
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd_, 0};
            TrackMouseEvent(&tme);
            ui_.OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp));
            if (ui_.HotChanged()) SetTimer(wnd_, kTickTimer, 650, nullptr);  // подсказка, если задержаться
            after();
            return 0;
        }
        case WM_MOUSELEAVE:
            ui_.OnLeave();
            after();
            return 0;
        case WM_LBUTTONDOWN:
            ui_.OnDown((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_LBUTTONUP:
            ui_.OnUp((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_LBUTTONDBLCLK:
            ui_.OnDown((short)LOWORD(lp), (short)HIWORD(lp));
            ui_.OnDouble((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(wnd_, &p);
            ui_.OnWheel(p.x, p.y, (short)HIWORD(wp));
            after();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (HWND)wp == wnd_ && ui_.OnSetCursor()) return TRUE;
            break;
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && ui_.OnEditChange((HWND)lp)) InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, ui_.FieldText());
            SetBkColor(dc, ui_.FieldBack());
            return (LRESULT)ui_.FieldBrush();
        }
        case WM_RUN: {
            auto todo = std::move(deferred_);
            deferred_.clear();
            for (auto& f : todo) f();
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            KillTimer(wnd_, wp);
            if (wp == kMsgTimer) msg_.clear();
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) {  // вернулись в окно - вдруг что-то поменяли в Параметрах
                Reload();
                InvalidateRect(wnd_, nullptr, FALSE);
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {(LONG)(900 * k_), (LONG)(560 * k_)};
            return 0;
        }
        case WM_DPICHANGED: {
            k_ = HIWORD(wp) / 96.0;
            RECT* r = (RECT*)lp;
            SetWindowPos(wnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                ui_.FocusField(ID_SEARCH);
                return 0;
            }
            break;
        case WM_DESTROY:
            if (memDC_) {
                SelectObject(memDC_, oldBmp_);
                DeleteObject(dib_);
                DeleteDC(memDC_);
                memDC_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    OleInitialize(nullptr);
    GdiplusStartupInput gin;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gin, nullptr);
    int code = 0;
    {
        static App app;
        g_app = &app;
        if (app.Create(inst, show)) {
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            code = (int)m.wParam;
        }
        g_app = nullptr;
    }
    GdiplusShutdown(token);
    OleUninitialize();
    return code;
}
