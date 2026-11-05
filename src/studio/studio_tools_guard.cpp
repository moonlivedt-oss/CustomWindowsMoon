// Студия, «Утилиты» - вкладка «Защита»: итог проверки кольцом, находки и расширения браузеров.
// Подробности каждой строки - при наведении: на странице только имя и причина.
#include <windows.h>

#include <algorithm>
#include <cmath>

#include "core/files.h"
#include "core/fx.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "net/net.h"
#include "system/ui/toolsui.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {
// «Опасно» - всегда красное, в любой теме: warn во многих темах почти совпадает с gold, и уровни не различались
const Color kDanger(255, 236, 72, 84);

float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

// «задача планировщика» -> «Задача»
std::wstring KindLabel(tools::FindKind k) {
    std::wstring n = toolsui::FindKindName(k);
    n = n.substr(0, n.find(L' '));
    if (!n.empty()) CharUpperBuffW(n.data(), 1);
    return n;
}

// Цвет значка расширения: по имени, чтобы одно расширение всегда выглядело одинаково.
Color NameHue(const std::wstring& s, const UiColors& c) {
    size_t h = std::hash<std::wstring>{}(s);
    const Color set[] = {c.accent, c.accent2, c.gold, MixColor(c.accent, c.text, 0.45f),
                         MixColor(c.accent2, c.text, 0.35f)};
    return set[h % (sizeof(set) / sizeof(set[0]))];
}
}  // namespace

void Studio::GuardHero(const RectF& b, int danger, int odd, bool busy) {
    const UiColors& c = ui.C();
    ULONGLONG now = GetTickCount64();
    bool still = fx::Game();
    float t = still ? 1 : std::clamp((float)((now - secHeroAt_) / std::max(1.0, fx::Dur(800))), 0.0f, 1.0f);
    if (t < 1) ui.Invalidate();
    float p = EaseOut(t);

    float R = ui.S(33), pw = ui.S(7), cx = b.X + R + pw, cy = b.Y + ui.S(40);
    RectF ring(cx - R, cy - R, R * 2, R * 2);
    Pen back(WithAlpha(c.border, 200), pw);
    ui.G().DrawEllipse(&back, ring);
    auto arc = [&](Color col, float from, float sweep) {
        if (sweep <= 0.5f) return;
        Pen pen(col, pw);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        ui.G().DrawArc(&pen, ring, from, sweep);
    };
    int total = danger + odd;
    if (secView_.running) {  // проверка: дуга бежит по кругу
        ui.Tick();
        arc(c.accent, (float)fmod(now * 0.28, 360.0), 95);
    } else if (!total && secView_.checked) {
        arc(c.accent, -90, 360 * p);
    } else if (total) {
        float dangerSweep = 360.0f * danger / total * p, oddSweep = 360.0f * odd / total * p;
        arc(c.gold, -90 + dangerSweep, oddSweep);
        arc(kDanger, -90, dangerSweep);
        // опасное находится: кольцо несколько секунд дышит, чтобы заметили, и успокаивается
        if (danger && !still && now - secHeroAt_ < 6000) {
            ui.Tick();
            float wave = 0.5f + 0.5f * (float)std::sin((now - secHeroAt_) / 260.0);
            Pen glow(WithAlpha(kDanger, (BYTE)(70 * wave)), pw + ui.S(5));
            ui.G().DrawEllipse(&glow, ring);
        }
    }
    // в середине - сколько нашлось (число набегает) или галочка
    if (secView_.checked && !secView_.running && !total) {
        Pen tick(c.accent, ui.S(3.2f));
        tick.SetStartCap(LineCapRound);
        tick.SetEndCap(LineCapRound);
        tick.SetLineJoin(LineJoinRound);
        PointF pts[] = {PointF(cx - ui.S(11), cy + ui.S(1)), PointF(cx - ui.S(3), cy + ui.S(9)),
                        PointF(cx + ui.S(12), cy - ui.S(9))};
        ui.G().DrawLines(&tick, pts, 3);
    } else if (!secView_.running) {
        ui.Text(secView_.checked ? std::to_wstring((int)std::lround(total * p)) : L"?", ring, c.text, 19, true,
                ui::Center);
    }

    float tx = cx + R + ui.S(20), tw = b.Width - (tx - b.X) - ui.S(200);
    std::wstring title =
        secView_.running    ? std::wstring(L"Проверяю подписи и места запуска")
        : !secView_.checked ? std::wstring(L"Ещё не проверялось")
        : !total            ? std::wstring(L"Ничего странного")
        : danger            ? Plural(danger, L"важная находка", L"важные находки", L"важных находок")
                 : Plural(odd, L"находка: стоит взглянуть", L"находки: стоит взглянуть", L"находок: стоит взглянуть");
    ui.Text(title, RectF(tx, b.Y + ui.S(4), tw, ui.S(24)), c.text, 14, true);
    ui.Text(secView_.checked ? Plural(secView_.processes, L"программа", L"программы", L"программ") + L", " +
                                   Plural(secView_.autoruns, L"запись", L"записи", L"записей") + L" автозапуска"
                             : std::wstring(L"Первая проверка идёт в фоне, до минуты"),
            RectF(tx, b.Y + ui.S(28), tw, ui.S(20)), c.muted, 11.5f);

    // фильтры по серьёзности: щелчок оставляет только такие, повторный - снимает
    float chipX = tx;
    auto chip = [&](int sev, const std::wstring& name, int n, Color col) {
        if (!n) return;
        std::wstring text = name + L"  " + std::to_wstring(n);
        float w = ui.Measure(text, 11, true) + ui.S(24);
        RectF r(chipX, b.Y + ui.S(54), w, ui.S(26));
        int id = ID_GUARD + sev;
        if (ui.Click(id, r)) secSev_ = secSev_ == sev ? 0 : sev;
        float on = ui.Anim(id + 400, secSev_ == sev, 16), hov = ui.Anim(id + 410, ui.Hot(id), 18);
        ui.Fill(r, WithAlpha(col, (BYTE)(38 + 26 * hov + 150 * on)), r.Height / 2);
        ui.Ripple(id, r, r.Height / 2);
        ui.Text(text, r, MixColor(col, c.onAccent, on), 11, true, ui::Center);
        chipX += w + ui.S(8);
    };
    chip(1, L"Опасно", danger, kDanger);
    chip(2, L"Странно", odd, c.gold);

    if (ui.Button(ID_TOOLS_SEC_RESCAN, RectF(b.X + b.Width - ui.S(186), b.Y + ui.S(2), ui.S(186), ui.S(36)),
                  secView_.running   ? L"Проверяю"
                  : secView_.checked ? L"Проверить заново"
                                     : L"Проверить",
                  Kind::Primary, L"", secView_.running) &&
        !secView_.running) {
        toolsui::SecurityRescan();
        secView_.running = true;
        toolsAt_ = 0;
    }
    if (secView_.ignored &&
        ui.Button(ID_TOOLS_SEC_UNHIDE, RectF(b.X + b.Width - ui.S(186), b.Y + ui.S(46), ui.S(186), ui.S(32)),
                  L"Показать скрытые " + std::to_wstring(secView_.ignored), Kind::Ghost)) {
        toolsui::ClearIgnored();
        toolsAt_ = 0;
    }
    (void)busy;
}

void Studio::ToolsGuardTab(ToolsKit& tk) {
    const UiColors& c = ui.C();
    ULONGLONG now = GetTickCount64();
    if (secView_.running) RepaintIn(1000);  // итог проверки придёт сам

    int danger = 0, odd = 0;
    std::vector<tools::FindKind> kinds;
    for (const tools::Suspect& s : secView_.finds) {
        (s.score >= 6 ? danger : odd)++;
        if (std::find(kinds.begin(), kinds.end(), s.kind) == kinds.end()) kinds.push_back(s.kind);
    }
    std::sort(kinds.begin(), kinds.end());
    if (secKind_ > (int)kinds.size()) secKind_ = 0;
    if ((secSev_ == 1 && !danger) || (secSev_ == 2 && !odd)) secSev_ = 0;
    std::vector<size_t> shown;
    for (size_t i = 0; i < secView_.finds.size() && shown.size() < 60; ++i) {
        const tools::Suspect& s = secView_.finds[i];
        if (secKind_ && s.kind != kinds[secKind_ - 1]) continue;
        if ((secSev_ == 1 && s.score < 6) || (secSev_ == 2 && s.score >= 6)) continue;
        shown.push_back(i);
    }

    // итог появился или поменялся - кольцо набирается заново; фильтр поменялся - строки выезжают по очереди
    std::wstring heroKey = std::to_wstring(secView_.checked) + L"|" + std::to_wstring(secView_.running) + L"|" +
                           std::to_wstring(danger) + L"|" + std::to_wstring(odd);
    if (heroKey != secHeroKey_) {
        secHeroKey_ = heroKey;
        secHeroAt_ = now;
    }
    std::wstring rowKey = std::to_wstring(secSev_) + L"|" + std::to_wstring(secKind_) + L"|" +
                          std::to_wstring(shown.size()) + L"|" + std::to_wstring(secView_.checked);
    if (rowKey != secRowKey_) {
        secRowKey_ = rowKey;
        secRowAt_ = now;
    }
    auto rowIn = [&](size_t n, ULONGLONG since) {
        if (fx::Game()) return 1.0f;
        double delay = std::min<size_t>(n, 10) * fx::Dur(36);
        float t = std::clamp((float)((now - since - delay) / std::max(1.0, fx::Dur(320))), 0.0f, 1.0f);
        if (t < 1) ui.Invalidate();
        return EaseOut(t);
    };

    float findH = ui.S(50), heroH = ui.S(92), filterH = kinds.size() > 1 ? ui.S(46) : 0;
    float findsH = heroH + filterH + (shown.empty() ? tk.lineH : shown.size() * findH);
    tk.Wide(L"Находки", findsH / tk.rowH, [&](const RectF& b) {
        GuardHero(b, danger, odd, tk.busy);
        float y = b.Y + heroH;
        if (kinds.size() > 1) {
            std::vector<std::wstring> labels{L"Все"};
            for (tools::FindKind k : kinds) {
                int n = 0;
                for (const tools::Suspect& s : secView_.finds) n += s.kind == k;
                labels.push_back(KindLabel(k) + L" " + std::to_wstring(n));
            }
            int sel = secKind_;
            if (ui.Segmented(ID_TOOLS_SEC_KIND, RectF(b.X, y, std::min(b.Width, ui.S(128) * labels.size()), ui.S(34)),
                             labels, sel))
                secKind_ = sel;
            y += filterH;
        }
        if (shown.empty()) {
            ui.Text(secView_.checked ? L"По этому фильтру пусто." : L"Находки появятся здесь.",
                    RectF(b.X, y, b.Width, tk.lineH), c.muted, 11.5f);
        }
        for (size_t n = 0; n < shown.size(); ++n) {
            const tools::Suspect& s = secView_.finds[shown[n]];
            RectF rr(b.X - ui.S(8), y, b.Width + ui.S(16), findH - ui.S(2));
            ui.RowGlow(rr);
            bool hot = tk.RowHot(rr);
            bool bad = s.score >= 6;
            float in = rowIn(n, secRowAt_), ox = ui.S(18) * (1 - in);
            BYTE a = (BYTE)(255 * in);
            Color sev = bad ? kDanger : c.gold;
            ui.Fill(RectF(b.X - ui.S(8), y + ui.S(10), ui.S(3), findH - ui.S(22)), WithAlpha(sev, a), ui.S(1.5f));
            if (in > 0.5f)
                ui.Icon(ExeDir() + L"\\icons\\" + toolsui::FindKindIcon(s.kind),
                        RectF(b.X + ui.S(6) + ox, y + ui.S(8), ui.S(32), ui.S(32)));
            RectF pill(b.X + ui.S(46) + ox, y + ui.S(14), ui.S(62), ui.S(20));
            ui.Fill(pill, WithAlpha(sev, (BYTE)(40 * in)), pill.Height / 2);
            ui.Text(bad ? L"опасно" : L"странно", pill, WithAlpha(sev, a), 10.5f, true, ui::Center);

            float tx = b.X + ui.S(120) + ox, tw = b.Width - ui.S(120);
            std::wstring name = Ellip(s.name, 60);
            float nw = std::min(tw, ui.Measure(name, 12.5f, true) + ui.S(12));
            ui.Text(name, RectF(tx, y + ui.S(5), nw, ui.S(22)), WithAlpha(c.text, a), 12.5f, true);
            ui.Text(toolsui::FindKindName(s.kind), RectF(tx + nw, y + ui.S(6), std::max(0.0f, tw - nw), ui.S(22)),
                    WithAlpha(c.faint, a), 10.5f);
            ui.Text(Ellip(s.why, 130), RectF(tx, y + ui.S(25), tw, ui.S(20)), WithAlpha(c.muted, a), 10.5f);

            // кнопки выезжают поверх текста, строка не перестраивается
            float ha = ui.Anim(ID_GUARD_ROW + (int)n, hot, 18);
            float cut = ui.S(262);
            if (ha > 0.01f) {
                float cx0 = b.X + b.Width - cut - ui.S(30);
                RectF fade(cx0, y + 1, ui.S(30), findH - ui.S(4));
                LinearGradientBrush br(RectF(fade.X - 1, fade.Y, fade.Width + 2, fade.Height), WithAlpha(c.cardHi, 0),
                                       WithAlpha(c.cardHi, (BYTE)(255 * ha)), LinearGradientModeHorizontal);
                ui.G().FillRectangle(&br, fade);
                ui.Fill(RectF(cx0 + ui.S(30), y + 1, cut + ui.S(8), findH - ui.S(4)),
                        WithAlpha(c.cardHi, (BYTE)(255 * ha)), ui.S(8));
            }
            if (hot) {
                float bx = b.X + b.Width - ui.S(250) + ui.S(26) * (1 - ha);
                if (ui.Button(ID_TOOLS_SEC + (int)n * 2, RectF(bx, y + ui.S(10), ui.S(134), ui.S(28)), L"Что делать",
                              Kind::Primary))
                    toolsui::SecurityAct(s);
                if (ui.Button(ID_TOOLS_SEC + (int)n * 2 + 1, RectF(bx + ui.S(142), y + ui.S(10), ui.S(108), ui.S(28)),
                              L"Это моё")) {
                    toolsui::IgnoreFind(s);
                    Toast(L"Больше не показываю: " + s.name);
                    tk.Refresh();
                    break;
                }
            }
            std::wstring links = s.kind == tools::FIND_TASK && !s.parent.empty() ? s.parent : s.path;
            if (!s.signer.empty()) links += L"\nподпись: " + s.signer;
            ui.Detail(RectF(rr.X, rr.Y, rr.Width - cut, rr.Height), s.name,
                      s.why + (s.where.empty() ? L"" : L".\n" + s.where), links);
            y += findH;
        }
    });

    // Расширения: одно и то же в разных профилях - одной строкой
    std::vector<tools::ExtGroup> groups = tools::GroupExts(secView_.exts);
    int wide = 0;
    for (const tools::ExtGroup& g : groups) wide += tools::ExtVerdict(g.e.rights, g.e.store) >= tools::EXT_V_WIDE;
    std::vector<size_t> extShown;
    for (size_t i = 0; i < groups.size(); ++i)
        if (!extKind_ || tools::ExtVerdict(groups[i].e.rights, groups[i].e.store) >= tools::EXT_V_WIDE)
            extShown.push_back(i);
    size_t extRows = extAll_ ? extShown.size() : std::min<size_t>(extShown.size(), 6);
    bool extMore = extShown.size() > extRows || (extAll_ && extShown.size() > 6);
    std::wstring extKey = std::to_wstring(extKind_) + L"|" + std::to_wstring(extRows);
    if (extKey != extRowKey_) {
        extRowKey_ = extKey;
        extRowAt_ = now;
    }
    float extH = ui.S(48) + std::max<size_t>(extRows, 1) * findH + (extMore ? ui.S(44) : 0);
    tk.Wide(L"Расширения браузеров", extH / tk.rowH, [&](const RectF& b) {
        std::wstring head = !secView_.checked ? std::wstring(L"Появятся после проверки")
                            : groups.empty()  ? std::wstring(L"Расширений с особыми правами нет")
                                             : Plural((int)groups.size(), L"расширение", L"расширения", L"расширений") +
                                                   L", с большими правами: " + std::to_wstring(wide);
        ui.Text(head, RectF(b.X, b.Y, b.Width - ui.S(300), ui.S(36)), c.muted, 11.5f);
        int sel = extKind_;
        if (ui.Segmented(ID_TOOLS_EXT_KIND, RectF(b.X + b.Width - ui.S(290), b.Y, ui.S(290), ui.S(34)),
                         {L"Все", L"С большими правами"}, sel))
            extKind_ = sel;
        float y = b.Y + ui.S(48);
        if (extShown.empty())
            ui.Text(extKind_ ? L"Ни одно расширение не получает входы, буфер и куки сразу на всех сайтах."
                             : L"Chrome, Edge, Brave и Яндекс: расширения с доступом к сайтам и данным.",
                    RectF(b.X, y, b.Width, tk.lineH), c.muted, 11.5f);
        for (size_t n = 0; n < extRows; ++n) {
            const tools::ExtGroup& g = groups[extShown[n]];
            const tools::BrowserExt& e = g.e;
            RectF rr(b.X - ui.S(8), y, b.Width + ui.S(16), findH - ui.S(2));
            ui.RowGlow(rr);
            bool hot = tk.RowHot(rr);
            tools::ExtLevel verdict = tools::ExtVerdict(e.rights, e.store);
            float in = rowIn(n, extRowAt_), ox = ui.S(18) * (1 - in);
            BYTE a = (BYTE)(255 * in);
            // значок: первая буква в круге цвета расширения
            Color hue = NameHue(e.name, c);
            float d = ui.S(32);
            RectF disc(b.X + ui.S(6) + ox, y + ui.S(8), d, d);
            SolidBrush fill(WithAlpha(hue, (BYTE)(46 * in)));
            ui.G().FillEllipse(&fill, disc);
            Pen rim(WithAlpha(hue, (BYTE)(150 * in)), std::max(1.0f, ui.S(1)));
            ui.G().DrawEllipse(&rim, disc);
            ui.Text(e.name.substr(0, 1), disc, WithAlpha(hue, a), 13, true, ui::Center);
            if (verdict != tools::EXT_V_NONE) {
                Color col = verdict == tools::EXT_V_DANGER ? kDanger : verdict == tools::EXT_V_WIDE ? c.gold : c.muted;
                std::wstring tag = verdict == tools::EXT_V_DANGER ? L"опасно"
                                   : verdict == tools::EXT_V_WIDE ? L"много прав"
                                                                  : L"все сайты";
                RectF pill(b.X + ui.S(46) + ox, y + ui.S(14), ui.S(68), ui.S(20));
                ui.Fill(pill, WithAlpha(col, (BYTE)(38 * in)), pill.Height / 2);
                ui.Text(tag, pill, WithAlpha(col, a), 9.5f, true, ui::Center);
            }
            float tx = b.X + ui.S(124) + ox, tw = b.Width - ui.S(124);
            std::wstring name = Ellip(e.name, 60);
            float nw = std::min(tw, ui.Measure(name, 12.5f, true) + ui.S(12));
            ui.Text(name, RectF(tx, y + ui.S(5), nw, ui.S(22)), WithAlpha(c.text, a), 12.5f, true);
            ui.Text(g.where + (e.store ? L"" : L", не из магазина"),
                    RectF(tx + nw, y + ui.S(6), std::max(0.0f, tw - nw), ui.S(22)),
                    WithAlpha(e.store ? c.faint : c.gold, a), 10.5f);
            ui.Text(Ellip(tools::ExtRightsText(e.rights), 130), RectF(tx, y + ui.S(25), tw, ui.S(20)),
                    WithAlpha(c.muted, a), 10.5f);
            float ha = ui.Anim(ID_GUARD_EXT + (int)n, hot, 18);
            float cut = ui.S(208);
            if (ha > 0.01f) {
                float cx0 = b.X + b.Width - cut - ui.S(30);
                RectF fade(cx0, y + 1, ui.S(30), findH - ui.S(4));
                LinearGradientBrush br(RectF(fade.X - 1, fade.Y, fade.Width + 2, fade.Height), WithAlpha(c.cardHi, 0),
                                       WithAlpha(c.cardHi, (BYTE)(255 * ha)), LinearGradientModeHorizontal);
                ui.G().FillRectangle(&br, fade);
                ui.Fill(RectF(cx0 + ui.S(30), y + 1, cut + ui.S(8), findH - ui.S(4)),
                        WithAlpha(c.cardHi, (BYTE)(255 * ha)), ui.S(8));
            }
            if (hot &&
                ui.Button(ID_TOOLS_EXT + (int)n,
                          RectF(b.X + b.Width - ui.S(196) + ui.S(26) * (1 - ha), y + ui.S(10), ui.S(196), ui.S(28)),
                          L"Открыть в браузере", Kind::Primary)) {
                tools::BrowserExt copy = e;
                Defer([copy] { toolsui::OpenExtension(copy); });
            }
            std::wstring body = tools::ExtRightsText(e.rights);
            if (verdict == tools::EXT_V_WIDE)
                body += L". Это права, а не вирус: расширение из магазина, но доверять ему приходится во всём.";
            if (verdict == tools::EXT_V_DANGER)
                body += L". Поставлено не из магазина: проверьте, что это вы его ставили.";
            ui.Detail(RectF(rr.X, rr.Y, rr.Width - cut, rr.Height), e.name, body,
                      g.where + (g.copies > 1 ? L" (" + std::to_wstring(g.copies) + L" профиля)" : L""));
            y += findH;
        }
        if (extMore) {
            std::wstring more = extAll_ ? L"Свернуть" : L"Показать все " + std::to_wstring(extShown.size());
            if (ui.Button(ID_GUARD_MORE, RectF(b.X + (b.Width - ui.S(200)) / 2, y + ui.S(6), ui.S(200), ui.S(32)), more,
                          Kind::Ghost))
                extAll_ = !extAll_;
        }
    });

    bool signReady = tools::CodeSigningReady();
    tk.Wide(L"Подпись своих exe", 1.1f, [&](const RectF& b) {
        ui.Icon(ExeDir() + L"\\icons\\tools-sign.png", RectF(b.X, b.Y + ui.S(2), ui.S(36), ui.S(36)));
        ui.Text(signReady ? L"Сборка подписывает RadialMenu и RadialTools" : L"Сборка пока не подписывает свои exe",
                RectF(b.X + ui.S(48), b.Y, b.Width - ui.S(270), ui.S(40)), c.text, 12.5f);
        ui.Detail(RectF(b.X, b.Y, b.Width - ui.S(220), ui.S(40)), L"Подпись своих exe",
                  signReady
                      ? L"Сертификат MoonRadial Local есть. Чужой файл с этой подписью «Защита» считает подделкой."
                      : L"Своим сертификатом сборка подпишет RadialMenu.exe и RadialTools.exe: меньше поводов у "
                        L"антивируса, а помощник с правами сверяет подпись. Сертификат доверен только у вас.");
        if (!signReady &&
            ui.Button(ID_TOOLS_SIGN, RectF(b.X + b.Width - ui.S(206), b.Y + ui.S(2), ui.S(206), ui.S(36)),
                      L"Настроить подпись", Kind::Secondary, L"", tk.busy) &&
            !tk.busy)
            ToolsRun(L"Подпись", [](const tools::Log& l, ToolsJob&) { return tools::SetupCodeSigning(l); });
        if (toolsJob_ && toolsJob_->title == L"Подпись") tk.Status(b, 1.0f, tk.Log(), c.faint);
    });
}

}  // namespace studio
