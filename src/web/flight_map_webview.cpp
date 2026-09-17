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
#include <winhttp.h>
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
#include <future>
#include <ctime>
#include <stdexcept>
#include <algorithm>

namespace {
struct HttpHandle {
    HINTERNET value;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
struct LookupResult {
    FlightMapPositions positions;
    std::set<std::string> requested;
    std::string error;
};
LookupResult lookupPositions(std::set<std::string> ids) {
    LookupResult result; result.requested = ids;
    try {
        std::wstring path = L"/v2/hex/";
        for (const auto& id : ids) {
            if (path.back() != L'/') path += L',';
            path.append(id.begin(), id.end());
        }
        HttpHandle session{WinHttpOpen(L"InmarScope/" INMARSCOPE_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.value) throw std::runtime_error("Cannot start online lookup");
        WinHttpSetTimeouts(session.value, 4000, 4000, 4000, 4000);
        HttpHandle connection{WinHttpConnect(session.value, L"api.adsb.lol", INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!connection.value) throw std::runtime_error("Cannot connect to ADSB.lol");
        HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        if (!request.value) throw std::runtime_error("Cannot create online lookup");
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr)) throw std::runtime_error("ADSB.lol unavailable; retrying");
        DWORD status=0, size=sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
            throw std::runtime_error("ADSB.lol HTTP " + std::to_string(status) + "; retrying");
        std::string body;
        char buffer[16384]; DWORD read=0;
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &read)) throw std::runtime_error("Position download failed");
            if (!read) break;
            body.append(buffer, read);
            if (body.size() > 2*1024*1024 || std::chrono::steady_clock::now()-started > std::chrono::seconds(10))
                throw std::runtime_error("Position response exceeded limits");
        }
        result.positions = parseFlightMapPositions(body, ids, static_cast<double>(std::time(nullptr)));
    } catch (const std::exception& e) { result.error=e.what(); }
    return result;
}
}

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
    RECT bounds{};
    bool haveBounds = false;
    bool visible = false;
    std::chrono::steady_clock::time_point lastUpdate{};
    std::chrono::steady_clock::time_point nextLookup{};
    std::future<LookupResult> lookup;
    FlightMapPositions positions;
    std::string lookupStatus = "Online positions: waiting for received aircraft";
    size_t lookupOffset = 0;

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
    const double epoch = static_cast<double>(std::time(nullptr));
    if (impl_->lookup.valid() && impl_->lookup.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = impl_->lookup.get();
        if (onlinePositions) {
            if (result.error.empty()) {
                for (const auto& id : result.requested) impl_->positions.erase(id);
                impl_->positions.insert(result.positions.begin(), result.positions.end());
                impl_->lookupStatus = "ADSB.lol: " + std::to_string(result.positions.size()) + " position(s) found for " +
                                      std::to_string(result.requested.size()) + " received aircraft";
            } else {
                impl_->lookupStatus = result.error;
                impl_->nextLookup = now + std::chrono::seconds(60);
            }
        }
    }
    const auto ids = flightMapLookupIds(aircraft, epoch);
    for (auto it=impl_->positions.begin(); it!=impl_->positions.end();) {
        if (!onlinePositions || !ids.count(it->first) || epoch-it->second.time > 300) it=impl_->positions.erase(it);
        else ++it;
    }
    if (!onlinePositions) impl_->lookupStatus = "Online positions disabled; decoded coordinates only";
    else if (ids.empty()) impl_->lookupStatus = "No recently received aircraft need an online position";
    else if (!impl_->lookup.valid() && now >= impl_->nextLookup) {
        // At most one request per 20 seconds, with bounded rotating batches.
        std::vector<std::string> ordered(ids.begin(), ids.end());
        std::set<std::string> batch;
        for (size_t i=0; i<std::min<size_t>(100, ordered.size()); ++i)
            batch.insert(ordered[(impl_->lookupOffset+i)%ordered.size()]);
        impl_->lookupOffset = (impl_->lookupOffset+batch.size())%ordered.size();
        impl_->lookup = std::async(std::launch::async, lookupPositions, std::move(batch));
        impl_->nextLookup = now + std::chrono::seconds(20);
        impl_->lookupStatus = "Looking up positions for received aircraft only...";
    }
    // JSON_ENSURE_ASCII safely represents decoded strings in JavaScript and
    // UTF-16. Retry each second, including when initial page loading is slow.
    const auto script = "window.updateAircraft && window.updateAircraft(" + flightMapJson(aircraft, impl_->positions, epoch) + ");";
    const std::wstring wide(script.begin(), script.end());
    impl_->webview->ExecuteScript(wide.c_str(), nullptr);
}

void FlightMapWebView::setBounds(int x, int y, int w, int h, bool visible) {
    if (!impl_ || !impl_->ctrl) return;
    if (!IsWindow(impl_->hwnd)) return;
    // Pass coordinates as-is; ScreenToClient was causing the map
    // to anchor to screen centre instead of the parent window.
    RECT r = (w <= 0 || h <= 0 || !visible) ? RECT{-32000,-32000,-31999,-31999} : RECT{x, y, x + w, y + h};
    // The host draws every frame. Avoid repeatedly resizing the compositor
    // when the dock rectangle has not changed, especially during wheel zoom.
    if (!impl_->haveBounds || !EqualRect(&r, &impl_->bounds)) {
        if (SUCCEEDED(impl_->ctrl->put_Bounds(r))) { impl_->bounds = r; impl_->haveBounds = true; }
    }
    const bool show = visible && w > 0 && h > 0;
    if (show != impl_->visible && SUCCEEDED(impl_->ctrl->put_IsVisible(show))) impl_->visible = show;
}

bool FlightMapWebView::isReady() const { return impl_ && impl_->ready; }
std::string FlightMapWebView::positionStatus() const { return impl_ ? impl_->lookupStatus : "Map not initialized"; }

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
std::string FlightMapWebView::positionStatus() const { return "Map unavailable on this platform"; }

#endif // _WIN32
