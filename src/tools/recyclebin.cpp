// Вернуть файлы из Корзины на прежнее место (Л3, отмена в журнале автоматики): как «Восстановить» в Проводнике -
// пункт undelete контекстного меню Корзины. Ничего не удаляет; файла в Корзине уже нет - он просто не вернётся.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <map>

#include "tools/tools.h"

namespace tools {

namespace {

// Исходная папка и время удаления у предмета Корзины (столбцы «Исходное расположение» и «Дата удаления»)
const GUID kDisplaced = {0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}};

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

}  // namespace

int RestoreRecycled(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return 0;
    std::map<std::wstring, bool> want;
    for (const std::wstring& p : paths) want[Lower(p)] = true;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int restored = 0;
    IShellFolder* desk = nullptr;
    PIDLIST_ABSOLUTE binPidl = nullptr;
    IShellFolder2* bin = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&desk)) &&
        SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &binPidl)) &&
        SUCCEEDED(desk->BindToObject(binPidl, nullptr, IID_PPV_ARGS(&bin)))) {
        // один и тот же путь мог удаляться не раз - берём самое свежее удаление
        struct Hit {
            PITEMID_CHILD id = nullptr;
            double when = 0;
        };
        std::map<std::wstring, Hit> hits;
        IEnumIDList* en = nullptr;
        if (bin->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en) == S_OK) {
            PITEMID_CHILD id = nullptr;
            while (en->Next(1, &id, nullptr) == S_OK) {
                bool keep = false;
                STRRET sr{};
                wchar_t name[MAX_PATH] = {};
                SHCOLUMNID from{kDisplaced, 2}, date{kDisplaced, 3};
                VARIANT vf, vd;
                VariantInit(&vf);
                VariantInit(&vd);
                if (SUCCEEDED(bin->GetDisplayNameOf(id, SHGDN_INFOLDER | SHGDN_FORPARSING, &sr)) &&
                    SUCCEEDED(StrRetToBufW(&sr, id, name, MAX_PATH)) && SUCCEEDED(bin->GetDetailsEx(id, &from, &vf)) &&
                    vf.vt == VT_BSTR) {
                    std::wstring full = std::wstring(vf.bstrVal) + L"\\" + PathFindFileNameW(name);
                    // имя «в папке» у Корзины бывает служебным ($R...) - тогда берём обычное отображаемое
                    if (!want.count(Lower(full))) {
                        STRRET sr2{};
                        wchar_t shown[MAX_PATH] = {};
                        if (SUCCEEDED(bin->GetDisplayNameOf(id, SHGDN_INFOLDER, &sr2)) &&
                            SUCCEEDED(StrRetToBufW(&sr2, id, shown, MAX_PATH)))
                            full = std::wstring(vf.bstrVal) + L"\\" + shown;
                    }
                    std::wstring key = Lower(full);
                    if (want.count(key)) {
                        double when = 0;
                        if (SUCCEEDED(bin->GetDetailsEx(id, &date, &vd)) && vd.vt == VT_DATE) when = vd.date;
                        Hit& h = hits[key];
                        if (!h.id || when > h.when) {
                            if (h.id) CoTaskMemFree(h.id);
                            h = {id, when};
                            keep = true;
                        }
                    }
                }
                VariantClear(&vf);
                VariantClear(&vd);
                if (!keep) CoTaskMemFree(id);
            }
            en->Release();
        }
        std::vector<PCUITEMID_CHILD> ids;
        for (auto& [k, h] : hits) ids.push_back(h.id);
        IContextMenu* menu = nullptr;
        if (!ids.empty() && SUCCEEDED(bin->GetUIObjectOf(nullptr, (UINT)ids.size(), ids.data(), IID_IContextMenu,
                                                         nullptr, (void**)&menu))) {
            CMINVOKECOMMANDINFO ci{sizeof(ci)};
            ci.lpVerb = "undelete";
            ci.fMask = CMIC_MASK_FLAG_NO_UI;
            ci.nShow = SW_HIDE;
            if (SUCCEEDED(menu->InvokeCommand(&ci)))
                for (const std::wstring& p : paths)
                    restored += GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
            menu->Release();
        }
        for (auto& [k, h] : hits) CoTaskMemFree(h.id);
    }
    if (bin) bin->Release();
    if (binPidl) CoTaskMemFree(binPidl);
    if (desk) desk->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return restored;
}

}  // namespace tools
