#include "helpers/ocr.h"

#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <shlwapi.h>
#include <winstring.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "core/bg.h"
#include "helpers/snip.h"

// Интерфейсы Windows.Media.Ocr и Windows.Graphics.Imaging описаны вручную, как в media.cpp:
// заголовков WinRT в MinGW нет. Порядок методов - как в .idl из Windows SDK;
// IID фабрик сверены с GetIids настоящих фабрик.
namespace ocr_abi {

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

struct IAsyncOp : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void*) = 0;
};

struct IDecoderStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BmpDecoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_JpegDecoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PngDecoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TiffDecoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_GifDecoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_JpegXRDecoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IcoDecoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDecoderInformationEnumerator(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateAsync(IUnknown* stream, IAsyncOp**) = 0;
};

struct IFrameWithSoftwareBitmap : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetSoftwareBitmapAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSoftwareBitmapConvertedAsync(int format, int alpha, IAsyncOp**) = 0;
};

struct IOcrEngine : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RecognizeAsync(IInspectable* bitmap, IAsyncOp**) = 0;
};

struct IOcrEngineStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MaxImageDimension(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableRecognizerLanguages(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLanguageSupported(void*, boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromLanguage(void*, IOcrEngine**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromUserProfileLanguages(IOcrEngine**) = 0;
};

struct ILanguageFactory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateLanguage(HSTRING tag, IInspectable** language) = 0;
};

struct OcrRect {
    float x, y, w, h;
};

struct IOcrWord : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BoundingRect(OcrRect*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};

struct IWordList : IInspectable {  // IVectorView<OcrWord>
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IOcrWord**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};

struct IOcrLine : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Words(IWordList**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};

struct ILineList : IInspectable {  // IVectorView<OcrLine>
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IOcrLine**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};

struct IOcrResult : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Lines(ILineList**) = 0;
};

}  // namespace ocr_abi

using namespace ocr_abi;

namespace {

const IID IID_AsyncInfo = {0x00000036, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const IID IID_DecoderStatics = {0x438CCB26, 0xBCEF, 0x4E95, {0xBA, 0xD6, 0x23, 0xA8, 0x22, 0xE5, 0x8D, 0x01}};
const IID IID_FrameWithSoftwareBitmap = {0xFE287C9A, 0x420C, 0x4963, {0x87, 0xAD, 0x69, 0x14, 0x36, 0xE0, 0x83, 0x83}};
const IID IID_OcrStatics = {0x5BFFA85A, 0x3384, 0x3540, {0x99, 0x40, 0x69, 0x91, 0x20, 0xD4, 0x28, 0xA8}};
const IID IID_LanguageFactory = {0x9B0252AC, 0x0C27, 0x44F8, {0xB7, 0x92, 0x97, 0x93, 0xFB, 0x66, 0xC6, 0x3E}};
const IID IID_RandomAccessStream = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};

template <class T>
struct Ptr {
    T* p = nullptr;
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    ~Ptr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Функции WinRT - из combase.dll, как в media.cpp.
struct Api {
    HRESULT(WINAPI* roInit)(RO_INIT_TYPE) = nullptr;
    void(WINAPI* roUninit)() = nullptr;
    HRESULT(WINAPI* factory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* create)(PCNZWCH, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* del)(HSTRING) = nullptr;
    PCWSTR(WINAPI* raw)(HSTRING, UINT32*) = nullptr;
    HRESULT(WINAPI* overStream)(IUnknown*, int, REFIID, void**) = nullptr;
    bool ok = false;
    Api() {
        HMODULE cb = LoadLibraryW(L"combase.dll"), sh = LoadLibraryW(L"shcore.dll");
        if (!cb || !sh) return;
        roInit = (decltype(roInit))(void*)GetProcAddress(cb, "RoInitialize");
        roUninit = (decltype(roUninit))(void*)GetProcAddress(cb, "RoUninitialize");
        factory = (decltype(factory))(void*)GetProcAddress(cb, "RoGetActivationFactory");
        create = (decltype(create))(void*)GetProcAddress(cb, "WindowsCreateString");
        del = (decltype(del))(void*)GetProcAddress(cb, "WindowsDeleteString");
        raw = (decltype(raw))(void*)GetProcAddress(cb, "WindowsGetStringRawBuffer");
        overStream = (decltype(overStream))(void*)GetProcAddress(sh, "CreateRandomAccessStreamOverStream");
        ok = roInit && roUninit && factory && create && del && raw && overStream;
    }
};

const Api& A() {
    static Api api;
    return api;
}

bool Await(IAsyncOp* op, void* out, DWORD timeout = 8000) {
    if (!op) return false;
    Ptr<IAsyncInfoAbi> info;
    if (FAILED(op->QueryInterface(IID_AsyncInfo, (void**)&info))) return false;
    ULONGLONG until = GetTickCount64() + timeout;
    int st = 0;
    while (SUCCEEDED(info->get_Status(&st)) && st == 0 && GetTickCount64() < until) Sleep(5);
    if (st != 1) {
        if (st == 0) info->Cancel();
        return false;
    }
    return SUCCEEDED(op->GetResults(out));
}

template <class T>
bool Factory(const wchar_t* cls, const IID& iid, T** out) {
    HSTRING h = nullptr;
    A().create(cls, (UINT32)wcslen(cls), &h);
    HRESULT hr = A().factory(h, iid, (void**)out);
    A().del(h);
    return SUCCEEDED(hr);
}

std::wstring Str(HSTRING h) {
    if (!h) return L"";
    UINT32 n = 0;
    const wchar_t* s = A().raw(h, &n);
    std::wstring r(s, n);
    A().del(h);
    return r;
}

struct Job {
    std::vector<uint8_t> png;
    HWND hwnd;
    UINT msg;
};

// Окно могло закрыться, пока шло распознавание: тогда результат некому удалить
void PostText(const Job* j, WPARAM failed, std::wstring text) {
    auto* s = new std::wstring(std::move(text));
    if (!PostMessageW(j->hwnd, j->msg, failed, (LPARAM)s)) delete s;
}

void JobWork(const Job* j) {
    if (!A().ok) {
        PostText(j, 1, L"OCR недоступен");
        return;
    }
    A().roInit(RO_INIT_MULTITHREADED);
    std::wstring err;
    std::wstring text = ocr::Recognize(j->png, &err);
    A().roUninit();
    bool failed = text.empty() && !err.empty();
    PostText(j, failed ? 1 : 0, failed ? err : text);
}

// Картинка для движка и сами движки: основной по языку профиля и английский
bool Prepare(const std::vector<uint8_t>& png, Ptr<IInspectable>& bitmap, Ptr<IOcrEngine>& main, Ptr<IOcrEngine>& en,
             std::wstring* error) {
    auto fail = [&](const wchar_t* e) {
        if (error) *error = e;
        return false;
    };
    if (!A().ok) return fail(L"OCR недоступен");
    IStream* raw = SHCreateMemStream(png.data(), (UINT)png.size());
    if (!raw) return fail(L"Не хватило памяти");
    Ptr<IUnknown> ras;
    HRESULT hr = A().overStream(raw, 0, IID_RandomAccessStream, (void**)&ras);
    raw->Release();
    if (FAILED(hr)) return fail(L"Картинка не открылась");
    Ptr<IDecoderStatics> dec;
    Ptr<IAsyncOp> op;
    Ptr<IInspectable> decoder;
    if (!Factory(L"Windows.Graphics.Imaging.BitmapDecoder", IID_DecoderStatics, &dec) ||
        FAILED(dec->CreateAsync(ras.p, &op)) || !Await(op.p, &decoder))
        return fail(L"Картинка не открылась");
    Ptr<IFrameWithSoftwareBitmap> frame;
    if (FAILED(decoder->QueryInterface(IID_FrameWithSoftwareBitmap, (void**)&frame)))
        return fail(L"Картинка не открылась");
    Ptr<IAsyncOp> op2;
    if (FAILED(frame->GetSoftwareBitmapConvertedAsync(87, 0, &op2)) || !Await(op2.p, &bitmap))
        return fail(L"Картинка не открылась");
    Ptr<IOcrEngineStatics> statics;
    if (!Factory(L"Windows.Media.Ocr.OcrEngine", IID_OcrStatics, &statics)) return fail(L"OCR недоступен");
    statics->TryCreateFromUserProfileLanguages(&main);
    Ptr<ILanguageFactory> lf;
    Ptr<IInspectable> enLang;
    if (Factory(L"Windows.Globalization.Language", IID_LanguageFactory, &lf)) {
        HSTRING tag = nullptr;
        A().create(L"en-US", 5, &tag);
        if (SUCCEEDED(lf->CreateLanguage(tag, &enLang)) && enLang) statics->TryCreateFromLanguage(enLang.p, &en);
        A().del(tag);
    }
    if (!main && !en) return fail(L"В Windows не установлен язык для распознавания");
    return true;
}

// Строки движка с положением: рамка строки - по её словам
void ReadLines(IOcrEngine* engine, IInspectable* bitmap, std::vector<ocr::Line>& out) {
    Ptr<IAsyncOp> op;
    Ptr<IOcrResult> result;
    if (!engine || FAILED(engine->RecognizeAsync(bitmap, &op)) || !Await(op.p, &result) || !result) return;
    Ptr<ILineList> lines;
    if (FAILED(result->get_Lines(&lines)) || !lines) return;
    UINT32 n = 0;
    lines->get_Size(&n);
    for (UINT32 i = 0; i < n; ++i) {
        Ptr<IOcrLine> line;
        Ptr<IWordList> words;
        HSTRING h = nullptr;
        if (FAILED(lines->GetAt(i, &line)) || !line || FAILED(line->get_Text(&h))) continue;
        ocr::Line l;
        l.text = Str(h);
        if (SUCCEEDED(line->get_Words(&words)) && words) {
            UINT32 count = 0;
            words->get_Size(&count);
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            bool any = false;
            for (UINT32 k = 0; k < count; ++k) {
                Ptr<IOcrWord> word;
                OcrRect r{};
                if (FAILED(words->GetAt(k, &word)) || !word || FAILED(word->get_BoundingRect(&r))) continue;
                x0 = any ? std::min(x0, r.x) : r.x;
                y0 = any ? std::min(y0, r.y) : r.y;
                x1 = any ? std::max(x1, r.x + r.w) : r.x + r.w;
                y1 = any ? std::max(y1, r.y + r.h) : r.y + r.h;
                any = true;
            }
            l.x = x0;
            l.y = y0;
            l.w = x1 - x0;
            l.h = y1 - y0;
        }
        out.push_back(std::move(l));
    }
}

void ReadWords(IOcrEngine* engine, IInspectable* bitmap, int& nextLine, std::vector<ocr::Word>& out) {
    Ptr<IAsyncOp> op;
    Ptr<IOcrResult> result;
    if (!engine || FAILED(engine->RecognizeAsync(bitmap, &op)) || !Await(op.p, &result) || !result) return;
    Ptr<ILineList> lines;
    if (FAILED(result->get_Lines(&lines)) || !lines) return;
    UINT32 n = 0;
    lines->get_Size(&n);
    for (UINT32 i = 0; i < n; ++i, ++nextLine) {
        Ptr<IOcrLine> line;
        Ptr<IWordList> words;
        if (FAILED(lines->GetAt(i, &line)) || !line || FAILED(line->get_Words(&words)) || !words) continue;
        UINT32 count = 0;
        words->get_Size(&count);
        for (UINT32 k = 0; k < count; ++k) {
            Ptr<IOcrWord> word;
            HSTRING h = nullptr;
            OcrRect r{};
            if (FAILED(words->GetAt(k, &word)) || !word || FAILED(word->get_BoundingRect(&r)) ||
                FAILED(word->get_Text(&h)))
                continue;
            ocr::Word w;
            w.text = Str(h);
            w.x = r.x;
            w.y = r.y;
            w.w = r.w;
            w.h = r.h;
            w.line = nextLine;
            out.push_back(std::move(w));
        }
    }
}

std::atomic<bool> g_enhance{true};

// Подготовка картинки: увеличить мелкое, серый цвет и растяжка контраста. Не вышло - исходная как есть, масштаб 1.
// scale - во сколько раз подготовленная больше исходной: координаты движка делятся на него.
std::vector<uint8_t> Enhance(const std::vector<uint8_t>& png, float& scale) {
    scale = 1;
    if (!g_enhance) return png;
    std::unique_ptr<Gdiplus::Bitmap> src = snipimg::FromPng(png);
    if (!src) return png;
    const int w = (int)src->GetWidth(), h = (int)src->GetHeight();
    const float k = ocrtext::UpscaleFactor(w, h);
    const int bw = std::max(1, (int)std::lround(w * k)), bh = std::max(1, (int)std::lround(h * k));
    Gdiplus::Bitmap big(bw, bh, PixelFormat32bppRGB);
    {
        Gdiplus::Graphics g(&big);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.DrawImage(src.get(), Gdiplus::Rect(0, 0, bw, bh), 0, 0, w, h, Gdiplus::UnitPixel);
    }
    Gdiplus::Rect all(0, 0, bw, bh);
    Gdiplus::BitmapData d{};
    if (big.LockBits(&all, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite, PixelFormat32bppRGB, &d) != Gdiplus::Ok)
        return png;
    ocrtext::GrayContrast((uint32_t*)d.Scan0, bw, bh, d.Stride / 4);
    big.UnlockBits(&d);
    std::vector<uint8_t> out = snipimg::ToPng(&big);
    if (out.empty()) return png;
    scale = (float)bw / w;
    return out;
}

}  // namespace

namespace ocr {

void SetEnhance(bool on) { g_enhance = on; }

bool RecognizeLines(const std::vector<uint8_t>& png, std::vector<Line>& out, std::wstring* error) {
    float scale = 1;
    std::vector<uint8_t> ready = Enhance(png, scale);
    Ptr<IInspectable> bitmap;
    Ptr<IOcrEngine> main, en;
    if (!Prepare(ready, bitmap, main, en, error)) return false;
    // основной движок - по языку профиля (у тебя русский); английский - отдельно,
    // потому что русский читает латиницу похожими буквами («Hello» -> «Непо»)
    std::vector<Line> a, b;
    ReadLines(main.p ? main.p : en.p, bitmap.p, a);
    if (main.p && en.p) ReadLines(en.p, bitmap.p, b);
    out = ocrtext::MergeEngines(a, b);
    for (Line& l : out) {
        l.x /= scale;
        l.y /= scale;
        l.w /= scale;
        l.h /= scale;
    }
    return true;
}

std::wstring Recognize(const std::vector<uint8_t>& png, std::wstring* error) {
    std::vector<Line> lines;
    if (!RecognizeLines(png, lines, error)) return L"";
    std::wstring out = ocrtext::CleanText(lines);
    if (out.empty() && error) *error = L"Текста на снимке не нашлось";
    return out;
}

bool RecognizeWords(const std::vector<uint8_t>& png, std::vector<Word>& out, std::wstring* error) {
    float scale = 1;
    std::vector<uint8_t> ready = Enhance(png, scale);
    Ptr<IInspectable> bitmap;
    Ptr<IOcrEngine> main, en;
    if (!Prepare(ready, bitmap, main, en, error)) return false;
    int line = 0;
    ReadWords(main.p, bitmap.p, line, out);
    ReadWords(en.p, bitmap.p, line, out);
    for (Word& w : out) {
        w.x /= scale;
        w.y /= scale;
        w.w /= scale;
        w.h /= scale;
    }
    return true;
}

void RecognizeWordsAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg) {
    auto j = std::make_shared<Job>(Job{std::move(png), (HWND)hwnd, msg});
    bg::Run([j] {
        auto* words = new std::vector<Word>();
        bool ok = false;
        if (A().ok) {
            A().roInit(RO_INIT_MULTITHREADED);
            ok = RecognizeWords(j->png, *words);
            A().roUninit();
        }
        if (!PostMessageW(j->hwnd, j->msg, ok ? 0 : 1, (LPARAM)words)) delete words;
    });
}

void RecognizeLinesAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg) {
    auto j = std::make_shared<Job>(Job{std::move(png), (HWND)hwnd, msg});
    bg::Run([j] {
        auto* lines = new std::vector<Line>();
        bool ok = false;
        if (A().ok) {
            A().roInit(RO_INIT_MULTITHREADED);
            ok = RecognizeLines(j->png, *lines);
            A().roUninit();
        }
        if (!PostMessageW(j->hwnd, j->msg, ok ? 0 : 1, (LPARAM)lines)) delete lines;
    });
}

void RecognizeAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg) {
    auto j = std::make_shared<Job>(Job{std::move(png), (HWND)hwnd, msg});
    bg::Run([j] { JobWork(j.get()); });
}

}  // namespace ocr
