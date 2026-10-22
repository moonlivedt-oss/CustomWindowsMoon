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
#include "gfx/scene.h"
#include "menu/menu.h"
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
