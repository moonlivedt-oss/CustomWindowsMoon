// Диски: тома и место, здоровье физических дисков (WMI Storage), что занимает место, кэши и временные файлы.
#include <windows.h>
#include <wbemidl.h>
#include <oleauto.h>

#include <algorithm>

#include "core/perf.h"
#include "core/text.h"
#include "net/net_internal.h"
#include "tools/tools.h"

namespace tools {

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

}  // namespace

std::vector<Volume> Volumes() {
    std::vector<Volume> out;
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW(512, buf);
    for (const wchar_t* p = buf; p < buf + n && *p; p += wcslen(p) + 1) {
        Volume v;
        v.root = p;
        v.type = GetDriveTypeW(p);
        if (v.type != DRIVE_FIXED && v.type != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER freeU{}, total{}, freeAll{};
        if (!GetDiskFreeSpaceExW(p, &freeU, &total, &freeAll)) continue;  // нет носителя
        v.total = total.QuadPart;
        v.free = freeU.QuadPart;
        wchar_t label[MAX_PATH] = L"", fs[64] = L"";
        GetVolumeInformationW(p, label, MAX_PATH, nullptr, nullptr, nullptr, fs, 64);
        v.label = label;
        v.fs = fs;
        out.push_back(v);
    }
    return out;
}

bool DiskWarn(const Volume& v) { return v.total && v.free * 10 < v.total && v.free < 20ull * 1024 * 1024 * 1024; }

std::vector<Physical> PhysicalDisks() {
    PERF_SCOPE("tools.PhysicalDisks");
    ComScope com;
    std::vector<Physical> out;
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc)))
        return out;
    IWbemServices* svc = nullptr;
    BSTR ns = SysAllocString(L"ROOT\\Microsoft\\Windows\\Storage");
    if (SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) {
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        BSTR lang = SysAllocString(L"WQL");
        BSTR q = SysAllocString(L"SELECT FriendlyName, MediaType, HealthStatus, Size FROM MSFT_PhysicalDisk");
        IEnumWbemClassObject* en = nullptr;
        if (SUCCEEDED(svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en))) {
            IWbemClassObject* o = nullptr;
            ULONG got = 0;
            while (en->Next(5000, 1, &o, &got) == S_OK && got) {
                Physical d;
                VARIANT v;
                if (SUCCEEDED(o->Get(L"FriendlyName", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR) d.name = v.bstrVal;
                VariantClear(&v);
                if (SUCCEEDED(o->Get(L"MediaType", 0, &v, nullptr, nullptr))) {
                    VariantChangeType(&v, &v, 0, VT_I4);
                    d.ssd = v.vt == VT_I4 && v.lVal == 4;
                }
                VariantClear(&v);
                if (SUCCEEDED(o->Get(L"HealthStatus", 0, &v, nullptr, nullptr))) {
                    VariantChangeType(&v, &v, 0, VT_I4);
                    if (v.vt == VT_I4) d.health = v.lVal;
                }
                VariantClear(&v);
                if (SUCCEEDED(o->Get(L"Size", 0, &v, nullptr, nullptr))) {
                    if (v.vt == VT_BSTR) d.size = _wcstoui64(v.bstrVal, nullptr, 10);  // uint64 в WMI приходит строкой
                }
                VariantClear(&v);
                out.push_back(d);
                o->Release();
            }
            en->Release();
        }
        SysFreeString(q);
        SysFreeString(lang);
        svc->Release();
    }
    SysFreeString(ns);
    loc->Release();
    return out;
}

std::wstring DiskHealthAdminMain() {
    // износ и температура (StorageReliabilityCounter) читаются только с правами; команда фиксированная
    std::wstring out;
    net::Exec(L"WindowsPowerShell\\v1.0\\powershell.exe",
              L"-NoProfile -Command \"Get-PhysicalDisk | ForEach-Object { $r = $_ | Get-StorageReliabilityCounter; "
              L"'disk:' + $_.FriendlyName + '|' + $r.Temperature + '|' + $r.Wear + '|' + $r.PowerOnHours }\"",
              &out, 30000);
    return out;
}

bool DiskHealth(std::vector<Physical>& disks, const Log& log) {
    bool any = false;
    net::Log collect = [&](const std::wstring& line) {
        if (line.rfind(L"disk:", 0) != 0) {
            log(line);
            return;
        }
        std::vector<std::wstring> p = Split(line.substr(5), L'|');
        if (p.size() < 4) return;
        for (Physical& d : disks)
            if (d.name == p[0]) {
                d.temp = p[1].empty() ? -1 : _wtoi(p[1].c_str());
                d.wear = p[2].empty() ? -1 : _wtoi(p[2].c_str());
                d.hours = p[3].empty() ? -1 : _wtoi(p[3].c_str());
                any = true;
            }
    };
    bool ok = net::RunAdmin(net::JOB_DISK_HEALTH, collect, 60000);
    return ok && any;
}

std::vector<FolderSize> ScanSizes(const std::wstring& dirIn, volatile bool* cancel, volatile long* progress,
                                  const std::function<void(const std::vector<FolderSize>&)>& partial) {
    PERF_SCOPE("tools.ScanSizes");
    std::wstring dir = dirIn;
    if (dir.empty()) return {};
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    if (dir.size() == 2) dir += L"\\";  // «C:» -> «C:\»
    std::wstring base = dir.back() == L'\\' ? dir.substr(0, dir.size() - 1) : dir;
    std::vector<FolderSize> out;
    FolderSize loose;
    loose.name = L"(файлы в корне)";
    loose.path = dir;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((base + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (cancel && *cancel) break;
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            FolderSize f;
            f.name = fd.cFileName;
            f.path = base + L"\\" + fd.cFileName;
            f.bytes = DirBytes(f.path, &f.files, cancel, progress);
            out.push_back(f);
            if (partial) {
                std::vector<FolderSize> sofar = out;
                std::sort(sofar.begin(), sofar.end(),
                          [](const FolderSize& a, const FolderSize& b) { return a.bytes > b.bytes; });
                partial(sofar);
            }
        } else {
            loose.bytes += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            ++loose.files;
            if (progress) InterlockedIncrement(progress);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (loose.files) out.push_back(loose);
    std::sort(out.begin(), out.end(), [](const FolderSize& a, const FolderSize& b) { return a.bytes > b.bytes; });
    return out;
}

}  // namespace tools
