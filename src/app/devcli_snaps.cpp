// Служебные режимы: снимки и фильмы без мыши - живые сцены, эталоны студии (--snap-suite), замер кольца.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "app/devcli_internal.h"
#include "core/bg.h"
#include "core/files.h"
#include "core/perf.h"
#include "core/themes.h"
#include "cards/boostcard.h"
#include "cards/translatecard.h"
#include "helpers/cliphist.h"
#include "helpers/snip.h"
#include "helpers/translate.h"
#include "media/widget.h"
#include "modes/boostpick.h"
#include "system/explain.h"
#include "gfx/scene.h"
#include "ring/menu.h"
#include "studio/studio.h"

namespace devcli {

namespace {

// Эталонные снимки студии (И33): --snap-suite <эталоны> <выход> [update]. Каждый раздел снимается отдельным
// процессом (--studio-snap с RADIAL_DEMO=1), потом сравнивается с эталоном попиксельно: пиксель «другой», если
// канал разошёлся больше чем на 24; раздел не прошёл - если таких больше 0,5%. Разница - картинкой <раздел>-diff.png
// (красным поверх бледного эталона). update - записать нынешние как эталоны. Итог - snaps_out.txt, код 0/1.
struct SnapCase {
    int page, tab;
    const wchar_t* name;
    int scroll = 0;  // RADIAL_SCROLL: снять не верх страницы, а ниже
};
// page -1 - кольцо утилит на выдуманных данных (--tools-ring-snap ... demo), tab - раскрытая группа
const SnapCase kSnapCases[] = {{2, 0, L"behavior"},   {13, 0, L"aids"},       {1, 0, L"themes"},
                               {9, 0, L"keys"},       {3, 0, L"desk"},        {7, 7, L"privacy"},
                               {7, 0, L"tools-path"}, {7, 1, L"tools-disks"}, {-1, 4, L"tools-ring"},
                               {6, 0, L"net", 420}};  // «Сеть»: скорость и Wi-Fi (проверка сети на снимке не идёт)

const CLSID kPngEncoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

bool SnapRun(const SnapCase& c, const std::wstring& out) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = c.page < 0 ? L"\"" + std::wstring(exe) + L"\" --tools-ring-snap \"" + out + L"\" " +
                                        std::to_wstring(c.tab) + L" demo"
                                  : L"\"" + std::wstring(exe) + L"\" --studio-snap " + std::to_wstring(c.page) + L" " +
                                        std::to_wstring(c.tab) + L" \"" + out + L"\" 2500";
    SetEnvironmentVariableW(L"RADIAL_DEMO", L"1");
    SetEnvironmentVariableW(L"RADIAL_SCROLL", c.scroll ? std::to_wstring(c.scroll).c_str() : nullptr);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    bool ok = WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0;
    if (!ok) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ok && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Доля отличающихся пикселей; diff - картинка разницы (пустой путь - не писать). -1 - не открылось или размер другой.
double SnapDiff(const std::wstring& ref, const std::wstring& now, const std::wstring& diff) {
    Gdiplus::Bitmap a(ref.c_str()), b(now.c_str());
    if (a.GetLastStatus() != Gdiplus::Ok || b.GetLastStatus() != Gdiplus::Ok || a.GetWidth() != b.GetWidth() ||
        a.GetHeight() != b.GetHeight())
        return -1;
    int w = (int)a.GetWidth(), h = (int)a.GetHeight();
    Gdiplus::Rect r(0, 0, w, h);
    Gdiplus::BitmapData da, db;
    a.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &da);
    b.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &db);
    Gdiplus::Bitmap out(w, h, PixelFormat32bppARGB);
    Gdiplus::BitmapData dout;
    out.LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &dout);
    long bad = 0;
    for (int y = 0; y < h; ++y) {
        auto* pa = (const uint32_t*)((const BYTE*)da.Scan0 + y * da.Stride);
        auto* pb = (const uint32_t*)((const BYTE*)db.Scan0 + y * db.Stride);
        auto* po = (uint32_t*)((BYTE*)dout.Scan0 + y * dout.Stride);
        for (int x = 0; x < w; ++x) {
            uint32_t u = pa[x], v = pb[x];
            int d = 0;
            for (int sh = 0; sh < 32; sh += 8) d = std::max(d, std::abs((int)(u >> sh & 255) - (int)(v >> sh & 255)));
            bool differs = d > 24;
            bad += differs;
            uint32_t g = ((u >> 16 & 255) + (u >> 8 & 255) + (u & 255)) / 3 / 3 + 170;  // бледный эталон
            po[x] = differs ? 0xFFFF2040u : 0xFF000000u | g << 16 | g << 8 | g;
        }
    }
    a.UnlockBits(&da);
    b.UnlockBits(&db);
    out.UnlockBits(&dout);
    if (!diff.empty() && bad) out.Save(diff.c_str(), &kPngEncoder, nullptr);
    return (double)bad / ((double)w * h);
}

}  // namespace

// Выдуманная чистка для снимков студии (RADIAL_SCENE=clean): четыре места по очереди, как настоящая работа.
DWORD WINAPI FakeClean(void*) {
    const ULONGLONG GB = 1024ull * 1024 * 1024;
    std::vector<scene::CleanItem> items = {{L"Кэш npm", 7600 * GB / 1000},
                                           {L"Кэш pip", 5 * GB},
                                           {L"TEMP старше суток", 4600 * GB / 1000},
                                           {L"Кэш uv", 900 * GB / 1000}};
    Sleep(300);
    scene::CleanBegin(items);
    for (size_t i = 0; i < items.size(); ++i) {
        Sleep(150);
        scene::CleanMark(i, 1);
        Sleep(750);
        scene::CleanMark(i, 2, items[i].bytes * 97 / 100);
    }
    scene::CleanEnd();
    return 0;
}

// Лист кадров живой сцены на выдуманных данных (как в карточке, 384 dip): --scene-film clean|guard|guard0 out.png.
// Лента ведётся по времени, как настоящая работа; кадры снимаются в заданные мгновения.
int SceneFilm(const std::wstring& what, const std::wstring& out) {
    using namespace Gdiplus;
    ThemeInfo th = CurrentTheme(LoadOwnConfig());
    scene::Palette p;
    p.text = ToColor(th.fg);
    p.muted = ToColor(th.muted);
    p.faint = MixColor(p.muted, ToColor(th.bg), 0.45);
    p.accent = ToColor(th.ac);
    p.accent2 = ToColor(th.glow);
    p.gold = ToColor(th.gold);
    p.card = ToColor(th.panel);
    bool clean = what == L"clean";
    const ULONGLONG GB = 1024ull * 1024 * 1024;
    std::vector<scene::CleanItem> items = {{L"Кэш npm", 7600 * GB / 1000},
                                           {L"Кэш pip", 5 * GB},
                                           {L"TEMP старше суток", 4600 * GB / 1000},
                                           {L"Кэш uv", 900 * GB / 1000}};
    std::vector<scene::GuardFind> finds;
    if (what == L"guard")
        finds = {{L"scvhost.exe", L"имя как у svchost, а лежит в TEMP; без подписи", 7},
                 {L"wscript.exe", L"запуск скрипта при входе из Run", 4}};
    std::vector<double> at = clean ? std::vector<double>{0.35, 0.8, 1.3, 1.9, 2.6, 3.4, 4.1, 4.6, 5.2, 6.4}
                                   : std::vector<double>{0.3, 0.9, 1.6, 2.1, 2.4, 2.8, 3.3, 4.2};
    float k = 1.25f, W = 352 * k;
    float H = clean ? scene::CleanScene::Height(items.size(), k) : scene::GuardScene::Height(finds.size(), k);
    int cols = 2, rows = (int)(at.size() + 1) / 2;
    int cw = (int)(W + 40 * k), ch = (int)(H + 40 * k);
    Bitmap sheet(cw * cols, ch * rows, PixelFormat32bppARGB);
    Graphics sg(&sheet);
    sg.Clear(ToColor(th.bg));
    Bitmap frame((int)W, (int)H, PixelFormat32bppARGB);
    scene::CleanScene cs;
    scene::GuardScene gsn;
    if (clean)
        scene::CleanBegin(items);
    else
        scene::GuardBegin(140);
    ULONGLONG t0 = GetTickCount64();
    size_t next = 0;
    bool ended = false;
    while (next < at.size()) {
        double t = (GetTickCount64() - t0) / 1000.0;
        if (clean) {
            for (size_t i = 0; i < items.size(); ++i) {
                double s = 0.4 + i * 0.9;
                if (t > s + 0.75)
                    scene::CleanMark(i, 2, items[i].bytes * 97 / 100);
                else if (t > s)
                    scene::CleanMark(i, 1);
            }
            if (!ended && t > 0.4 + items.size() * 0.9) ended = true, scene::CleanEnd();
        } else if (!ended && t > 1.8) {
            ended = true;
            scene::GuardEnd(140, finds);
        }
        Graphics g(&frame);
        g.Clear(ToColor(th.bg));
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        POINT m{-100, -100};
        if (clean)
            cs.Draw(g, RectF(0, 0, W, H), k, p, m, false);
        else
            gsn.Draw(g, RectF(0, 0, W, H), k, p, m);
        if (t >= at[next]) {
            int x = (int)(next % cols) * cw, y = (int)(next / cols) * ch;
            sg.DrawImage(&frame, x + 20 * k, y + 28 * k);
            SolidBrush lb(p.faint);
            Font f(L"Segoe UI", 11 * k, FontStyleRegular, UnitPixel);
            wchar_t s[32];
            swprintf(s, 32, L"%.1f с", at[next]);
            sg.DrawString(s, -1, &f, PointF(x + 20 * k, y + 6 * k), &lb);
            ++next;
        }
        Sleep(15);
    }
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png"))
            return sheet.Save(out.c_str(), &codecs[i].Clsid, nullptr) == Ok ? 0 : 1;
    return 1;
}

namespace {

// Панель «Сейчас играет» на выдуманном треке: media-full|media-mini и обложка -bright|-dark|-none в имени
int MediaSnap(const std::wstring& what, const Config& cfg, const std::wstring& out) {
    using namespace Gdiplus;
    media::Track t;
    t.has = true;
    t.status = media::kPlaying;
    t.title = L"Очень длинное название песни, которое не помещается в строку";
    t.artist = L"Исполнитель";
    t.app = L"Spotify";
    t.end = 2100000000;
    t.position = 840000000;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    t.updated = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    bool bright = what.find(L"-bright") != std::wstring::npos, dark = what.find(L"-dark") != std::wstring::npos;
    if (bright || dark) {
        Bitmap art(300, 300, PixelFormat32bppRGB);
        {
            Graphics g(&art);
            LinearGradientBrush fill(PointF(0, 0), PointF(300, 300),
                                     bright ? Color(255, 255, 244, 190) : Color(255, 44, 30, 70),
                                     bright ? Color(255, 250, 190, 210) : Color(255, 20, 70, 90));
            g.FillRectangle(&fill, 0, 0, 300, 300);
        }
        t.art = snipimg::ToPng(&art);
        t.artId = 1;
    }
    auto* widget = new MediaWidget;  // живёт до конца процесса: картинки нельзя разбирать после GDI+
    double k = GetDpiForSystem() / 96.0;
    if (what.find(L"-sim") != std::wstring::npos) {  // настоящий такт: out - папка для кадров и журнала
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        media::Track b = t;
        b.title = L"Гравитация";
        b.artist = L"Орбита";
        b.id = 2;
        b.position = 0;
        b.end = 1700000000;
        t.id = 1;
        Bitmap art(300, 300, PixelFormat32bppRGB);
        {
            Graphics g(&art);
            LinearGradientBrush fill(PointF(0, 0), PointF(300, 300), Color(255, 94, 224, 208), Color(255, 59, 84, 200));
            g.FillRectangle(&fill, 0, 0, 300, 300);
        }
        b.art = snipimg::ToPng(&art);
        b.artId = 2;
        widget->DemoRun(GetModuleHandleW(nullptr), cfg, t, b, out);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    if (what.find(L"-bench") != std::wstring::npos) {  // замер: итог - в файл out рядом с exe
        double spin = 0, full = 0;
        widget->Bench(cfg, t, k, spin, full);
        wchar_t line[120];
        swprintf(line, 120, L"кадр пластинки %.2f мс, полная перерисовка %.2f мс\r\n", spin, full);
        WriteOut(out.c_str(), line);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    MediaWidget::SnapState st;
    if (what.find(L"mini") != std::wstring::npos) st.morph = 0;
    if (what.find(L"-morph") != std::wstring::npos) st.morph = 0.5f;
    if (what.find(L"-roll") != std::wstring::npos) st.rx = -95, st.ang = 200;
    if (what.find(L"-lyr") != std::wstring::npos) st.expand = 1;
    if (what.find(L"-paused") != std::wstring::npos) {
        st.arm = -22;
        t.status = media::kPaused;
    }
    bool ok = widget->SnapFrame(cfg, t, st, k, out);
    TerminateProcess(GetCurrentProcess(), ok ? 0 : 1);
    return ok ? 0 : 1;
}

}  // namespace

int CardSnap(const std::wstring& what, bool more, const std::wstring& out) {
    using namespace Gdiplus;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdip gdip;
    Config cfg = LoadOwnConfig();
    if (what.rfind(L"media", 0) == 0) return MediaSnap(what, cfg, out);
    ThemeInfo theme = CurrentTheme(cfg);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static BoostCard card;
    static BoostPick pick;
    static SnipCard snip;
    if (what == L"later-sim") {  // меню карточки настоящими сообщениями окна: out - папка для кадров, журнал - later_out.txt
        card.Create(inst);
        int yesPressed = 0;
        std::wstring log;
        auto pump = [&](int ms) {
            for (ULONGLONG until = GetTickCount64() + ms; GetTickCount64() < until;) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
                card.Tick();
                Sleep(16);
            }
        };
        auto note = [&](const wchar_t* what2) {
            wchar_t line[160];
            swprintf(line, 160, L"%-26ls visible=%d later=%d repeat=%d yes=%d\r\n", what2, card.Visible() ? 1 : 0,
                     card.LaterCount(), card.RepeatMinutes(), yesPressed);
            log += line;
        };
        auto show = [&] {
            card.yesLabel = L"Что закрыть";
            card.noLabel = L"Подкачка";
            card.onYes = [&yesPressed] { ++yesPressed; };
            card.meter = 0.93f;
            card.more = L"Подробности.";
            card.Show(BoostCard::OFFER, L"Память на пределе", L"Занято 25,6 ГБ из 27,6 ГБ, вместе с подкачкой.", theme, 60000,
                      Toast::G_WARN);
        };
        show();
        pump(1200);
        note(L"shown");
        card.DemoRightClick();
        pump(500);
        card.SaveFrame(out + L"\\menu.png");
        note(L"menu open");
        card.DemoClickMenu(1, 1);  // повторять каждые 10 минут
        pump(300);
        card.SaveFrame(out + L"\\menu-note.png");
        note(L"chose repeat 10");
        pump(1400);
        note(L"left, waiting");
        card.DemoFastForward();
        pump(1300);
        card.SaveFrame(out + L"\\menu-repeat.png");
        note(L"came back");
        card.DemoExpire();  // ушла сама - вернётся
        pump(1000);
        note(L"timed out");
        card.DemoFastForward();
        pump(1300);
        note(L"came back again");
        card.DemoRightClick();
        pump(400);
        card.SaveFrame(out + L"\\menu-active.png");
        card.DemoClickMenu(1, 1);  // та же кнопка: выключить повтор
        pump(1200);
        note(L"repeat switched off");
        card.DemoExpire();
        pump(1000);
        note(L"timed out, no repeat");
        show();
        pump(1200);
        card.DemoRightClick();
        pump(300);
        card.DemoClickMenu(0, 0);  // напомнить через 5 минут
        pump(1400);
        note(L"snoozed 5");
        card.DemoFastForward();
        pump(1300);
        note(L"snooze came back");
        card.DemoClickYes();  // кнопка карточки работает и после возвращения
        pump(900);
        note(L"pressed action");
        WriteOut(L"later_out.txt", log);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    Toast* shown = nullptr;
    bool isSnip = what.rfind(L"snip", 0) == 0;
    if (isSnip) {  // snip-wide|snip-tall|snip-tiny и -saved: кадр самой карточки, без рабочего стола вокруг
        snip.Create(inst);
        snip.Apply(cfg);
        bool tall = what.find(L"-tall") != std::wstring::npos, tiny = what.find(L"-tiny") != std::wstring::npos;
        snip.DemoShot(tall   ? 600
                      : tiny ? 90
                             : 1920,
                      tall   ? 1800
                      : tiny ? 40
                             : 1080,
                      what.find(L"-saved") != std::wstring::npos);
        if (what.find(L"-recent") != std::wstring::npos) {  // лента: ещё три снимка поверх показанного
            snip.DemoShot(800, 600, false);
            snip.DemoShot(600, 1800, false);
            snip.DemoShot(1920, 1080, false);
        }
        shown = &snip;
    } else if (what == L"pick") {
        pick.Create(inst);
        std::vector<BoostPick::Row> rows = {{L"Google Chrome", L"chrome.exe", 1840, 0},
                                            {L"Discord", L"discord.exe", 620, 1},
                                            {L"Telegram", L"telegram.exe", 310, 2}};
        pick.Show(L"Освободить память для Minecraft?", L"Выберите, что сделать с каждой программой.", rows, theme);
        if (more) pick.ExpandRow(1);
        shown = &pick;
    } else if (what.rfind(L"fail", 0) == 0) {
        card.Create(inst);
        card.yesLabel = L"Найти ошибку";
        card.noLabel = L"Не надо";
        card.onYes = [] {};
        card.kind = L"-";
        card.Show(BoostCard::OFFER, L"Сборка не удалась: RadialMenu",
                  L"cmake --build check. Соберу упавшее ещё раз в фоне и покажу первую ошибку.", theme, 60000,
                  Toast::G_FAIL);
        shown = &card;
    } else if (what.rfind(L"mem", 0) == 0) {
        card.Create(inst);
        card.yesLabel = L"Что закрыть";
        card.noLabel = L"Подкачка";
        card.onYes = [] {};
        card.kind = L"-";
        card.meter = 25.6f / 27.6f;
        card.more = L"Подкачка сейчас 12,0 ГБ. " + explain::Action(L"pagefile");
        card.Show(BoostCard::OFFER, L"Память на пределе",
                  L"Занято 25,6 ГБ из 27,6 ГБ, вместе с подкачкой. Дальше программы начнут падать, Unreal - при сборке "
                  L"шейдеров.",
                  theme, 60000, Toast::G_WARN);
        if (more) card.ExpandMore();
        shown = &card;
    } else {
        card.Create(inst);
        card.yesLabel = L"Закрыть";
        card.noLabel = L"Не надо";
        card.onYes = [] {};
        card.more = explain::Action(L"app:0", L"discord.exe");
        card.kind = L"-";
        card.Show(BoostCard::OFFER, L"Закрыть Discord?",
                  L"3 процесса, 620 МБ. Сначала попрошу закрыться (окна сохранят своё), через 5 с - силой.", theme,
                  60000);
        if (more) card.ExpandMore();
        shown = &card;
    }
    // snip-...@мс: кадр в этот момент от начала, чтобы посмотреть движение
    size_t atPos = what.find(L'@');
    int atMs = atPos == std::wstring::npos ? 1600 : _wtoi(what.c_str() + atPos + 1);
    if (what.find(L"-busy") != std::wstring::npos) snip.DemoBusy();
    // -save и -close: карточка сперва приезжает (секунда), @мс считается уже от нажатия
    bool actSave = what.find(L"-save-") != std::wstring::npos, actClose = what.find(L"-close") != std::wstring::npos;
    ULONGLONG started = GetTickCount64();
    bool acted = !actSave && !actClose;
    ULONGLONG until = started + atMs + (acted ? 0 : 1000);
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    while (GetTickCount64() < until) {
        if (!acted && GetTickCount64() - started >= 1000) {
            acted = true;
            if (actSave) snip.DemoSave(std::wstring(tempDir) + L"radial-snap-test");
            if (actClose) snip.DemoClose();
        }
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        shown->Tick();
        Sleep(16);
    }
    // кадр самой карточки: рабочий стол вокруг в снимок не попадает
    int code = shown->SaveFrame(out) ? 0 : 1;
    shown->Hide();
    TerminateProcess(GetCurrentProcess(), code);  // окна карточек и кэши GDI+ разбирать незачем
    return code;
}

// Настоящая карточка снимка на выдуманных снимках: для проверки щелчков, разметки и ленты. secs - сколько жить
int SnipTry(int secs) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    OleInitialize(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    static Config cfg = LoadOwnConfig();
    static SnipCard snip;
    snip.Create(inst);
    snip.Apply(cfg);
    snip.DemoShot(800, 600, false);
    snip.DemoShot(1920, 1080, false);
    snip.DemoShot(1000, 500, false);
    ULONGLONG until = GetTickCount64() + secs * 1000ull;
    while (GetTickCount64() < until) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        snip.Tick();
        Sleep(16);
    }
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

namespace {

void PumpCard(ULONGLONG ms, const std::function<bool()>& until) {
    const ULONGLONG stop = GetTickCount64() + ms;
    while (GetTickCount64() < stop && !until()) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        translatecard::Tick();
        Sleep(16);
    }
}

}  // namespace

int TranslateTry(const std::wstring& frame, const std::wstring& out, const std::wstring& text, bool fake, bool google) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    OleInitialize(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    static Config cfg = LoadOwnConfig();
    cfg.translateGoogle = google;  // только для этого запуска: настройки не пишутся
    snipimg::SetTestOutput(out);
    translate::SetFake(fake);
    translatecard::Create(inst);
    translatecard::Apply(cfg);
    const ULONGLONG started = GetTickCount64();
    translatecard::Ask(text, [](std::wstring) {});
    PumpCard(200000, [] { return !translatecard::DemoBusy(); });
    const ULONGLONG took = GetTickCount64() - started;
    PumpCard(1600, [] { return false; });  // доехать и дорисоваться
    bool ok = translatecard::DemoFrame(frame);
    WriteOut(L"translate_out.txt", std::to_wstring(took) + L" ms");
    translatecard::Destroy();
    return ok ? 0 : 1;
}

int ClipOcrTry(const std::wstring& image, const std::wstring& frame, const std::wstring& out) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    OleInitialize(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // оба переключателя выключены: окно истории видит буфер, но ничего не запоминает и не стирает
    static Config cfg = LoadOwnConfig();
    cfg.clipHistory = false;
    cfg.clipSecrets = false;
    snipimg::SetTestOutput(out);
    translate::SetFake(true);
    translatecard::Create(inst);
    translatecard::Apply(cfg);
    cliphist::Start(inst, &cfg);
    Gdiplus::Bitmap src(image.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return 2;
    const int w = (int)src.GetWidth(), h = (int)src.GetHeight();
    std::vector<BYTE> dib(sizeof(BITMAPINFOHEADER) + (size_t)w * h * 4);
    auto* head = (BITMAPINFOHEADER*)dib.data();
    head->biSize = sizeof(*head);
    head->biWidth = w;
    head->biHeight = -h;
    head->biPlanes = 1;
    head->biBitCount = 32;
    head->biCompression = BI_RGB;
    Gdiplus::Rect all(0, 0, w, h);
    Gdiplus::BitmapData d{};
    src.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &d);
    for (int y = 0; y < h; ++y) memcpy(dib.data() + sizeof(*head) + (size_t)y * w * 4, (BYTE*)d.Scan0 + (size_t)y * d.Stride, (size_t)w * 4);
    src.UnlockBits(&d);
    cliphist::DemoAddImage(dib);
    std::wstring names;
    std::function<void(const std::vector<Item>&, const std::wstring&)> dump = [&](const std::vector<Item>& items, const std::wstring& pad) {
        for (const Item& it : items) {
            names += pad + it.name + L" [" + it.path + L"]\r\n";
            dump(it.children, pad + L"  ");
        }
    };
    dump(cliphist::Ring(), L"");
    WriteOut(L"clipocr_out.txt", names);
    cliphist::ReadImage(0);
    PumpCard(9000, [&] {
        DWORD a = GetFileAttributesW((out + L".txt").c_str());
        return a != INVALID_FILE_ATTRIBUTES;
    });
    PumpCard(1600, [] { return false; });
    bool ok = translatecard::DemoFrame(frame);
    cliphist::Stop();
    translatecard::Destroy();
    return ok ? 0 : 1;
}

int SnapSuite(const std::wstring& refDir, const std::wstring& outDir, bool update) {
    Gdip gdip;
    SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr);
    if (update) SHCreateDirectoryExW(nullptr, refDir.c_str(), nullptr);
    std::wstring report;
    int failed = 0;
    for (const SnapCase& c : kSnapCases) {
        std::wstring now = outDir + L"\\" + c.name + L".png", ref = refDir + L"\\" + c.name + L".png";
        if (!SnapRun(c, now)) {
            report += std::wstring(c.name) + L": снимок не получился\r\n";
            ++failed;
            continue;
        }
        if (update) {
            CopyFileW(now.c_str(), ref.c_str(), FALSE);
            report += std::wstring(c.name) + L": эталон обновлён\r\n";
            continue;
        }
        double d = SnapDiff(ref, now, outDir + L"\\" + c.name + L"-diff.png");
        wchar_t line[160];
        if (d < 0)
            swprintf(line, 160, L"%ls: нет эталона или другой размер\r\n", c.name);
        else
            swprintf(line, 160, L"%ls: отличается %.2f%%%ls\r\n", c.name, d * 100, d > 0.005 ? L" - НЕ СОВПАЛО" : L"");
        report += line;
        failed += d < 0 || d > 0.005;
    }
    report += failed ? L"Не совпало: " + std::to_wstring(failed) + L"\r\n" : L"Все снимки совпали\r\n";
    WriteOut(L"snaps_out.txt", report);
    return failed ? 1 : 0;
}

// --ring-bench [мс]: кольцо левого края рисуется в скрытом окне, курсор водится по секторам; время по частям -
// в profile.log, итог - в bench_out.txt. Для сравнения до и после оптимизаций отрисовки.
int RingBench(int ms) {
    SetEnvironmentVariableW(L"RADIAL_PROFILE", L"1");
    OleInitialize(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int code = 1;
    {
        Gdip gdip;
        Config cfg = LoadOwnConfig();
        RadialMenu menu;
        if (menu.Create(GetModuleHandleW(nullptr))) {
            UINT dpi = GetDpiForSystem();
            while (!menu.Prewarm(&cfg, dpi, 1000)) {}
            ULONGLONG t0 = GetTickCount64();
            int frames = menu.Bench(&cfg, dpi, ms);
            WriteOut(L"bench_out.txt", L"кадров " + std::to_wstring(frames) + L" за " +
                                           std::to_wstring(GetTickCount64() - t0) + L" мс, расхождение кэша колец: " +
                                           std::to_wstring(menu.BenchDiff()) + L" пикс.\r\n");
            code = frames > 0 ? 0 : 1;
        }
        perf::Flush();
        // итог записан; окно с целью перетаскивания и кэши значков GDI+ разбирать незачем - замер окончен
        TerminateProcess(GetCurrentProcess(), (UINT)code);
    }
    return code;
}

}  // namespace devcli
