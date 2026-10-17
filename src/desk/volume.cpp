#include "desk/volume.h"

#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <vector>

#include "core/text.h"
#include "desk/media.h"

namespace vol {

namespace {

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool Device(IMMDevice** out) {
    Com<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&en)))
        return false;
    return SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, out));
}

std::wstring ProcessExe(DWORD pid) {
    std::wstring r;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return r;
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        r = Lower(buf);
        r = r.substr(r.find_last_of(L'\\') + 1);
        if (EndsWithI(r, L".exe")) r.resize(r.size() - 4);
    }
    CloseHandle(p);
    return r;
}

// Каким признакам должен отвечать звуковой сеанс плеера.
struct Match {
    std::wstring package;            // семейство пакета для программ из Store
    std::vector<std::wstring> exes;  // имена программ строчными, без .exe
};

Match MatchFor(const std::wstring& aumid) {
    Match m;
    std::wstring a = Lower(aumid);
    size_t bang = a.find(L'!');
    if (bang != std::wstring::npos) m.package = a.substr(0, bang);
    std::wstring e = a;
    if (EndsWithI(e, L".exe")) e.resize(e.size() - 4);
    if (bang == std::wstring::npos) m.exes.push_back(e);
    for (auto& x : media::SourceExes(aumid)) m.exes.push_back(x);
    struct {
        const wchar_t* key;
        const wchar_t* exe;
    } known[] = {{L"yandex", L"browser"}, {L"308046b0af4a39cb", L"firefox"}, {L"zunemusic", L"microsoft.media.player"}};
    for (auto& k : known)
        if (a.find(k.key) != std::wstring::npos) m.exes.push_back(k.exe);
    return m;
}

// Все звуковые сеансы плеера (у браузера их бывает несколько).
std::vector<ISimpleAudioVolume*> Sessions(const std::wstring& aumid) {
    std::vector<ISimpleAudioVolume*> out;
    if (aumid.empty()) return out;
    Match m = MatchFor(aumid);
    Com<IMMDevice> dev;
    if (!Device(&dev)) return out;
    Com<IAudioSessionManager2> mgr;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) return out;
    Com<IAudioSessionEnumerator> en;
    if (FAILED(mgr->GetSessionEnumerator(&en))) return out;
    int n = 0;
    en->GetCount(&n);
    for (int i = 0; i < n; ++i) {
        Com<IAudioSessionControl> ctl;
        if (FAILED(en->GetSession(i, &ctl))) continue;
        Com<IAudioSessionControl2> c2;
        if (FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2))) continue;
        bool hit = false;
        LPWSTR id = nullptr;
        if (SUCCEEDED(c2->GetSessionIdentifier(&id)) && id) {
            std::wstring s = Lower(id);
            CoTaskMemFree(id);
            if (!m.package.empty() && s.find(m.package) != std::wstring::npos) hit = true;
            for (auto& e : m.exes)
                if (!e.empty() && s.find(L"\\" + e + L".exe") != std::wstring::npos) hit = true;
        }
        DWORD pid = 0;
        if (!hit && SUCCEEDED(c2->GetProcessId(&pid)) && pid) {
            std::wstring e = ProcessExe(pid);
            hit = !e.empty() && std::find(m.exes.begin(), m.exes.end(), e) != m.exes.end();
        }
        ISimpleAudioVolume* v = nullptr;
        if (hit && SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&v))) out.push_back(v);
    }
    return out;
}

float AppImpl(const std::wstring& aumid, float delta, bool set) {
    auto list = Sessions(aumid);
    float level = -1;
    if (!list.empty()) {
        float cur = 0;
        list[0]->GetMasterVolume(&cur);
        level = set ? std::clamp(cur + delta, 0.0f, 1.0f) : cur;
        if (set)
            for (auto* v : list) v->SetMasterVolume(level, nullptr);
    }
    for (auto* v : list) v->Release();
    return level;
}

float MasterImpl(float delta, bool set) {
    Com<IMMDevice> dev;
    if (!Device(&dev)) return -1;
    Com<IAudioEndpointVolume> ev;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&ev))) return -1;
    float cur = 0;
    ev->GetMasterVolumeLevelScalar(&cur);
    if (!set) return cur;
    float level = std::clamp(cur + delta, 0.0f, 1.0f);
    ev->SetMasterVolumeLevelScalar(level, nullptr);
    if (level > 0) ev->SetMute(FALSE, nullptr);
    return level;
}

}  // namespace

float AddMaster(float delta) { return MasterImpl(delta, true); }
float App(const std::wstring& aumid) { return AppImpl(aumid, 0, false); }
float AddApp(const std::wstring& aumid, float delta) { return AppImpl(aumid, delta, true); }

}  // namespace vol
