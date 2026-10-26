// «Телефон рядом», пульт: экран ПК кадрами JPEG (кадр уходит, когда картинка поменялась), касание в точку экрана,
// сочетания клавиш, окна (список, вперёд, свернуть, закрыть), операции с файлами в открытых папках ПК.
// Экран - только при phone_screen, файлы - только при phone_files; всё сделанное - в журнал.
#include <winsock2.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>

#include "core/text.h"
#include "phone/phone_internal.h"

namespace phone::detail {

std::atomic<ULONGLONG> g_screenAt{0};

namespace {

std::string Q(const Request& r, const char* k) {
    auto it = r.query.find(k);
    return it == r.query.end() ? std::string() : it->second;
}

std::vector<RECT> Monitors() {  // основной - первым
    std::vector<RECT> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(m, &mi);
            auto* v = (std::vector<RECT>*)lp;
            if (mi.dwFlags & MONITORINFOF_PRIMARY)
                v->insert(v->begin(), mi.rcMonitor);
            else
                v->push_back(mi.rcMonitor);
            return TRUE;
        },
        (LPARAM)&out);
    return out;
}

RECT MonitorOf(const Request& r) {
    std::vector<RECT> m = Monitors();
    size_t i = (size_t)atoi(Q(r, "m").c_str());
    return m.empty() ? RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}
                     : m[std::min(i, m.size() - 1)];
}

// снимок монитора: отпечаток картинки и JPEG шириной не больше maxW
std::string Frame(const RECT& mon, int maxW, int quality, uint32_t& hash) {
    int w = mon.right - mon.left, h = mon.bottom - mon.top;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::string out;
    if (bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, screen, mon.left, mon.top, SRCCOPY | CAPTUREBLT);
        CURSORINFO ci{sizeof(ci)};  // курсор - тоже на картинке, иначе не видно, куда попадёт касание
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
            DrawIconEx(mem, ci.ptScreenPos.x - mon.left, ci.ptScreenPos.y - mon.top, ci.hCursor, 0, 0, 0, nullptr,
                       DI_NORMAL | DI_DEFAULTSIZE);
        SelectObject(mem, old);
        const uint32_t* px = (const uint32_t*)bits;
        uint32_t fnv = 2166136261u;
        for (size_t i = 0, n = (size_t)w * h; i < n; i += 13) fnv = (fnv ^ px[i]) * 16777619u;
        hash = fnv ? fnv : 1;
        SIZE fs = FrameSize(w, h, maxW);
        Gdiplus::Bitmap src(w, h, w * 4, PixelFormat32bppRGB, (BYTE*)bits);
        Gdiplus::Bitmap dst(fs.cx, fs.cy, PixelFormat24bppRGB);
        {
            Gdiplus::Graphics g(&dst);
            g.SetInterpolationMode(fs.cx < w ? Gdiplus::InterpolationModeHighQualityBilinear
                                             : Gdiplus::InterpolationModeNearestNeighbor);
            g.DrawImage(&src, 0, 0, fs.cx, fs.cy);
        }
        CLSID jpg = {0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        Gdiplus::EncoderParameters ep{};
        ULONG q = (ULONG)std::clamp(quality, 20, 90);
        ep.Count = 1;
        ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
        ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Value = &q;
        IStream* st = nullptr;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) && dst.Save(st, &jpg, &ep) == Gdiplus::Ok) {
            HGLOBAL g = nullptr;
            GetHGlobalFromStream(st, &g);
            STATSTG ss{};
            st->Stat(&ss, STATFLAG_NONAME);
            if (void* p = GlobalLock(g)) {
                out.assign((const char*)p, (size_t)ss.cbSize.QuadPart);
                GlobalUnlock(g);
            }
        }
        if (st) st->Release();
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

void Screen(Conn& cn, const Request& r) {
    {
        Lock l;
        S().seen = GetTickCount64();
    }
    RECT mon = MonitorOf(r);
    int maxW = atoi(Q(r, "w").c_str()), quality = atoi(Q(r, "q").c_str());
    uint32_t last = (uint32_t)strtoul(Q(r, "h").c_str(), nullptr, 10), hash = 0;
    std::string jpg;
    // кадр уходит, когда картинка поменялась; не поменялась за 1,5 с - тот же кадр (телефон знает, что связь жива)
    for (int i = 0; i < 12 && !Stopping(); ++i) {
        g_screenAt = GetTickCount64();
        uint32_t h = 0;
        std::string f = Frame(mon, maxW ? maxW : 1280, quality ? quality : 60, h);
        if (h != last || i == 11) {
            jpg = std::move(f);
            hash = h;
            break;
        }
        Sleep(120);
    }
    Reply(cn, 200, "image/jpeg", jpg,
          "X-Frame: " + std::to_string(hash) + "\r\nX-Screen: " + std::to_string(mon.right - mon.left) + "x" +
              std::to_string(mon.bottom - mon.top) + "\r\n");
}

void Click(const std::string& b) {
    auto press = [](DWORD dn, DWORD up) {
        INPUT in[2]{};
        in[0].type = in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = dn;
        in[1].mi.dwFlags = up;
        SendInput(2, in, sizeof(INPUT));
    };
    if (b == "r")
        press(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
    else if (b != "n")
        for (int i = 0; i < (b == "d" ? 2 : 1); ++i) press(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
}

void Keys(const std::vector<WORD>& vk) {
    std::vector<INPUT> in;
    for (WORD k : vk) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = k;
        in.push_back(i);
    }
    for (auto it = vk.rbegin(); it != vk.rend(); ++it) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = *it;
        i.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

bool ShellOp(UINT func, const std::wstring& from, const std::wstring& to, bool recycle) {
    std::wstring f = from + L'\0', t = to + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = func;
    op.pFrom = f.c_str();
    op.pTo = to.empty() ? nullptr : t.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR | (recycle ? FOF_ALLOWUNDO : 0) |
                (func == FO_COPY || func == FO_MOVE ? FOF_RENAMEONCOLLISION : 0);
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

void Fs(Conn& cn, const Request& r) {
    Seen();
    if (!FilesAllowed()) return Text(cn, 403, "операции с файлами выключены (phone_files)");
    std::string op = Q(r, "op");
    if (!FsOp(op)) return Text(cn, 400, "не понял");
    std::wstring src = SharePathOf(r, "r", "p");
    if (src.empty()) return Text(cn, 403, "нет доступа");
    std::wstring name = src.substr(src.find_last_of(L'\\') + 1), what;
    bool ok = false;
    if (op == "open") {
        std::string out;
        ok = CallMain("open:" + Narrow(src), out);
        what = L"открыто на ПК: " + src;
    } else if (op == "delete") {
        if (ShareRootIs(src)) return Text(cn, 403, "открытую папку целиком не удалить");
        ok = ShellOp(FO_DELETE, src, L"", true);
        what = L"в Корзину: " + src;
    } else if (op == "rename") {
        std::wstring nn = SafeName(Widen(Q(r, "name")));
        if (ShareRootIs(src)) return Text(cn, 403, "открытую папку не переименовать");
        std::wstring to = src.substr(0, src.find_last_of(L'\\') + 1) + nn;
        ok = GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES && MoveFileExW(src.c_str(), to.c_str(), 0);
        what = L"переименовано: " + name + L" -> " + nn;
    } else if (op == "mkdir") {
        std::wstring nn = SafeName(Widen(Q(r, "name")));
        DWORD a = GetFileAttributesW(src.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return Text(cn, 400, "это не папка");
        ok = CreateDirectoryW((src + L"\\" + nn).c_str(), nullptr) != FALSE;
        what = L"новая папка: " + src + L"\\" + nn;
    } else {  // copy, move: в папку tr/tp
        std::wstring dst = SharePathOf(r, "tr", "tp");
        DWORD a = dst.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(dst.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return Text(cn, 403, "нет такой папки");
        if (op == "move" && ShareRootIs(src)) return Text(cn, 403, "открытую папку не переместить");
        std::wstring ls = Lower(src) + L"\\", ld = Lower(dst) + L"\\";
        if (ld.compare(0, ls.size(), ls) == 0) return Text(cn, 400, "в саму себя нельзя");
        ok = ShellOp(op == "copy" ? FO_COPY : FO_MOVE, src, dst, false);
        what = (op == "copy" ? L"скопировано: " : L"перемещено: ") + src + L" -> " + dst;
    }
    if (ok) Post(EV_NOTE, what);
    Text(cn, ok ? 200 : 500, ok ? "ok" : "не вышло (файл занят или нет прав?)");
}

}  // namespace

bool HandleRemote(Conn& cn, const Request& r, const std::string& rest) {
    bool get = r.method == "GET" || r.method == "HEAD", post = r.method == "POST";
    if (rest == "/screen" && get) {
        if (!ScreenAllowed()) {
            Text(cn, 403, "экран ПК для телефона выключен (phone_screen) или остановлен на ПК");
            return true;
        }
        Screen(cn, r);
        return true;
    }
    if (rest == "/mons" && get) {
        std::string j = "[";
        std::vector<RECT> m = Monitors();
        for (size_t i = 0; i < m.size(); ++i)
            j += std::string(i ? "," : "") + "{\"w\":" + std::to_string(m[i].right - m[i].left) +
                 ",\"h\":" + std::to_string(m[i].bottom - m[i].top) + "}";
        JsonReply(cn, j + "]");
        return true;
    }
    if (rest == "/tap" && post) {  // касание по картинке экрана: x, y - доли, b - l / r / d / n (только навести)
        if (!ScreenAllowed() || !InputAllowed()) {
            Text(cn, 403, "выключено");
            return true;
        }
        POINT p = TapPoint(atof(Q(r, "x").c_str()), atof(Q(r, "y").c_str()), MonitorOf(r));
        SetCursorPos(p.x, p.y);
        Click(Q(r, "b"));
        Text(cn, 200, "ok");
        return true;
    }
    if (rest == "/combo" && post) {
        std::vector<WORD> k = Combo(Q(r, "k"));
        if (!InputAllowed() || k.empty()) {
            Text(cn, k.empty() ? 400 : 403, "нет");
            return true;
        }
        Keys(k);
        Text(cn, 200, "ok");
        return true;
    }
    if (rest == "/wins" && get) {
        if (!InputAllowed()) {
            Text(cn, 403, "выключено");
            return true;
        }
        std::string out;
        CallMain("wins", out);
        JsonReply(cn, out.empty() ? "[]" : out);
        return true;
    }
    if (rest == "/win" && post) {  // op = front / min / close, h - окно из списка
        std::string op = Q(r, "op"), out;
        if (!InputAllowed() || (op != "front" && op != "min" && op != "close")) {
            Text(cn, 403, "нет");
            return true;
        }
        bool ok = CallMain("win:" + op + ":" + Q(r, "h"), out);
        Text(cn, ok ? 200 : 404, ok ? "ok" : "окна нет");
        return true;
    }
    if (rest == "/fs" && post) {
        Fs(cn, r);
        return true;
    }
    return false;
}

}  // namespace phone::detail
