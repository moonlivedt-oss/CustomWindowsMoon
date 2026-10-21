#include "desk/brightness.h"

#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>

#include <algorithm>
#include <vector>

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

// DDC/CI: у внешних мониторов яркость меняется командой по кабелю.
bool DdcGet(HMONITOR mon, int& level) {
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || !n) return false;
    std::vector<PHYSICAL_MONITOR> pm(n);
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm.data())) return false;
    DWORD lo = 0, cur = 0, hi = 0;
    bool ok = GetMonitorBrightness(pm[0].hPhysicalMonitor, &lo, &cur, &hi) && hi > lo;
    if (ok) level = (int)((cur - lo) * 100 / (hi - lo));
    DestroyPhysicalMonitors(n, pm.data());
    return ok;
}

bool DdcSet(HMONITOR mon, int level) {
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || !n) return false;
    std::vector<PHYSICAL_MONITOR> pm(n);
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm.data())) return false;
    bool ok = false;
    for (auto& p : pm) {
        DWORD lo = 0, cur = 0, hi = 0;
        if (GetMonitorBrightness(p.hPhysicalMonitor, &lo, &cur, &hi) && hi > lo)
            ok |= SetMonitorBrightness(p.hPhysicalMonitor, lo + (hi - lo) * level / 100) != FALSE;
    }
    DestroyPhysicalMonitors(n, pm.data());
    return ok;
}

// WMI: встроенный экран ноутбука.
bool Wmi(IWbemServices** out) {
    Com<IWbemLocator> loc;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc)))
        return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, out);
    SysFreeString(ns);
    if (FAILED(hr)) return false;
    CoSetProxyBlanket(*out, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

bool FirstInstance(IWbemServices* svc, const wchar_t* query, IWbemClassObject** obj) {
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    Com<IEnumWbemClassObject> en;
    HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(q);
    ULONG got = 0;
    return SUCCEEDED(hr) && en && SUCCEEDED(en->Next(3000, 1, obj, &got)) && got == 1;
}

bool WmiGet(int& level) {
    Com<IWbemServices> svc;
    if (!Wmi(&svc)) return false;
    Com<IWbemClassObject> o;
    if (!FirstInstance(svc.p, L"SELECT CurrentBrightness FROM WmiMonitorBrightness", &o)) return false;
    VARIANT v;
    VariantInit(&v);
    bool ok = SUCCEEDED(o->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr));
    if (ok) level = v.vt == VT_UI1 ? v.bVal : v.lVal;
    VariantClear(&v);
    return ok;
}

bool WmiSet(int level) {
    Com<IWbemServices> svc;
    if (!Wmi(&svc)) return false;
    Com<IWbemClassObject> inst;
    if (!FirstInstance(svc.p, L"SELECT * FROM WmiMonitorBrightnessMethods", &inst)) return false;
    VARIANT path;
    VariantInit(&path);
    if (FAILED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) return false;
    BSTR cls = SysAllocString(L"WmiMonitorBrightnessMethods"), method = SysAllocString(L"WmiSetBrightness");
    Com<IWbemClassObject> def, inDef, in;
    bool ok = SUCCEEDED(svc->GetObject(cls, 0, nullptr, &def, nullptr)) &&
              SUCCEEDED(def->GetMethod(method, 0, &inDef, nullptr)) && SUCCEEDED(inDef->SpawnInstance(0, &in));
    if (ok) {
        VARIANT t, b;
        VariantInit(&t);
        VariantInit(&b);
        t.vt = VT_I4;
        t.lVal = 0;
        b.vt = VT_UI1;
        b.bVal = (BYTE)level;
        in->Put(L"Timeout", 0, &t, 0);
        in->Put(L"Brightness", 0, &b, 0);
        ok = SUCCEEDED(svc->ExecMethod(path.bstrVal, method, 0, nullptr, in.p, nullptr, nullptr));
    }
    SysFreeString(cls);
    SysFreeString(method);
    VariantClear(&path);
    return ok;
}

}  // namespace

namespace brightness {

// Для кольца «Мониторы»: сразу нужный уровень. DDC/CI у внешнего, WMI у экрана ноутбука. Медленно - только в фоне.
bool Get(HMONITOR mon, int& level) { return DdcGet(mon, level) || WmiGet(level); }
bool Set(HMONITOR mon, int level) {
    level = std::clamp(level, 0, 100);
    return DdcSet(mon, level) || WmiSet(level);
}

}  // namespace brightness

void Brightness::Start(HWND notify, UINT msg) {
    if (thread_) return;
    notify_ = notify;
    msg_ = msg;
    quit_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
}

void Brightness::Stop() {
    if (!thread_) return;
    quit_ = true;
    SetEvent(wake_);
    if (WaitForSingleObject(thread_, 3000) == WAIT_OBJECT_0) {
        CloseHandle(thread_);
        CloseHandle(wake_);
    }
    thread_ = wake_ = nullptr;
}

void Brightness::Add(HMONITOR mon, int delta) {
    AcquireSRWLockExclusive(&mu_);
    if (mon != mon_) pending_ = 0;
    mon_ = mon;
    pending_ += delta;
    ReleaseSRWLockExclusive(&mu_);
    SetEvent(wake_);
}

DWORD WINAPI Brightness::Thread(void* self) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                         EOAC_NONE, nullptr);
    ((Brightness*)self)->Loop();
    CoUninitialize();
    return 0;
}

void Brightness::Loop() {
    HMONITOR known = nullptr;  // для какого монитора запомнен уровень
    int level = -1;
    bool ddc = true;
    ULONGLONG knownAt = 0;
    while (!quit_) {
        WaitForSingleObject(wake_, INFINITE);
        if (quit_) break;
        AcquireSRWLockExclusive(&mu_);
        HMONITOR mon = mon_;
        int delta = pending_;
        pending_ = 0;
        ReleaseSRWLockExclusive(&mu_);
        if (!delta || !mon) continue;
        // уровень читаем заново, если давно не трогали: его могли поменять клавишами
        if (mon != known || level < 0 || GetTickCount64() - knownAt > 5000) {
            ddc = DdcGet(mon, level);
            if (!ddc && !WmiGet(level)) {
                PostMessageW(notify_, msg_, (WPARAM)-1, 0);  // яркость этого экрана не меняется
                continue;
            }
            known = mon;
        }
        level = std::clamp(level + delta, 0, 100);
        if (ddc ? DdcSet(mon, level) : WmiSet(level)) {
            knownAt = GetTickCount64();
            PostMessageW(notify_, msg_, (WPARAM)level, 0);
        } else {
            level = -1;
        }
    }
}
