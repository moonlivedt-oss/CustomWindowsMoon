// Громкость по программам (Л36): звуковые сеансы устройства по умолчанию раз в 4 с, свой поток (MTA).
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <atomic>

#include "core/text.h"
#include "desk/appvol.h"

namespace appvol {

namespace {

HANDLE g_stop = nullptr, g_thread = nullptr;
std::atomic<bool> g_on{true};
SRWLOCK g_lock = SRWLOCK_INIT;
std::map<std::wstring, float> g_saved;  // под g_lock
bool g_loaded = false;

std::wstring File() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", p, MAX_PATH);
    CreateDirectoryW(p, nullptr);
    return std::wstring(p) + L"\\appvol.tsv";
}

void Load() {  // под g_lock
    if (g_loaded) return;
    g_loaded = true;
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string s(64 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    g_saved = ParseSaved(Widen(s));
}

void Write() {  // под g_lock
    std::string s = Narrow(SavedText(g_saved));
    HANDLE f = CreateFileW(File().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(f);
}

std::wstring ExeOf(DWORD pid) {
    std::wstring r;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return r;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        r = Lower(buf);
        r = r.substr(r.find_last_of(L'\\') + 1);
        if (EndsWithI(r, L".exe")) r.resize(r.size() - 4);
    }
    CloseHandle(p);
    return r;
}

struct Live {
    SessionNow s;
    ISimpleAudioVolume* v = nullptr;
};

// живые сеансы (кроме ушедших и системных звуков); ISimpleAudioVolume - освободить вызывающему
std::vector<Live> Sessions(std::map<DWORD, std::wstring>& exeCache) {
    std::vector<Live> out;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionEnumerator* list = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&en)) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&list))) {
        int n = 0;
        list->GetCount(&n);
        for (int i = 0; i < n; ++i) {
            IAudioSessionControl* ctl = nullptr;
            if (FAILED(list->GetSession(i, &ctl))) continue;
            IAudioSessionControl2* c2 = nullptr;
            AudioSessionState st = AudioSessionStateExpired;
            DWORD pid = 0;
            if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)) &&
                SUCCEEDED(ctl->GetState(&st)) && st != AudioSessionStateExpired &&
                c2->IsSystemSoundsSession() != S_OK && SUCCEEDED(c2->GetProcessId(&pid)) && pid) {
                auto it = exeCache.find(pid);
                std::wstring exe = it != exeCache.end() ? it->second : (exeCache[pid] = ExeOf(pid));
                ISimpleAudioVolume* v = nullptr;
                float level = 1;
                if (!exe.empty() && SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&v)) &&
                    SUCCEEDED(v->GetMasterVolume(&level)))
                    out.push_back({{pid, exe, level}, v});
                else if (v)
                    v->Release();
            }
            if (c2) c2->Release();
            ctl->Release();
        }
    }
    if (list) list->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    return out;
}

DWORD WINAPI Loop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::map<DWORD, float> last;
    std::map<DWORD, std::wstring> exeCache;  // номер процесса -> имя (сеанс живёт, пока жив процесс)
    bool first = true;
    do {
        if (!g_on) {
            last.clear();
            first = true;
            continue;
        }
        std::vector<Live> live = Sessions(exeCache);
        std::vector<SessionNow> now;
        for (const Live& l : live) now.push_back(l.s);
        AcquireSRWLockExclusive(&g_lock);
        Load();
        std::map<std::wstring, float> saved = g_saved;
        ReleaseSRWLockExclusive(&g_lock);
        Plan p = PlanVolumes(now, last, saved, first);
        first = false;
        for (const auto& [pid, level] : p.set)
            for (const Live& l : live)
                if (l.s.pid == pid) l.v->SetMasterVolume(level, nullptr);
        if (!p.save.empty()) {
            AcquireSRWLockExclusive(&g_lock);
            for (const auto& [exe, level] : p.save) g_saved[exe] = level;
            Write();
            ReleaseSRWLockExclusive(&g_lock);
        }
        for (Live& l : live) l.v->Release();
        for (auto it = exeCache.begin(); it != exeCache.end();)  // ушедшие процессы: номер может достаться другому
            it = last.count(it->first) ? std::next(it) : exeCache.erase(it);
    } while (WaitForSingleObject(g_stop, g_on ? 4000 : 30000) == WAIT_TIMEOUT);
    CoUninitialize();
    return 0;
}

}  // namespace

void Start(bool enabled) {
    g_on = enabled;
    if (g_thread) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
}

void SetEnabled(bool on) { g_on = on; }

void Stop() {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) WaitForSingleObject(g_thread, 3000), CloseHandle(g_thread);
    g_thread = nullptr;
}

std::map<std::wstring, float> Saved() {
    AcquireSRWLockExclusive(&g_lock);
    Load();
    std::map<std::wstring, float> s = g_saved;
    ReleaseSRWLockExclusive(&g_lock);
    return s;
}

std::vector<SessionNow> Current() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::map<DWORD, std::wstring> cache;
    std::vector<SessionNow> out;
    for (Live& l : Sessions(cache)) {
        out.push_back(l.s);
        l.v->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

void Forget(const std::wstring& exe) {
    AcquireSRWLockExclusive(&g_lock);
    Load();
    g_saved.erase(exe);
    Write();
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace appvol
