// Перетаскивание файлов на меню: окно пересылает его настоящей папке под курсором.
#include "menu/menu.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

#include "core/files.h"
#include "core/launch.h"
#include "core/perf.h"
#include "core/text.h"
#include "core/themes.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/sound.h"
#include "menu/menu_internal.h"

using namespace Gdiplus;
using namespace menu_detail;

class MenuDrop : public IDropTarget {
public:
    explicit MenuDrop(RadialMenu* m) : m_(m) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // живёт столько же, сколько меню

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
        m_->OnDragEnter(d);
        DWORD allowed = *eff;
        *eff = m_->OnDragOver(keys, pt, allowed);
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragEnter(m_->hwnd_, d, &p, *eff);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* eff) override {
        *eff = m_->OnDragOver(keys, pt, *eff);
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragOver(&p, *eff);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        m_->OnDragLeave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->Drop(d, &p, *eff);
        }
        *eff = m_->OnDrop(d, keys, pt, *eff);
        return S_OK;
    }

private:
    RadialMenu* m_;
    IDropTargetHelper* helper_ = nullptr;
    ULONG refs_ = 1;
};

namespace menu_detail {
IDropTarget* CreateMenuDrop(RadialMenu* m) { return new MenuDrop(m); }
}  // namespace menu_detail

void RadialMenu::OnDragEnter(IDataObject* data) {
    dragging_ = true;
    if (dragData_) dragData_->Release();
    dragData_ = data;
    if (dragData_) dragData_->AddRef();
    targetKey_.clear();
    dwellKey_.clear();
}

void RadialMenu::LeaveShellTarget() {
    if (shellTarget_) {
        shellTarget_->DragLeave();
        shellTarget_->Release();
        shellTarget_ = nullptr;
    }
    targetKey_.clear();
    targetName_.clear();
    dragEffect_ = DROPEFFECT_NONE;
}

DWORD RadialMenu::OnDragOver(DWORD keys, POINTL pt, DWORD allowed) {
    if (!open_ || closing_) return DROPEFFECT_NONE;
    UpdateHover(HitTest({pt.x, pt.y}), GetTickCount64());
    const Item* t = HoveredItem();
    if (t && (t->special || t->full.empty())) t = nullptr;
    std::wstring key = t ? t->full : L"";
    keys &= ~MK_ALT;  // Alt держат, чтобы открыть меню; для Проводника он значит «ярлык»
    if (key != targetKey_) {
        LeaveShellTarget();
        targetKey_ = key;
        if (t) {
            targetName_ = t->name;
            targetDir_ = t->dir;
            // цель бросания берём у родительской папки, как это делает сам Проводник
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(t->full.c_str(), nullptr, &pidl, 0, nullptr))) {
                IShellFolder* parent = nullptr;
                PCUITEMID_CHILD child = nullptr;
                if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child))) {
                    parent->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr, (void**)&shellTarget_);
                    parent->Release();
                }
                CoTaskMemFree(pidl);
            }
            if (shellTarget_) {
                DWORD e = allowed;
                if (FAILED(shellTarget_->DragEnter(dragData_, keys, pt, &e))) e = DROPEFFECT_NONE;
                dragEffect_ = e;
                return e;
            }
        }
        dragEffect_ = DROPEFFECT_NONE;
        return DROPEFFECT_NONE;
    }
    if (!shellTarget_) return DROPEFFECT_NONE;
    DWORD e = allowed;
    if (FAILED(shellTarget_->DragOver(keys, pt, &e))) e = DROPEFFECT_NONE;
    dragEffect_ = e;
    return e;
}

void RadialMenu::OnDragLeave() {
    LeaveShellTarget();
    dragging_ = false;
    if (dragData_) dragData_->Release();
    dragData_ = nullptr;
}

DWORD RadialMenu::OnDrop(IDataObject* data, DWORD keys, POINTL pt, DWORD allowed) {
    DWORD e = DROPEFFECT_NONE;
    if (shellTarget_) {
        // что бросили - до Drop: после него Проводник может уже переносить файлы в фоне
        std::vector<std::wstring> files;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (targetDir_ && onDropped && data && SUCCEEDED(data->GetData(&fe, &sm))) {
            if (auto* hd = (HDROP)GlobalLock(sm.hGlobal)) {
                UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n && i < 500; ++i) {
                    UINT len = DragQueryFileW(hd, i, nullptr, 0);
                    std::wstring f(len + 1, L'\0');
                    DragQueryFileW(hd, i, f.data(), len + 1);
                    f.resize(len);
                    files.push_back(f);
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
        }
        std::wstring dir = targetKey_;
        e = allowed;
        if (FAILED(shellTarget_->Drop(data, keys & ~MK_ALT, pt, &e))) e = DROPEFFECT_NONE;
        shellTarget_->Release();  // после Drop цель сама завершила перетаскивание
        shellTarget_ = nullptr;
        if (e != DROPEFFECT_NONE && cfg_->sounds) Sound::Play(Sound::Drop);
        // перенос «оптимизированный» возвращает NONE, хотя файлы переехали: что было на деле - смотрит onDropped
        if (!files.empty()) onDropped(std::move(files), dir);
    }
    targetKey_.clear();
    dragging_ = false;
    if (dragData_) dragData_->Release();
    dragData_ = nullptr;
    Close();
    return e;
}
