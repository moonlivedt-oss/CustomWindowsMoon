// Конвертер броском: кольцо вариантов, работа в фоне (WIC, ffmpeg, pypdf), карточка хода и итога.
#include <objbase.h>
#include <shlobj.h>
#include <wincodec.h>

#include <atomic>
#include <memory>

#include "app/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "life/convert.h"
#include "menu/menu.h"
#include "tools/tools.h"

namespace convert {

namespace {

constexpr UINT WM_CONV = WM_APP + 1;

const Config* g_cfg = nullptr;
RadialMenu* g_menu = nullptr;
HWND g_wnd = nullptr;
BoostCard g_card;
std::vector<std::wstring> g_files, g_pending;
ULONGLONG g_showAt = 0;
std::atomic<bool> g_busy{false};

struct Done {
    bool ok = false;
    std::wstring title, sub, show;  // show - что выделить в Проводнике
};

Item Act(const std::wstring& name, const std::wstring& key, const std::wstring& note) {
    Item it;
    it.name = name;
    it.special = 3;
    it.path = key;
    it.note = note;
    it.iconFull = ExeDir() + L"\\icons\\tools-box.png";
    if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\more.png";
    return it;
}

std::wstring Unique(const std::wstring& p) {
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    for (int i = 2; i < 1000; ++i) {
        std::wstring q = OutName(p, p.substr(p.find_last_of(L'.')), std::to_wstring(i));
        if (GetFileAttributesW(q.c_str()) == INVALID_FILE_ATTRIBUTES) return q;
    }
    return p;
}

long long SizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

std::wstring Ffmpeg(const wchar_t* exe) {  // ffmpeg.exe / ffprobe.exe: PATH, потом пакет winget
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr)) return buf;
    wchar_t local[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Packages", local, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(local) + L"\\*FFmpeg*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        std::wstring pkg = std::wstring(local) + L"\\" + fd.cFileName;
        WIN32_FIND_DATAW sub;
        HANDLE h2 = FindFirstFileW((pkg + L"\\*").c_str(), &sub);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring p = pkg + L"\\" + sub.cFileName + L"\\bin\\" + exe;
            if (sub.cFileName[0] != L'.' && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) found = p;
        } while (found.empty() && FindNextFileW(h2, &sub));
        FindClose(h2);
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

double Duration(const std::wstring& in) {
    std::wstring probe = Ffmpeg(L"ffprobe.exe"), out;
    if (probe.empty()) return 0;
    tools::RunCapture(
        L"\"" + probe + L"\" -v error -show_entries format=duration -of default=nw=1:nk=1 \"" + in + L"\"", out, 30000);
    return _wtof(Trim(out).c_str());
}

// Картинка через WIC: в PNG или JPEG (quality 0..1, scale - доля размера)
bool WicSave(const std::wstring& in, const std::wstring& out, bool jpeg, float quality, double scale) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fe = nullptr;
    IPropertyBag2* props = nullptr;
    do {
        if (FAILED(
                f->CreateDecoderFromFilename(in.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
            break;
        if (FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv))) break;
        WICPixelFormatGUID fmt = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
        if (FAILED(conv->Initialize(frame, fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            break;
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        IWICBitmapSource* src = conv;
        if (scale < 0.999) {
            UINT sw = std::max(1u, (UINT)(w * scale)), sh = std::max(1u, (UINT)(h * scale));
            if (FAILED(f->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(conv, sw, sh, WICBitmapInterpolationModeFant)))
                break;
            src = scaler;
            w = sw, h = sh;
        }
        if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE)))
            break;
        if (FAILED(f->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) ||
            FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&fe, &props)))
            break;
        if (jpeg && props) {
            PROPBAG2 opt{};
            opt.pstrName = (LPOLESTR)L"ImageQuality";
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = quality;
            props->Write(1, &opt, &v);
        }
        if (FAILED(fe->Initialize(props)) || FAILED(fe->SetSize(w, h))) break;
        WICPixelFormatGUID pf = fmt;
        fe->SetPixelFormat(&pf);
        ok = SUCCEEDED(fe->WriteSource(src, nullptr)) && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
    } while (false);
    for (IUnknown* u : std::initializer_list<IUnknown*>{props, fe, enc, stream, scaler, conv, frame, dec, f})
        if (u) u->Release();
    if (!ok) DeleteFileW(out.c_str());
    return ok;
}

bool FfmpegRun(const std::wstring& op, const std::wstring& in, const std::wstring& out, int kbps, std::wstring& why) {
    std::wstring exe = Ffmpeg(L"ffmpeg.exe");
    if (exe.empty()) {
        why = L"нет ffmpeg (winget install Gyan.FFmpeg)";
        return false;
    }
    std::wstring log;
    int code = tools::RunCapture(L"\"" + exe + L"\" " + FfmpegArgs(op, in, out, kbps), log, 60 * 60000);
    if (code != 0) {
        DeleteFileW(out.c_str());
        size_t e = log.find_last_not_of(L"\r\n");
        size_t b = log.find_last_of(L'\n', e);
        why = L"ffmpeg: " + Trim(log.substr(b == std::wstring::npos ? 0 : b + 1));
        return false;
    }
    return true;
}

Done One(const std::wstring& key, const std::wstring& in) {
    Done d;
    std::wstring name = in.substr(in.find_last_of(L'\\') + 1), why;
    if (key == L"png" || key == L"jpg") {
        std::wstring out = Unique(OutName(in, key == L"png" ? L".png" : L".jpg", L""));
        d.ok = WicSave(in, out, key == L"jpg", 0.9f, 1.0);
        if (!d.ok) d.ok = FfmpegRun(L"img", in, out, 0, why);  // кодека WIC нет (avif, heic без расширения)
        d.show = out;
        if (!d.ok && why.empty()) why = L"Windows не умеет читать этот формат";
    } else if (key == L"img8") {
        std::wstring out = Unique(OutName(in, L".jpg", L"8 МБ"));
        const long long target = 8ll << 20;
        float q = 0.9f;
        double scale = 1.0;
        for (int i = 0; i < 10 && !d.ok; ++i) {
            if (!WicSave(in, out, true, q, scale)) break;
            if (SizeOf(out) <= target)
                d.ok = true;
            else if (q > 0.6f)
                q -= 0.15f;
            else
                scale *= 0.75;
        }
        d.show = out;
        if (!d.ok) why = L"не вышло ужать";
    } else if (key == L"mp4" || key == L"gif") {
        std::wstring out = Unique(OutName(in, key == L"gif" ? L".gif" : L".mp4", L""));
        if (EndsWithI(in, L".mp4") && key == L"mp4") out = Unique(OutName(in, L".mp4", L"mp4"));
        d.ok = FfmpegRun(key, in, out, 0, why);
        d.show = out;
    } else if (key == L"v8" || key == L"v25") {
        long long target = (key == L"v8" ? 8ll : 25ll) << 20;
        double sec = Duration(in);
        int kbps = VideoKbps(target, sec);
        std::wstring out = Unique(OutName(in, L".mp4", key == L"v8" ? L"8 МБ" : L"25 МБ"));
        if (kbps <= 0) {
            why = L"не узнать длительность (ffprobe)";
        } else {
            d.ok = FfmpegRun(L"fit", in, out, kbps, why);
            if (d.ok && SizeOf(out) > target)  // промахнулись - ещё раз потише
                d.ok = FfmpegRun(L"fit", in, out, kbps * 4 / 5, why) && SizeOf(out) <= target;
        }
        d.show = out;
    }
    d.title = name;
    d.sub =
        d.ok ? d.show.substr(d.show.find_last_of(L'\\') + 1) + L", " + tools::Bytes((ULONGLONG)SizeOf(d.show)) : why;
    return d;
}

Done MergePdf(const std::vector<std::wstring>& pdfs) {
    Done d;
    wchar_t py[MAX_PATH];
    if (!SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, py, nullptr)) {
        d.sub = L"нужен Python с pypdf (py -3 -m pip install pypdf)";
        return d;
    }
    std::wstring out = Unique(OutName(pdfs[0], L".pdf", L"вместе"));
    std::wstring cmd = std::wstring(L"\"") + py +
                       L"\" -3 -I -c \"import sys,pypdf;w=pypdf.PdfWriter();[w.append(f) for f in sys.argv[2:]];"
                       L"w.write(sys.argv[1]);print('ok')\" \"" +
                       out + L"\"";
    for (const std::wstring& p : pdfs) cmd += L" \"" + p + L"\"";
    std::wstring log;
    int code = tools::RunCapture(cmd, log, 5 * 60000);
    d.ok = code == 0 && log.find(L"ok") != std::wstring::npos;
    d.show = out;
    d.sub = d.ok ? out.substr(out.find_last_of(L'\\') + 1) + L", " + tools::Bytes((ULONGLONG)SizeOf(out))
                 : (log.find(L"pypdf") != std::wstring::npos ? L"нужен pypdf (py -3 -m pip install pypdf)"
                                                             : L"не склеилось: " + Trim(log).substr(0, 120));
    if (!d.ok) DeleteFileW(out.c_str());
    return d;
}

void Run(const std::wstring& key) {
    if (g_busy.exchange(true)) return;
    std::vector<std::wstring> files = g_files;
    g_card.kind = L"-";
    g_card.Show(BoostCard::WORKING, L"Превращаю...", Plural((int)files.size(), L"файл", L"файла", L"файлов"),
                CurrentTheme(*g_cfg));
    HWND w = g_wnd;
    bg::Run([key, files, w] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<Done> res;
        if (key == L"pdf") {
            std::vector<std::wstring> pdfs;
            for (const std::wstring& f : files)
                if (KindOf(f) == PDF) pdfs.push_back(f);
            res.push_back(MergePdf(pdfs));
        } else {
            for (const std::wstring& f : files) {
                Kind k = KindOf(f);
                bool img = key == L"png" || key == L"jpg" || key == L"img8";
                if ((img && k == IMAGE) || (!img && k == VIDEO)) res.push_back(One(key, f));
            }
        }
        CoUninitialize();
        auto* d = new Done;
        int ok = 0;
        for (const Done& r : res) ok += r.ok;
        d->ok = ok > 0;
        if (res.size() == 1) {
            *d = res[0];
            d->title = (res[0].ok ? L"Готово: " : L"Не вышло: ") + res[0].title;
        } else {
            d->title = L"Готово " + std::to_wstring(ok) + L" из " + std::to_wstring(res.size());
            for (const Done& r : res)
                if (r.ok) d->show = r.show;
            for (const Done& r : res)
                if (!r.ok) d->sub = r.title + L": " + r.sub;
        }
        if (!PostMessageW(w, WM_CONV, 0, (LPARAM)d)) delete d;
    });
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_CONV) return DefWindowProcW(h, msg, wp, lp);
    std::unique_ptr<Done> d((Done*)lp);
    g_busy = false;
    autos::Note(L"Превратить: " + d->title + (d->sub.empty() ? L"" : L" - " + d->sub));
    std::wstring show = d->show;
    g_card.kind = L"-";
    if (d->ok) {
        g_card.yesLabel = L"Показать";
        g_card.noLabel = L"Закрыть";
        g_card.onYes = [show] {
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + show + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
        };
        g_card.onNo = nullptr;
        g_card.Show(BoostCard::OFFER, d->title, d->sub, CurrentTheme(*g_cfg), 12000, Toast::G_OK);
    } else {
        g_card.Show(BoostCard::DONE, d->title, d->sub, CurrentTheme(*g_cfg), 9000, Toast::G_FAIL);
    }
    return 0;
}

}  // namespace

std::vector<Item> RingItems(const std::vector<std::wstring>& files) {
    int img = 0, vid = 0, pdf = 0;
    for (const std::wstring& f : files) {
        Kind k = KindOf(f);
        img += k == IMAGE, vid += k == VIDEO, pdf += k == PDF;
    }
    std::vector<Item> r;
    if (img) {
        r.push_back(Act(L"В PNG", L"png", L"Без потерь, прозрачность сохранится"));
        r.push_back(Act(L"В JPG", L"jpg", L"Качество 90%, меньше места"));
        r.push_back(Act(L"Сжать до 8 МБ", L"img8", L"JPG, качество и размер ниже, пока не влезет (Discord)"));
    }
    if (vid) {
        r.push_back(Act(L"В MP4", L"mp4", L"H.264 + AAC - откроется везде"));
        r.push_back(Act(L"В GIF", L"gif", L"Первые 15 секунд, 480 px, 15 кадров в секунду"));
        r.push_back(Act(L"Видео до 8 МБ", L"v8", L"Под Discord без Nitro"));
        r.push_back(Act(L"Видео до 25 МБ", L"v25", L"Под Telegram и почту"));
    }
    if (pdf >= 2) r.push_back(Act(L"Склеить PDF", L"pdf", L"В один файл, в порядке броска"));
    return r;
}

void Init(HINSTANCE inst, const Config* cfg, RadialMenu* menu) {
    g_cfg = cfg;
    g_menu = menu;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MoonRadialConvert";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    g_card.Create(inst);
}

void Drop(const std::vector<std::wstring>& files) {
    g_pending = files;
    g_showAt = GetTickCount64() + 300;  // прежнее кольцо успевает погаснуть
}

void Tick() {
    g_card.Tick();
    if (!g_showAt || GetTickCount64() < g_showAt || !g_menu) return;
    if (g_menu->IsOpen()) return;
    g_showAt = 0;
    g_files = g_pending;
    std::vector<Item> ring = RingItems(g_files);
    if (ring.empty()) {
        g_card.kind = L"-";
        g_card.Show(BoostCard::DONE, L"Превратить нечего", L"Картинки, видео или PDF (два и больше)",
                    CurrentTheme(*g_cfg), 5000, Toast::G_INFO);
        return;
    }
    g_menu->onNet = [](const std::wstring& key) { Run(key); };
    std::wstring status =
        L"Превратить: " + (g_files.size() == 1 ? g_files[0].substr(g_files[0].find_last_of(L'\\') + 1)
                                               : Plural((int)g_files.size(), L"файл", L"файла", L"файлов"));
    g_menu->netStatus = [status] { return status; };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForSystem();
    g_menu->ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

void Shutdown() {
    g_card.Destroy();
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
}

}  // namespace convert

namespace convert {

std::wstring RunSync(const std::wstring& key, const std::vector<std::wstring>& files) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring o;
    if (key == L"pdf") {
        Done d = MergePdf(files);
        o = (d.ok ? L"ok " : L"FAIL ") + d.sub;
    } else {
        for (const std::wstring& f : files) {
            Done d = One(key, f);
            o += (d.ok ? L"ok " : L"FAIL ") + d.title + L" -> " + d.sub + L"\n";
        }
    }
    return o;
}

}  // namespace convert
