#include "ui/narrator.h"

#include <ole2.h>
#include <uiautomation.h>

#include <atomic>

namespace ui {

// Минимальный серверный провайдер: имя и тип «панель»; события - уведомлениями
struct Narrator::Provider final : IRawElementProviderSimple {
    std::atomic<ULONG> refs{1};
    HWND hwnd = nullptr;
    std::wstring name = L"PlayerMoon";

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *out = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override {
        if (!options) return E_POINTER;
        *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* value) override {
        if (!value) return E_POINTER;
        VariantInit(value);
        if (id == UIA_NamePropertyId) {
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocString(name.c_str());
        } else if (id == UIA_ControlTypePropertyId) {
            value->vt = VT_I4;
            value->lVal = UIA_PaneControlTypeId;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        return UiaHostProviderFromHwnd(hwnd, out);
    }
};

Narrator::~Narrator() { Detach(); }

void Narrator::Detach() {
    if (provider_) {
        UiaReturnRawElementProvider(hwnd_, 0, 0, nullptr);  // отпустить ссылки UIA на провайдер
        provider_->Release();
        provider_ = nullptr;
    }
}

LRESULT Narrator::OnGetObject(WPARAM wp, LPARAM lp, bool& handled) {
    handled = false;
    if (static_cast<long>(lp) != static_cast<long>(UiaRootObjectId) || !hwnd_) return 0;
    if (!provider_) {
        provider_ = new Provider;
        provider_->hwnd = hwnd_;
    }
    handled = true;
    return UiaReturnRawElementProvider(hwnd_, wp, lp, provider_);
}

void Narrator::Announce(const std::wstring& text) {
    if (text.empty()) return;
    // Диктор не запущен - никто не спрашивал провайдер, проговаривать некому
    if (!provider_ || !UiaClientsAreListening()) return;
    provider_->name = text;
    BSTR message = SysAllocString(text.c_str());
    BSTR activity = SysAllocString(L"PlayerMoon");
    UiaRaiseNotificationEvent(provider_, NotificationKind_Other, NotificationProcessing_ImportantMostRecent, message,
                              activity);
    SysFreeString(message);
    SysFreeString(activity);
}

}  // namespace ui
