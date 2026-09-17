#include "web/flight_map_webview.h"
#include "util/log.h"

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
    std::string pendingIcao;

    ~Impl() {
        if (ctrl) { ctrl->Close(); ctrl->Release(); ctrl = nullptr; }
        if (env)  { env->Release();  env  = nullptr; }
    }
    void nav(const std::string& icao) {
        if (!webview) return;
        char url[160];
        if (icao.empty()) std::snprintf(url, sizeof(url), "https://globe.airplanes.live/");
        else std::snprintf(url, sizeof(url), "https://globe.airplanes.live/?icao=%s", icao.c_str());
        wchar_t wurl[160]; int i;
        for (i = 0; url[i]; ++i) wurl[i] = (wchar_t)(unsigned char)url[i];
        wurl[i] = 0;
        webview->Navigate(wurl);
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

    g_impl->ready = true;
    logWrite("[webview] ready");

    std::string icao = g_impl->pendingIcao;
    g_impl->pendingIcao.clear();
    g_impl->nav(icao); // Load the normal traffic map even before the first decode.
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

void FlightMapWebView::setIcao(const std::string& icao) {
    if (!impl_) return;
    if (impl_->ready && impl_->webview) impl_->nav(icao);
    else impl_->pendingIcao = icao;
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
void FlightMapWebView::setIcao(const std::string&) {}
void FlightMapWebView::setBounds(int, int, int, int, bool) {}
bool FlightMapWebView::isReady() const { return false; }

#endif // _WIN32
