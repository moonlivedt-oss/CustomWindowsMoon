// «Телефон рядом», четвёртый заход: кольцо RadialMenu на телефоне (те же пункты, запуск на ПК), что сейчас играет
// (название, обложка, перемотка), громкость каждой программы, состояние ПК (процессор, память, батарея, диск).
#include <winsock2.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <map>

#include "core/text.h"
#include "desk/appvol.h"
#include "desk/media.h"
#include "phone/phone_internal.h"

namespace phone::detail {

namespace {

std::string Q(const Request& r, const char* k) {
    auto it = r.query.find(k);
    return it == r.query.end() ? std::string() : it->second;
}

media::Watcher* g_media = nullptr;  // создаётся в основном потоке на время передачи
SRWLOCK g_mediaLock = SRWLOCK_INIT;

SRWLOCK g_iconLock = SRWLOCK_INIT;
std::map<std::string, std::string> g_icons;  // путь значка -> PNG

bool Picture(const std::wstring& path) {
    std::wstring e = Lower(path.substr(path.find_last_of(L'.') + 1));
    return e == L"png" || e == L"ico" || e == L"jpg" || e == L"jpeg" || e == L"bmp" || e == L"gif" || e == L"webp";
}

// значок файла, программы или папки - PNG 96x96 с прозрачностью (как в Проводнике)
std::string IconPng(const std::wstring& path) {
    std::string out;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellItemImageFactory* f = nullptr;
    HBITMAP hb = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) &&
        // свой значок пункта (png, ico) - сама картинка; программа, папка, ярлык - их значок
        SUCCEEDED(f->GetImage({96, 96}, SIIGBF_BIGGERSIZEOK | (Picture(path) ? 0 : SIIGBF_ICONONLY), &hb)) && hb) {
        DIBSECTION ds{};
        if (GetObjectW(hb, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 32 && ds.dsBm.bmBits) {
            int w = ds.dsBm.bmWidth, h = std::abs(ds.dsBm.bmHeight), stride = ds.dsBm.bmWidthBytes;
            BYTE* bits = (BYTE*)ds.dsBm.bmBits;
            if (ds.dsBm.bmHeight > 0) bits += (size_t)(h - 1) * stride, stride = -stride;  // снизу вверх
            Gdiplus::Bitmap src(w, h, stride, PixelFormat32bppPARGB, bits);
            IStream* st = nullptr;
            CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) && src.Save(st, &png, nullptr) == Gdiplus::Ok) {
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
        }
        DeleteObject(hb);
    }
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

std::string MediaJson() {
    media::Track t;
    {
        AcquireSRWLockShared(&g_mediaLock);
        if (g_media) t = g_media->Get();
        ReleaseSRWLockShared(&g_mediaLock);
    }
    if (!t.has) return "{\"has\":false}";
    return "{\"has\":true,\"st\":" + std::to_string(t.status) + ",\"t\":" + phone::Json(t.title) +
           ",\"a\":" + phone::Json(t.artist) + ",\"app\":" + phone::Json(t.app) +
           ",\"pos\":" + std::to_string(t.Now() / 10000) + ",\"end\":" + std::to_string(t.end / 10000) +
           ",\"art\":" + std::to_string(t.art.empty() ? 0 : t.artId) + ",\"next\":" + (t.canNext ? "true" : "false") +
           ",\"prev\":" + (t.canPrev ? "true" : "false") + ",\"seek\":" + (t.canSeek ? "true" : "false") + "}";
}

std::string AppsJson() {
    std::vector<appvol::SessionNow> list = appvol::Current();
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.exe < b.exe; });
    std::string j = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        char v[16];
        snprintf(v, sizeof(v), "%.3f", list[i].level);
        j += std::string(i ? "," : "") + "{\"pid\":" + std::to_string(list[i].pid) +
             ",\"n\":" + phone::Json(list[i].exe) + ",\"v\":" + v + "}";
    }
    return j + "]";
}

// процессор - за время с прошлого вопроса (не чаще раза в 0,5 с), остальное - сейчас
std::string StatJson() {
    static SRWLOCK lock = SRWLOCK_INIT;
    static ULONGLONG lastIdle = 0, lastTotal = 0, lastAt = 0;
    static int cpu = 0;
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        auto U = [](const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
        ULONGLONG idle = U(fi), total = U(fk) + U(fu), now = GetTickCount64();  // ядро уже включает простой
        AcquireSRWLockExclusive(&lock);
        if (now - lastAt >= 500) {
            if (lastAt && total > lastTotal) cpu = (int)(100 - (idle - lastIdle) * 100 / (total - lastTotal));
            lastIdle = idle, lastTotal = total, lastAt = now;
        }
        ReleaseSRWLockExclusive(&lock);
    }
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    SYSTEM_POWER_STATUS ps{};
    GetSystemPowerStatus(&ps);
    ULARGE_INTEGER freeB{}, totalB{};
    wchar_t sys[MAX_PATH] = L"C:\\";
    GetWindowsDirectoryW(sys, MAX_PATH);
    sys[3] = 0;
    GetDiskFreeSpaceExW(sys, &freeB, &totalB, nullptr);
    wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD pcn = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(pc, &pcn);
    int bat = ps.BatteryFlag == 128 || ps.BatteryLifePercent > 100 ? -1 : ps.BatteryLifePercent;  // 128 - батареи нет
    return "{\"pc\":" + phone::Json(pc) + ",\"cpu\":" + std::to_string(std::clamp(cpu, 0, 100)) +
           ",\"ramUsed\":" + std::to_string((long long)(ms.ullTotalPhys - ms.ullAvailPhys)) +
           ",\"ram\":" + std::to_string((long long)ms.ullTotalPhys) + ",\"bat\":" + std::to_string(bat) +
           ",\"ac\":" + (ps.ACLineStatus == 1 ? "true" : "false") +
           ",\"diskFree\":" + std::to_string((long long)freeB.QuadPart) +
           ",\"disk\":" + std::to_string((long long)totalB.QuadPart) +
           ",\"up\":" + std::to_string((long long)(GetTickCount64() / 1000)) + "}";
}

}  // namespace

void MediaStart() {
    AcquireSRWLockExclusive(&g_mediaLock);
    if (!g_media) {
        g_media = new media::Watcher;
        g_media->Start(nullptr, 0);
    }
    ReleaseSRWLockExclusive(&g_mediaLock);
}

void MediaStop() {
    AcquireSRWLockExclusive(&g_mediaLock);
    media::Watcher* m = g_media;
    g_media = nullptr;
    ReleaseSRWLockExclusive(&g_mediaLock);
    delete m;       // Stop ждёт свой поток
    ReleaseKeys();  // геймпад: ничего не должно остаться зажатым после передачи
}

bool HandleExtra(Conn& cn, const Request& r, const std::string& rest) {
    bool get = r.method == "GET" || r.method == "HEAD", post = r.method == "POST";
    if (rest == "/ring" && get) {  // пункты обоих колец (как в radial.cfg)
        Seen();
        std::string j;
        if (!CallMain("ring", j)) j = "[]";
        JsonReply(cn, j);
        return true;
    }
    if (rest == "/ringrun" && post) {
        Seen();
        std::string out;
        if (!InputAllowed())
            Text(cn, 403, "пульт выключен (phone_input)");
        else if (!CallMain("ringrun:" + Q(r, "id"), out))
            Text(cn, 404, "такого пункта уже нет");
        else
            JsonReply(cn, "{\"ok\":true}");
        return true;
    }
    if (rest == "/ringicon" && get) {
        std::string path;
        if (!CallMain("ringicon:" + Q(r, "id"), path) || path.empty()) {
            Text(cn, 404, "без значка");
            return true;
        }
        std::string png;
        {
            AcquireSRWLockShared(&g_iconLock);
            auto it = g_icons.find(path);
            if (it != g_icons.end()) png = it->second;
            ReleaseSRWLockShared(&g_iconLock);
        }
        if (png.empty()) {
            std::wstring w = Widen(path);
            std::replace(w.begin(), w.end(), L'/', L'\\');  // «icons/code.png» из radial.cfg: Проводник ждёт «\»
            png = IconPng(w);
            AcquireSRWLockExclusive(&g_iconLock);
            if (g_icons.size() > 300) g_icons.clear();
            g_icons[path] = png;
            ReleaseSRWLockExclusive(&g_iconLock);
        }
        if (png.empty())
            Text(cn, 404, "значок не прочитался: " + path);
        else
            Reply(cn, 200, "image/png", png, "Cache-Control: max-age=600\r\n");
        return true;
    }
    if (rest == "/media" && get) {
        {
            Lock l;
            S().seen = GetTickCount64();
        }
        JsonReply(cn, MediaJson());
        return true;
    }
    if (rest == "/mediaart" && get) {
        std::vector<uint8_t> art;
        AcquireSRWLockShared(&g_mediaLock);
        if (g_media) art = g_media->Get().art;
        ReleaseSRWLockShared(&g_mediaLock);
        if (art.empty()) {
            Text(cn, 404, "без обложки");
        } else {
            bool png = art.size() > 4 && art[1] == 'P' && art[2] == 'N' && art[3] == 'G';
            Reply(cn, 200, png ? "image/png" : "image/jpeg", std::string(art.begin(), art.end()));
        }
        return true;
    }
    if (rest == "/mediacmd" && post) {
        Seen();
        std::string c = Q(r, "c");
        media::Cmd cmd = c == "next"   ? media::CMD_NEXT
                         : c == "prev" ? media::CMD_PREV
                         : c == "seek" ? media::CMD_SEEK
                                       : media::CMD_TOGGLE;
        AcquireSRWLockShared(&g_mediaLock);
        if (g_media) g_media->Send(cmd, atoll(Q(r, "t").c_str()) * 10000);  // мс -> сотни нс
        ReleaseSRWLockShared(&g_mediaLock);
        Text(cn, 200, "ok");
        return true;
    }
    if (rest == "/apps" && get) {
        Seen();
        JsonReply(cn, AppsJson());
        return true;
    }
    if (rest == "/appvol" && post) {
        Seen();
        bool ok = appvol::SetLevel((DWORD)strtoul(Q(r, "pid").c_str(), nullptr, 10), (float)atof(Q(r, "v").c_str()));
        Text(cn, ok ? 200 : 404, ok ? "ok" : "программа уже молчит");
        return true;
    }
    if (rest == "/links" && post) {  // ссылки с ПК прочитаны - убрать
        Seen();
        {
            Lock l;
            S().links.clear();
            ++S().seq;
        }
        Text(cn, 200, "ok");
        return true;
    }
    if (rest == "/stat" && get) {
        Seen();
        JsonReply(cn, StatJson());
        return true;
    }
    return false;
}

}  // namespace phone::detail
