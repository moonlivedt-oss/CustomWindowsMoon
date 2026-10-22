// Недельный отчёт картинкой (weekreport.h): сбор из того, что программа уже знает, и рисование на GDI+ в цветах
// студии.
#include "app/weekreport.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cwchar>

#include "app/toolsui.h"
#include "core/crash.h"
#include "core/themes.h"
#include "gfx/color.h"
#include "net/net.h"
#include "net/speed.h"
#include "tools/tools.h"

using namespace Gdiplus;

namespace weekreport {

namespace {

constexpr LONGLONG kWeek = 7 * 24 * 3600;

LONGLONG UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (LONGLONG)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ull - 11644473600ull);
}

SYSTEMTIME LocalAt(LONGLONG unix) {
    ULONGLONG t = (ULONGLONG)(unix + 11644473600ll) * 10000000ull;
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)}, loc;
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &loc);
    FileTimeToSystemTime(&loc, &st);
    return st;
}

std::wstring DayMonth(const SYSTEMTIME& st) {
    wchar_t b[16];
    swprintf(b, 16, L"%02u.%02u", st.wDay, st.wMonth);
    return b;
}

std::wstring Dir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\reports", p, MAX_PATH);
    return p;
}

std::wstring Mbps(double v) {
    wchar_t b[32];
    swprintf(b, 32, v >= 100 ? L"%.0f" : L"%.1f", v);
    return b;
}

// числа прошлого отчёта - последняя строка numbers.tsv не моложе 5 дней (иначе сравнивать неделю с ней же)
bool PreviousNumbers(LONGLONG now, Data& prev) {
    HANDLE f = CreateFileW((Dir() + L"\\numbers.tsv").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string raw(256 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    raw.resize(got);
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), text.data(), n);
    bool found = false;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find(L'\n', a);
        std::wstring line = text.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
        a = e == std::wstring::npos ? text.size() : e + 1;
        Data d;
        if (ParseNumbers(line, d) && now - d.when >= 5 * 24 * 3600) prev = d, found = true;
    }
    return found;
}

void AppendNumbers(const Data& d) {
    CreateDirectoryW(Dir().c_str(), nullptr);
    std::wstring line = NumbersRow(d) + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string raw(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), raw.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW((Dir() + L"\\numbers.tsv").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, raw.data(), (DWORD)raw.size(), &put, nullptr);
    CloseHandle(f);
}

const CLSID kPng = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

}  // namespace

Data Collect() {
    Data d;
    d.when = UnixNow();
    SYSTEMTIME now = LocalAt(d.when), from = LocalAt(d.when - kWeek);
    d.from = DayMonth(from);
    d.to = DayMonth(now);
    for (const tools::Volume& v : tools::VolumesCached(0))
        if (v.type == DRIVE_FIXED && v.total) d.disks.push_back({v.root, v.free, v.total});
    toolsui::State s = toolsui::Snapshot();
    for (const tools::Growth& g : s.growth)
        if (g.delta > 0 && d.growth.size() < 4) d.growth.push_back({g.name, g.delta});
    for (const tools::WeekChange& c : s.week.changes) {
        (c.what == tools::WeekChange::ADDED ? d.added : c.what == tools::WeekChange::REMOVED ? d.removed : d.updated)++;
        if (c.what == tools::WeekChange::ADDED && d.addedNames.size() < 3) d.addedNames.push_back(c.entry.label);
    }
    // обрывы: строки журнала сети «дд.мм чч:мм», год - текущий (на стыке лет - прошлый)
    for (const auto& [when, text] : net::History(400)) {
        if (text.rfind(L"Пропал интернет", 0) != 0 || when.size() < 5) continue;
        SYSTEMTIME st = now;
        st.wDay = (WORD)_wtoi(when.substr(0, 2).c_str());
        st.wMonth = (WORD)_wtoi(when.substr(3, 2).c_str());
        if (st.wMonth > now.wMonth) --st.wYear;
        FILETIME a, b;
        SystemTimeToFileTime(&st, &a);
        SystemTimeToFileTime(&now, &b);
        ULONGLONG ta = ((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime,
                  tb = ((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime;
        if (tb >= ta && (tb - ta) / 10000000ull < (ULONGLONG)kWeek) ++d.drops;
    }
    // скорость за неделю: медианы своего вида только по замерам этой недели
    std::vector<net::Speed> week;
    ULONGLONG since = (ULONGLONG)(d.when - kWeek + 11644473600ll) * 10000000ull;
    for (const net::Speed& sp : net::SpeedHistory())
        if (sp.when >= since) week.push_back(sp);
    d.tunnelDown = net::SpeedMedianOf(week, true).down;
    d.directDown = net::SpeedMedianOf(week, false).down;
    std::vector<net::SpeedExit> exits = net::SpeedByExit(week);
    if (exits.size() > 1) d.bestExit = exits[0].country;
    // падения: crash-ГГГГММДД-... за 7 дней
    wchar_t edge[16];
    swprintf(edge, 16, L"%04u%02u%02u", from.wYear, from.wMonth, from.wDay);
    for (const crash::Report& r : crash::All(crash::Dir())) {
        size_t at = r.dump.rfind(L"crash-");
        if (at != std::wstring::npos && r.dump.compare(at + 6, 8, edge) >= 0) ++d.crashes;
    }
    tools::BatteryInfo bat = tools::Battery();
    if (bat.present && bat.designed) d.batteryWear = (int)(100 - bat.full * 100 / bat.designed);
    return d;
}

Data DemoData(bool previous) {
    const ULONGLONG GB = 1024ull * 1024 * 1024;
    Data d;
    d.when = previous ? 1792000000 : 1792604800;
    d.from = previous ? L"09.10" : L"16.10";
    d.to = previous ? L"16.10" : L"23.10";
    d.disks = {{L"C:\\", (previous ? 152 : 140) * GB, 475 * GB}, {L"D:\\", 211 * GB, 476 * GB}};
    d.growth = {{L"Кэш Unreal Engine", (LONGLONG)(GB * 8 / 10)},
                {L"Временные файлы", (LONGLONG)(GB / 2)},
                {L"Загрузки", (LONGLONG)(GB / 7)}};
    d.added = 4, d.removed = 2, d.updated = 3;
    d.addedNames = {L"Blender 4.5", L"Discord", L"GoogleUpdaterTaskUser"};
    d.drops = previous ? 1 : 3;
    d.tunnelDown = previous ? 38 : 46, d.directDown = 105;
    d.bestExit = L"Швеция";
    d.crashes = 0;
    d.batteryWear = 11;
    return d;
}

bool Render(const Data& d, const Data* prev, const UiColors& c, const std::wstring& png) {
    const int W = 1200, H = 760;
    Bitmap bmp(W, H, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(c.win);
    FontFamily fam(L"Segoe UI");
    auto text = [&](const std::wstring& s, REAL x, REAL y, REAL w, REAL size, Color col, bool bold = false) {
        Font f(&fam, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        SolidBrush b(col);
        StringFormat sf;
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(s.c_str(), -1, &f, RectF(x, y, w, size * 1.5f), &sf, &b);
    };
    auto card = [&](REAL x, REAL y, REAL w, REAL h, const wchar_t* title) {
        GraphicsPath p;
        REAL r = 14;
        p.AddArc(x, y, r * 2, r * 2, 180, 90);
        p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
        p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
        p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
        p.CloseFigure();
        SolidBrush fill(c.card);
        g.FillPath(&fill, &p);
        Pen edge(c.border, 1);
        g.DrawPath(&edge, &p);
        text(title, x + 20, y + 14, w - 40, 17, c.text, true);
    };
    text(L"Неделя " + d.from + L" - " + d.to, 40, 26, 700, 30, c.text, true);
    text(L"Radial Menu: место, сеть и система за 7 дней" +
             (prev && prev->when ? L", разница - к отчёту за " + prev->to : std::wstring()),
         42, 72, 1000, 15, c.muted);

    const REAL cw = (W - 40 * 2 - 20) / 2.0f, top = 112;
    // место
    card(40, top, cw, 190, L"Место");
    REAL y = top + 52;
    for (size_t i = 0; i < d.disks.size() && i < 3; ++i, y += 42) {
        const Disk& k = d.disks[i];
        double used = k.total ? 1.0 - (double)k.free / k.total : 0;
        text(k.root.substr(0, 2) + L"  свободно " + tools::Bytes(k.free) + L" из " + tools::Bytes(k.total), 60, y,
             cw - 40, 14, c.text);
        SolidBrush back(c.field), bar(used > 0.9 ? c.warn : c.accent);
        g.FillRectangle(&back, 60.0f, y + 24, cw - 40, 8.0f);
        g.FillRectangle(&bar, 60.0f, y + 24, (REAL)((cw - 40) * used), 8.0f);
    }
    // рост
    card(40 + cw + 20, top, cw, 190, L"Выросло сильнее всего");
    y = top + 52;
    if (d.growth.empty()) text(L"Замеры ещё копятся", 40 + cw + 40, y, cw - 40, 14, c.muted);
    LONGLONG maxG = d.growth.empty() ? 1 : std::max<LONGLONG>(1, d.growth[0].second);
    for (size_t i = 0; i < d.growth.size(); ++i, y += 32) {
        REAL x0 = 40 + cw + 40;
        text(d.growth[i].first, x0, y, cw * 0.42f, 14, c.text);
        SolidBrush bar(i == 0 ? c.accent : WithAlpha(c.accent, 170));
        g.FillRectangle(&bar, x0 + cw * 0.44f, y + 7, (REAL)(cw * 0.32f * d.growth[i].second / maxG), 10.0f);
        text(L"+" + tools::Bytes((ULONGLONG)d.growth[i].second), x0 + cw * 0.78f, y, cw * 0.2f, 14, c.muted, true);
    }
    // что изменилось
    REAL top2 = top + 210;
    card(40, top2, cw, 190, L"Поставилось и пропало");
    text(L"появилось " + std::to_wstring(d.added) + L",  убрано " + std::to_wstring(d.removed) + L",  обновлено " +
             std::to_wstring(d.updated),
         60, top2 + 52, cw - 40, 15, c.text, true);
    y = top2 + 84;
    for (const std::wstring& n : d.addedNames) {
        text(L"+ " + n, 60, y, cw - 40, 14, c.muted);
        y += 26;
    }
    // сеть
    card(40 + cw + 20, top2, cw, 190, L"Сеть");
    REAL x1 = 40 + cw + 40;
    text(d.drops ? L"обрывов интернета: " + std::to_wstring(d.drops) : std::wstring(L"обрывов не было"), x1, top2 + 52,
         cw - 40, 15, d.drops ? c.gold : c.text, true);
    text(L"через туннель " + (d.tunnelDown > 0 ? L"↓ " + Mbps(d.tunnelDown) + L" Мбит/с" : std::wstring(L"не мерили")),
         x1, top2 + 86, cw - 40, 14, c.text);
    text(L"напрямую " + (d.directDown > 0 ? L"↓ " + Mbps(d.directDown) + L" Мбит/с" : std::wstring(L"не мерили")), x1,
         top2 + 112, cw - 40, 14, c.text);
    if (!d.bestExit.empty()) text(L"быстрее всего выход: " + d.bestExit, x1, top2 + 138, cw - 40, 14, c.muted);
    // программа и разница
    REAL top3 = top2 + 210;
    card(40, top3, cw, 170, L"Программа и батарея");
    text(d.crashes ? L"падений: " + std::to_wstring(d.crashes) : std::wstring(L"ни одного падения"), 60, top3 + 52,
         cw - 40, 15, d.crashes ? c.warn : c.text, true);
    text(d.batteryWear >= 0 ? L"износ батареи " + std::to_wstring(d.batteryWear) + L"%" : std::wstring(L"батареи нет"),
         60, top3 + 86, cw - 40, 14, c.text);
    card(40 + cw + 20, top3, cw, 170, L"К прошлой неделе");
    std::vector<std::wstring> deltas = prev ? Deltas(d, *prev) : std::vector<std::wstring>();
    y = top3 + 52;
    if (deltas.empty())
        text(prev && prev->when ? L"всё примерно так же" : L"сравнение - со следующего отчёта", x1, y, cw - 40, 14,
             c.muted);
    for (size_t i = 0; i < deltas.size() && i < 4; ++i, y += 26) text(deltas[i], x1, y, cw - 40, 14, c.text);
    return bmp.Save(png.c_str(), &kPng, nullptr) == Ok;
}

std::wstring Make(const UiColors& c, std::vector<std::wstring>* deltas) {
    Data now = Collect(), prev;
    bool had = PreviousNumbers(now.when, prev);
    CreateDirectoryW(Dir().c_str(), nullptr);
    SYSTEMTIME st = LocalAt(now.when);
    wchar_t name[48];
    swprintf(name, 48, L"\\week-%04u-%02u-%02u.png", st.wYear, st.wMonth, st.wDay);
    std::wstring path = Dir() + name;
    if (!Render(now, had ? &prev : nullptr, c, path)) return L"";
    if (deltas) *deltas = had ? Deltas(now, prev) : std::vector<std::wstring>();
    AppendNumbers(now);
    return path;
}

}  // namespace weekreport
