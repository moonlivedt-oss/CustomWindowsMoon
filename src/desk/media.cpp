#include "desk/media.h"

#include <inspectable.h>
#include <propkey.h>
#include <roapi.h>
#include <shcore.h>
#include <shobjidl.h>
#include <winstring.h>

#include <algorithm>

#include "core/text.h"

// Заголовков Windows.Media.Control в MinGW нет, поэтому нужные интерфейсы описаны здесь вручную:
// порядок методов как в windows.media.control.idl из Windows SDK. IID нужен только у фабрики,
// остальные объекты приходят готовыми из методов.
namespace {

// Функции WinRT берутся из combase.dll напрямую, чтобы не тянуть libruntimeobject.a.
struct Api {
    HRESULT(WINAPI* roInit)(RO_INIT_TYPE) = nullptr;
    void(WINAPI* roUninit)() = nullptr;
    HRESULT(WINAPI* factory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* create)(PCNZWCH, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* del)(HSTRING) = nullptr;
    PCWSTR(WINAPI* raw)(HSTRING, UINT32*) = nullptr;
    HRESULT(WINAPI* overRas)(IUnknown*, REFIID, void**) = nullptr;
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
        overRas = (decltype(overRas))(void*)GetProcAddress(sh, "CreateStreamOverRandomAccessStream");
        ok = roInit && roUninit && factory && create && del && raw && overRas;
    }
};

const Api& A() {
    static Api api;
    return api;
}

const IID IID_ManagerStatics = {0x2050C4EE, 0x11A0, 0x57DE, {0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45}};
const IID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

}  // namespace

// Интерфейсы - не в анонимном пространстве: иначе GCC видит, что наследников у них нет,
// и с -O3 заменяет вызовы методов на «чистую виртуальную функцию».
namespace media_abi {

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;  // 0 идёт, 1 готово, 2 отменено, 3 ошибка
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// IAsyncOperation<T> для любого T: результат забирается как указатель
struct IAsyncOp : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void*) = 0;
};

struct IReferenceDouble : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Value(double*) = 0;
};

struct IStreamRef : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOp**) = 0;
};

struct IMediaProps : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IStreamRef**) = 0;
};

struct ITimeline : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(INT64*) = 0;
};

struct IPlaybackControls : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayPauseToggleEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsShuffleEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRepeatEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackRateEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackPositionEnabled(boolean*) = 0;
};

struct IPlaybackInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IPlaybackControls**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AutoRepeatMode(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackRate(IReferenceDouble**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsShuffleActive(void**) = 0;
};

struct ISession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(ITimeline**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IPlaybackInfo**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(int, IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(double, IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(boolean, IAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64, IAsyncOp**) = 0;
};

// IVectorView<Session>
struct ISessionList : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, ISession**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};

struct IManager : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(ISession**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(ISessionList**) = 0;
};

struct IManagerStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOp**) = 0;
};

}  // namespace media_abi

using namespace media_abi;

namespace {

template <class T>
struct Ptr {
    T* p = nullptr;
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    ~Ptr() { Reset(); }
    void Reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** operator&() {
        Reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring Str(HSTRING h) {
    if (!h) return L"";
    UINT32 n = 0;
    const wchar_t* s = A().raw(h, &n);
    std::wstring r(s, n);
    A().del(h);
    return r;
}

// Дождаться асинхронной операции (поток MTA, так что просто опрашиваем состояние).
bool Await(IAsyncOp* op, void* out, DWORD timeout = 3000) {
    if (!op) return false;
    Ptr<IAsyncInfoAbi> info;
    if (FAILED(op->QueryInterface(IID_AsyncInfo, (void**)&info))) return false;
    ULONGLONG until = GetTickCount64() + timeout;
    int st = 0;
    while (SUCCEEDED(info->get_Status(&st)) && st == 0 && GetTickCount64() < until) Sleep(4);
    if (st != 1) {
        if (st == 0) info->Cancel();
        return false;
    }
    return SUCCEEDED(op->GetResults(out));
}

std::vector<uint8_t> ReadArt(IStreamRef* ref) {
    std::vector<uint8_t> out;
    if (!ref) return out;
    Ptr<IAsyncOp> op;
    Ptr<IUnknown> ras;
    if (FAILED(ref->OpenReadAsync(&op)) || !Await(op.p, &ras)) return out;
    Ptr<IStream> st;
    if (FAILED(A().overRas(ras.p, IID_IStream, (void**)&st))) return out;
    uint8_t buf[16384];
    ULONG got = 0;
    while (out.size() < 8u * 1024 * 1024 && SUCCEEDED(st->Read(buf, sizeof(buf), &got)) && got) {
        out.insert(out.end(), buf, buf + got);
    }
    return out;
}

int64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

}  // namespace

namespace media {

int64_t Track::Now() const {
    if (status != kPlaying || !updated) return position;
    int64_t p = position + (int64_t)((NowFileTime() - updated) * rate);
    return end > 0 ? std::min(p, end) : p;
}

void Watcher::Start(HWND notify, UINT msg) {
    if (thread_) return;
    notify_ = notify;
    msg_ = msg;
    quit_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
}

void Watcher::Stop() {
    if (!thread_) return;
    quit_ = true;
    SetEvent(wake_);
    // поток может ждать ответа медиа-службы, долго не держим выход программы
    if (WaitForSingleObject(thread_, 4000) == WAIT_OBJECT_0) {
        CloseHandle(thread_);
        CloseHandle(wake_);
    }
    thread_ = wake_ = nullptr;
}

void Watcher::Send(Cmd c, int64_t arg) {
    {
        Lock l(mu_);
        cmds_.push_back({c, arg});
    }
    if (wake_) SetEvent(wake_);
}

void Watcher::SetFast(bool fast) {
    if (fast_ == fast) return;
    fast_ = fast;
    if (fast && wake_) SetEvent(wake_);
}

void Watcher::SetSource(const std::wstring& aumid) {
    {
        Lock l(mu_);
        chosen_ = aumid;
    }
    if (wake_) SetEvent(wake_);
}

Track Watcher::Get() {
    Lock l(mu_);
    return track_;
}

DWORD WINAPI Watcher::Thread(void* self) {
    if (!A().ok) return 0;
    A().roInit(RO_INIT_MULTITHREADED);
    ((Watcher*)self)->Loop();
    A().roUninit();
    return 0;
}

void Watcher::Loop() {
    Ptr<IManager> mgr;
    std::wstring artKey;  // для какого трека уже взята обложка
    int artTries = 0;     // браузеры отдают обложку не сразу после смены трека
    uint32_t nextId = 1, nextArt = 1;
    while (!quit_) {
        if (!mgr) {
            HSTRING cls = nullptr;
            const wchar_t name[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
            A().create(name, (UINT32)wcslen(name), &cls);
            Ptr<IManagerStatics> f;
            Ptr<IAsyncOp> op;
            if (SUCCEEDED(A().factory(cls, IID_ManagerStatics, (void**)&f)) && SUCCEEDED(f->RequestAsync(&op)))
                Await(op.p, &mgr);
            A().del(cls);
        }

        Ptr<ISession> s;
        if (mgr && FAILED(mgr->GetCurrentSession(&s))) mgr.Reset();  // служба перезапустилась

        // все сеансы: для переключателя источников; выбранный вручную важнее текущего
        std::vector<Source> sources;
        std::wstring chosen;
        {
            Lock l(mu_);
            chosen = chosen_;
        }
        Ptr<ISessionList> list;
        if (mgr && SUCCEEDED(mgr->GetSessions(&list)) && list) {
            UINT32 n = 0;
            list->get_Size(&n);
            for (UINT32 i = 0; i < n && i < 8; ++i) {
                Ptr<ISession> one;
                if (FAILED(list->GetAt(i, &one)) || !one) continue;
                Source src;
                HSTRING h = nullptr;
                if (SUCCEEDED(one->get_SourceAppUserModelId(&h))) src.aumid = Str(h);
                src.app = AppName(src.aumid);
                Ptr<IPlaybackInfo> pi;
                if (SUCCEEDED(one->GetPlaybackInfo(&pi)) && pi) pi->get_PlaybackStatus(&src.status);
                if (!chosen.empty() && src.aumid == chosen) {
                    s.Reset();
                    s.p = one.p;
                    one.p = nullptr;
                }
                sources.push_back(std::move(src));
            }
        }

        std::vector<std::pair<Cmd, int64_t>> cmds;
        {
            Lock l(mu_);
            cmds.swap(cmds_);
        }
        for (auto& [c, arg] : cmds) {
            if (!s) break;
            Ptr<IAsyncOp> op;
            if (c == CMD_TOGGLE) s->TryTogglePlayPauseAsync(&op);
            if (c == CMD_NEXT) s->TrySkipNextAsync(&op);
            if (c == CMD_PREV) {
                // как в обычных плеерах: дальше трёх секунд от начала «назад» - это в начало трека
                int64_t pos;
                bool seek;
                {
                    Lock l(mu_);
                    pos = track_.Now();
                    seek = track_.canSeek && track_.end > 0;
                }
                if (seek && pos > 30000000)
                    s->TryChangePlaybackPositionAsync(0, &op);
                else
                    s->TrySkipPreviousAsync(&op);
            }
            if (c == CMD_SEEK) s->TryChangePlaybackPositionAsync(arg, &op);
            boolean ok = 0;
            bool done = Await(op.p, &ok, 1500);
            // плеер отказался (например, у Медиаплеера «следующий» гаснет на последнем треке очереди):
            // пробуем мультимедийной клавишей, её Windows отдаст текущему сеансу
            if ((!done || !ok) && (c == CMD_NEXT || c == CMD_PREV)) {
                INPUT in[2]{};
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = c == CMD_NEXT ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PREV_TRACK;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
            }
            if (GetEnvironmentVariableW(L"RADIAL_MEDIA_LOG", nullptr, 0)) {
                FILE* f = _wfopen(L"media.log", L"a");
                if (f) {
                    fwprintf(f, L"cmd=%d done=%d ok=%d\n", (int)c, done, (int)ok);
                    fclose(f);
                }
            }
        }
        if (!cmds.empty()) Sleep(120);  // дать программе применить команду до опроса

        Track t;
        t.sources = std::move(sources);
        if (s) {
            t.has = true;
            HSTRING h = nullptr;
            if (SUCCEEDED(s->get_SourceAppUserModelId(&h))) t.aumid = Str(h);
            t.app = AppName(t.aumid);
            Ptr<IPlaybackInfo> pi;
            if (SUCCEEDED(s->GetPlaybackInfo(&pi)) && pi) {
                pi->get_PlaybackStatus(&t.status);
                Ptr<IPlaybackControls> pc;
                if (SUCCEEDED(pi->get_Controls(&pc)) && pc) {
                    boolean n = 1, p = 1, sk = 1;
                    pc->get_IsNextEnabled(&n);
                    pc->get_IsPreviousEnabled(&p);
                    pc->get_IsPlaybackPositionEnabled(&sk);
                    t.canNext = n;
                    t.canPrev = p;
                    t.canSeek = sk;
                }
                Ptr<IReferenceDouble> r;
                double rate = 1;
                if (SUCCEEDED(pi->get_PlaybackRate(&r)) && r && SUCCEEDED(r->get_Value(&rate)) && rate > 0)
                    t.rate = rate;
            }
            Ptr<ITimeline> tl;
            if (SUCCEEDED(s->GetTimelineProperties(&tl)) && tl) {
                INT64 st = 0, en = 0;
                tl->get_StartTime(&st);
                tl->get_EndTime(&en);
                tl->get_Position(&t.position);
                tl->get_LastUpdatedTime(&t.updated);
                t.end = en - st;
                t.position -= st;
            }
            Ptr<IAsyncOp> op;
            Ptr<IMediaProps> mp;
            if (SUCCEEDED(s->TryGetMediaPropertiesAsync(&op)) && Await(op.p, &mp) && mp) {
                if (SUCCEEDED(mp->get_Title(&h))) t.title = Str(h);
                if (SUCCEEDED(mp->get_Artist(&h))) t.artist = Str(h);
                if (t.artist.empty() && SUCCEEDED(mp->get_AlbumArtist(&h))) t.artist = Str(h);
                std::wstring key = t.aumid + L"\n" + t.title + L"\n" + t.artist;
                if (key != artKey) {
                    artKey = key;
                    artTries = 0;
                }
                Lock l(mu_);
                bool sameTrack =
                    track_.has && track_.aumid == t.aumid && track_.title == t.title && track_.artist == t.artist;
                t.id = sameTrack ? track_.id : nextId++;
                if (sameTrack) {
                    t.art = track_.art;
                    t.artId = track_.artId;
                }
            }
            if (t.art.empty() && artTries < 3 && mp) {
                ++artTries;
                Ptr<IStreamRef> ref;
                if (SUCCEEDED(mp->get_Thumbnail(&ref)) && ref) {
                    t.art = ReadArt(ref.p);
                    if (!t.art.empty()) t.artId = nextArt++;
                }
            }
        }

        bool changed;
        {
            Lock l(mu_);
            changed = t.has != track_.has || t.status != track_.status || t.id != track_.id ||
                      t.artId != track_.artId || t.end != track_.end || t.app != track_.app ||
                      t.sources.size() != track_.sources.size() || t.canNext != track_.canNext ||
                      t.canPrev != track_.canPrev ||
                      std::llabs(t.Now() - track_.Now()) > 20000000;  // перемотали больше чем на 2 с
            track_ = std::move(t);
        }
        if (changed && notify_) PostMessageW(notify_, msg_, 0, 0);
        WaitForSingleObject(wake_, fast_ ? 1000 : 4000);
    }
}

std::wstring AppName(const std::wstring& aumid) {
    std::wstring a = Lower(aumid);
    struct {
        const wchar_t* key;
        const wchar_t* name;
    } known[] = {{L"yandex", L"Яндекс"},        {L"spotify", L"Spotify"},
                 {L"chrome", L"Chrome"},        {L"308046b0af4a39cb", L"Firefox"},
                 {L"firefox", L"Firefox"},      {L"msedge", L"Edge"},
                 {L"telegram", L"Telegram"},    {L"vlc", L"VLC"},
                 {L"zunemusic", L"Медиаплеер"}, {L"discord", L"Discord"},
                 {L"opera", L"Opera"},          {L"aimp", L"AIMP"},
                 {L"foobar", L"foobar2000"},    {L"steam", L"Steam"}};
    for (auto& k : known)
        if (a.find(k.key) != std::wstring::npos) return k.name;
    std::wstring n =
        aumid.substr(aumid.find_last_of(L"\\/!") == std::wstring::npos ? 0 : aumid.find_last_of(L"\\/!") + 1);
    if (EndsWithI(n, L".exe")) n.resize(n.size() - 4);
    return n;
}

namespace {

struct FindCtx {
    std::wstring aumid, exe;  // exe - имя программы без .exe, в нижнем регистре
    HWND best = nullptr;
};

std::wstring WindowAumid(HWND w) {
    std::wstring r;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(w, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR) r = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return r;
}

std::wstring WindowExe(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        r = buf;
        r = Lower(r.substr(r.find_last_of(L'\\') + 1));
        if (EndsWithI(r, L".exe")) r.resize(r.size() - 4);
    }
    CloseHandle(p);
    return r;
}

BOOL CALLBACK FindProc(HWND w, LPARAM lp) {
    auto* c = (FindCtx*)lp;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || GetWindowTextLengthW(w) == 0) return TRUE;
    if (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    std::wstring wa = WindowAumid(w);
    if ((!wa.empty() && _wcsicmp(wa.c_str(), c->aumid.c_str()) == 0) || (!c->exe.empty() && WindowExe(w) == c->exe)) {
        c->best = w;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

std::vector<std::wstring> SourceExes(const std::wstring& aumid) {
    struct Ctx {
        std::wstring aumid;
        std::vector<std::wstring> out;
    } c{aumid, {}};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            std::wstring wa = WindowAumid(w);
            if (wa.empty() || _wcsicmp(wa.c_str(), c->aumid.c_str()) != 0) return TRUE;
            std::wstring e = WindowExe(w);
            if (!e.empty() && std::find(c->out.begin(), c->out.end(), e) == c->out.end()) c->out.push_back(e);
            return TRUE;
        },
        (LPARAM)&c);
    return c.out;
}

void FocusSource(const std::wstring& aumid) {
    if (aumid.empty()) return;
    FindCtx c;
    c.aumid = aumid;
    c.exe = Lower(aumid);
    if (EndsWithI(c.exe, L".exe")) c.exe.resize(c.exe.size() - 4);
    if (c.exe.find(L'!') != std::wstring::npos) c.exe.clear();  // это пакет из Store
    EnumWindows(FindProc, (LPARAM)&c);
    if (c.best) {
        if (IsIconic(c.best)) ShowWindow(c.best, SW_RESTORE);
        SetForegroundWindow(c.best);
        return;
    }
    IApplicationActivationManager* am = nullptr;
    if (SUCCEEDED(
            CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am)))) {
        DWORD pid = 0;
        am->ActivateApplication(aumid.c_str(), nullptr, AO_NONE, &pid);
        am->Release();
    }
}

}  // namespace media
