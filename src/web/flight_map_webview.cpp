#include "web/flight_map_webview.h"
#include "util/log.h"
#include "web/flight_map_data.h"
#include "version.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <WebView2.h>

#include <cstdio>
#include <string>
#include <filesystem>
#include <chrono>

struct EnvCB : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    LONG ref = 1;
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&ref); if (r == 0) delete this; return r; }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler || riid == IID_IUnknown)
        { *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD(Invoke)(HRESULT r, ICoreWebView2Environment* e) override;
};
struct CtrlCB : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    LONG ref = 1;
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&ref); if (r == 0) delete this; return r; }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler || riid == IID_IUnknown)
        { *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD(Invoke)(HRESULT r, ICoreWebView2Controller* c) override;
};

struct FlightMapWebView::Impl {
    HWND hwnd = nullptr;
    ICoreWebView2Environment* env    = nullptr;
    ICoreWebView2Controller*  ctrl   = nullptr;
    ICoreWebView2*            webview = nullptr;
    bool ready = false;
    std::chrono::steady_clock::time_point lastUpdate{};

    ~Impl() {
        if (ctrl) { ctrl->Close(); ctrl->Release(); ctrl = nullptr; }
        if (webview) { webview->Release(); webview = nullptr; }
        if (env)  { env->Release();  env  = nullptr; }
    }
};

static FlightMapWebView::Impl* g_impl = nullptr;

HRESULT STDMETHODCALLTYPE EnvCB::Invoke(HRESULT result, ICoreWebView2Environment* env) {
    if (FAILED(result) || !env || !g_impl) return result ? result : E_POINTER;
    g_impl->env = env;
    env->AddRef();
    auto* cb = new CtrlCB{};
    HRESULT hr = env->CreateCoreWebView2Controller(g_impl->hwnd, cb);
    if (FAILED(hr)) { logWrite("[webview] CreateController failed: 0x%lx", (unsigned long)hr); cb->Release(); }
    return hr;
}

HRESULT STDMETHODCALLTYPE CtrlCB::Invoke(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || !controller || !g_impl) {
        logWrite("[webview] CtrlCB failed: 0x%lx", (unsigned long)(result ? result : E_POINTER));
        return result ? result : E_POINTER;
    }
    g_impl->ctrl = controller;
    controller->AddRef();

    HRESULT hr = controller->get_CoreWebView2(&g_impl->webview);
    if (FAILED(hr)) { logWrite("[webview] get_CoreWebView2 failed: 0x%lx", (unsigned long)hr); return hr; }

    RECT r{ -32000, -32000, -31999, -31999 };
    controller->put_Bounds(r);
    controller->put_IsVisible(FALSE);

    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return E_FAIL;
    const auto folder = std::filesystem::path(executable).parent_path() / "flight-map";
    ICoreWebView2_3* local = nullptr;
    hr = g_impl->webview->QueryInterface(IID_ICoreWebView2_3, reinterpret_cast<void**>(&local));
    if (FAILED(hr)) { logWrite("[webview] Update WebView2 runtime for the local flight map"); return hr; }
    hr = local->SetVirtualHostNameToFolderMapping(L"inmarscope.local", folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    local->Release();
    if (FAILED(hr)) return hr;
    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(g_impl->webview->get_Settings(&settings))) {
        ICoreWebView2Settings2* settings2 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings2, reinterpret_cast<void**>(&settings2)))) {
            const std::string agent = "InmarScope/" INMARSCOPE_VERSION " (+https://github.com/blkph0x/InmarScope)";
            const std::wstring wide(agent.begin(), agent.end());
            settings2->put_UserAgent(wide.c_str()); settings2->Release();
        }
        settings->Release();
    }
    hr = g_impl->webview->Navigate(L"https://inmarscope.local/index.html");
    g_impl->ready = SUCCEEDED(hr);
    logWrite("[webview] local received-aircraft map: 0x%lx", (unsigned long)hr);
    return S_OK;
}

FlightMapWebView::~FlightMapWebView() {
    g_impl = nullptr;
    delete impl_;
}

void FlightMapWebView::init(void* nativeHwnd) {
    if (impl_) return;
    impl_ = new Impl{};
    impl_->hwnd = (HWND)nativeHwnd;
    g_impl = impl_;
    auto* cb = new EnvCB{};
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr, cb);
}

void FlightMapWebView::updateAircraft(const std::vector<AircraftEntry>& aircraft) {
    if (!impl_ || !impl_->ready || !impl_->webview) return;
    LPWSTR source = nullptr;
    const HRESULT sourceResult = impl_->webview->get_Source(&source);
    const bool localPage = SUCCEEDED(sourceResult) && source &&
        std::wstring(source).rfind(L"https://inmarscope.local/", 0) == 0;
    CoTaskMemFree(source);
    if (!localPage) return; // Never pass decoded records to an external page.
    const auto now = std::chrono::steady_clock::now();
    if (now - impl_->lastUpdate < std::chrono::seconds(1)) return;
    impl_->lastUpdate = now;
    // JSON_ENSURE_ASCII safely represents decoded strings in JavaScript and
    // UTF-16. Retry each second, including when initial page loading is slow.
    const auto script = "window.updateAircraft && window.updateAircraft(" + flightMapJson(aircraft) + ");";
    const std::wstring wide(script.begin(), script.end());
    impl_->webview->ExecuteScript(wide.c_str(), nullptr);
}

void FlightMapWebView::setBounds(int x, int y, int w, int h, bool visible) {
    if (!impl_ || !impl_->ctrl) return;
    if (!IsWindow(impl_->hwnd)) return;
    // Pass coordinates as-is; ScreenToClient was causing the map
    // to anchor to screen centre instead of the parent window.
    RECT r = (w <= 0 || h <= 0 || !visible) ? RECT{-32000,-32000,-31999,-31999} : RECT{x, y, x + w, y + h};
    impl_->ctrl->put_Bounds(r);
    impl_->ctrl->put_IsVisible(visible && r.right > r.left && r.bottom > r.top);
}

bool FlightMapWebView::isReady() const { return impl_ && impl_->ready; }

#else // !_WIN32

// Non-Windows platforms have no embedded WebView2 browser. Provide no-op stubs
// so the rest of the application builds and links unchanged; the Flight Map
// panel is not drawn on these platforms (see gui_panels.cpp).
struct FlightMapWebView::Impl {};

FlightMapWebView::~FlightMapWebView() { delete impl_; }
void FlightMapWebView::init(void*) {}
void FlightMapWebView::updateAircraft(const std::vector<AircraftEntry>&) {}
void FlightMapWebView::setBounds(int, int, int, int, bool) {}
bool FlightMapWebView::isReady() const { return false; }

#endif // _WIN32
