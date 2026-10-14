#include "desk/ocr.h"

#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <shlwapi.h>
#include <winstring.h>

#include <memory>

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

struct IOcrLine : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Words(void**) = 0;
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

// Строка точно русская: есть кириллические буквы, которых нет среди похожих на латиницу
// (а, в, е, к, м, н, о, п, р, с, т, у, х, ь английский движок не различит с латинскими).
bool Cyrillic(const std::wstring& s) {
    static const wchar_t lookalike[] = L"авекмнопрстухьАВЕКМНОПРСТУХЬ";
    for (wchar_t c : s)
        if (c >= 0x0400 && c <= 0x04FF && !wcschr(lookalike, c)) return true;
    return false;
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

DWORD WINAPI JobThread(void* p) {
    std::unique_ptr<Job> j((Job*)p);
    if (!A().ok) {
        PostMessageW(j->hwnd, j->msg, 1, (LPARAM) new std::wstring(L"OCR недоступен"));
        return 0;
    }
    A().roInit(RO_INIT_MULTITHREADED);
    std::wstring err;
    std::wstring text = ocr::Recognize(j->png, &err);
    A().roUninit();
    bool failed = text.empty() && !err.empty();
    PostMessageW(j->hwnd, j->msg, failed ? 1 : 0, (LPARAM) new std::wstring(failed ? err : text));
    return 0;
}

}  // namespace

namespace ocr {

std::wstring Recognize(const std::vector<uint8_t>& png, std::wstring* error) {
    auto fail = [&](const wchar_t* e) {
        if (error) *error = e;
        return std::wstring();
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
    Ptr<IInspectable> bitmap;
    // Bgra8 (87), с готовой прозрачностью (0): такой формат OCR берёт без вопросов
    if (FAILED(frame->GetSoftwareBitmapConvertedAsync(87, 0, &op2)) || !Await(op2.p, &bitmap))
        return fail(L"Картинка не открылась");

    Ptr<IOcrEngineStatics> statics;
    if (!Factory(L"Windows.Media.Ocr.OcrEngine", IID_OcrStatics, &statics)) return fail(L"OCR недоступен");
    auto run = [&](IOcrEngine* engine) {
        std::vector<std::wstring> out;
        Ptr<IAsyncOp> op3;
        Ptr<IOcrResult> result;
        if (!engine || FAILED(engine->RecognizeAsync(bitmap.p, &op3)) || !Await(op3.p, &result) || !result) return out;
        Ptr<ILineList> lines;
        if (SUCCEEDED(result->get_Lines(&lines)) && lines) {
            UINT32 n = 0;
            lines->get_Size(&n);
            for (UINT32 i = 0; i < n; ++i) {
                Ptr<IOcrLine> line;
                HSTRING h = nullptr;
                if (SUCCEEDED(lines->GetAt(i, &line)) && line && SUCCEEDED(line->get_Text(&h))) out.push_back(Str(h));
            }
        }
        return out;
    };
    // основной движок - по языку профиля (у тебя русский); английский - отдельно,
    // потому что русский читает латиницу похожими буквами («Hello» -> «Непо»)
    Ptr<IOcrEngine> main, en;
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
    std::vector<std::wstring> a = run(main.p ? main.p : en.p),
                              b = main.p && en.p ? run(en.p) : std::vector<std::wstring>();
    std::wstring out;
    for (size_t i = 0; i < a.size(); ++i) {
        // строки обоих движков идут в одном порядке; если их число разное - доверяем основному
        const std::wstring& line = b.size() == a.size() && !Cyrillic(a[i]) && !b[i].empty() ? b[i] : a[i];
        if (!out.empty()) out += L"\r\n";
        out += line;
    }
    if (out.empty()) return fail(L"Текста на снимке не нашлось");
    return out;
}

void RecognizeAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg) {
    auto* j = new Job{std::move(png), (HWND)hwnd, msg};
    HANDLE t = CreateThread(nullptr, 0, JobThread, j, 0, nullptr);
    if (t)
        CloseHandle(t);
    else
        delete j;
}

}  // namespace ocr
