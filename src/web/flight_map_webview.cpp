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
#include <cctype>

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
    FlightMapWebView* owner = nullptr;
    int loadedMode = -1;          // mode currently shown in the browser
    std::string shownIcao;        // live mode: ICAO currently focused
    std::wstring defaultAgent;    // stock Edge UA, used for airplanes.live
    std::wstring localAgent;      // identifying UA for OSM tiles on local page
    std::vector<std::string> received;  // mode 1 filter (lowercase hex)
    std::string filterSent;             // last filter pushed into the page
    std::chrono::steady_clock::time_point lastFilterPush{};

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
    // The local map needs a virtual host; the live airplanes.live map does not,
    // so a failure here only disables the received-aircraft mode.
    ICoreWebView2_3* local = nullptr;
    hr = g_impl->webview->QueryInterface(IID_ICoreWebView2_3, reinterpret_cast<void**>(&local));
    if (FAILED(hr)) logWrite("[webview] Update WebView2 runtime for the local flight map");
    else {
        hr = local->SetVirtualHostNameToFolderMapping(L"inmarscope.local", folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        local->Release();
        if (FAILED(hr)) logWrite("[webview] local map folder mapping failed: 0x%lx", (unsigned long)hr);
    }
    {
        const std::string agent = "InmarScope/" INMARSCOPE_VERSION " (+https://github.com/blkph0x/InmarScope)";
        g_impl->localAgent.assign(agent.begin(), agent.end());
        ICoreWebView2Settings* settings = nullptr;
        if (SUCCEEDED(g_impl->webview->get_Settings(&settings))) {
            ICoreWebView2Settings2* settings2 = nullptr;
            if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings2, reinterpret_cast<void**>(&settings2)))) {
                LPWSTR ua = nullptr;
                if (SUCCEEDED(settings2->get_UserAgent(&ua)) && ua) { g_impl->defaultAgent = ua; CoTaskMemFree(ua); }
                settings2->Release();
            }
            settings->Release();
        }
    }
    g_impl->ready = true;
    if (g_impl->owner) g_impl->owner->applyMode();
    logWrite("[webview] flight map ready (mode %d)", g_impl->owner ? g_impl->owner->mode : -1);
    return S_OK;
}

namespace {
void setAgent(FlightMapWebView::Impl* impl, const std::wstring& agent) {
    if (agent.empty()) return;
    ICoreWebView2Settings* settings = nullptr;
    if (FAILED(impl->webview->get_Settings(&settings))) return;
    ICoreWebView2Settings2* settings2 = nullptr;
    if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings2, reinterpret_cast<void**>(&settings2)))) {
        settings2->put_UserAgent(agent.c_str()); settings2->Release();
    }
    settings->Release();
}
// Only hex digits reach the URL, so a decoded string can never alter it.
std::string cleanIcao(const std::string& icao) {
    std::string out;
    for (char c : icao) if (std::isxdigit((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
    return out.size() == 6 ? out : std::string{};
}
std::string joinIds(const std::vector<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) { if (!out.empty()) out += ','; out += id; }
    return out;
}
void navigateLive(FlightMapWebView::Impl* impl, const std::string& icao, bool filtered) {
    std::string url = "https://globe.airplanes.live/";
    if (filtered) {
        // tar1090 treats an empty icaoFilter as "no filter", so start with a
        // placeholder that matches nothing; the list is updated in-page.
        const std::string ids = joinIds(impl->received);
        std::vector<std::string> list = impl->received;
        if (!icao.empty() && std::find(list.begin(), list.end(), icao) == list.end()) list.push_back(icao);
        const std::string all = joinIds(list);
        url += "?icaoFilter=" + (all.empty() ? std::string("000000") : all);
        // Select and centre the voice-call aircraft, keeping the others visible.
        if (!icao.empty()) url += "&icao=" + icao + "&noIsolation";
        impl->filterSent = ids;
        impl->lastFilterPush = std::chrono::steady_clock::now();
    } else if (!icao.empty()) {
        url += "?icao=" + icao;
    }
    setAgent(impl, impl->defaultAgent);
    const std::wstring wide(url.begin(), url.end());
    const HRESULT hr = impl->webview->Navigate(wide.c_str());
    impl->shownIcao = icao.empty() ? impl->shownIcao : icao;
    logWrite("[webview] airplanes.live%s %s: 0x%lx", filtered ? " (received only)" : "",
             icao.empty() ? "" : icao.c_str(), (unsigned long)hr);
}
}

FlightMapWebView::~FlightMapWebView() {
    g_impl = nullptr;
    delete impl_;
}

void FlightMapWebView::init(void* nativeHwnd) {
    if (impl_) return;
    impl_ = new Impl{};
    impl_->hwnd = (HWND)nativeHwnd;
    impl_->owner = this;
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

void FlightMapWebView::applyMode() {
    if (!impl_ || !impl_->ready || !impl_->webview) return;
    if (mode < 0 || mode > 2) mode = 0;
    if (mode == impl_->loadedMode) return;
    impl_->loadedMode = mode;
    if (mode == 0) {
        navigateLive(impl_, impl_->shownIcao, false);
    } else if (mode == 1) {
        navigateLive(impl_, impl_->shownIcao, true);
    } else {
        setAgent(impl_, impl_->localAgent);
        const HRESULT hr = impl_->webview->Navigate(L"https://inmarscope.local/index.html");
        logWrite("[webview] local received-aircraft map: 0x%lx", (unsigned long)hr);
    }
}

void FlightMapWebView::showAircraft(const std::string& icaoHex) {
    if (!impl_) return;
    const std::string icao = cleanIcao(icaoHex);
    if (icao.empty() || icao == impl_->shownIcao) return; // keep the user's pan/zoom
    impl_->shownIcao = icao;
    if (!impl_->ready || !impl_->webview || impl_->loadedMode != mode) return; // applied on load
    if (mode == 0 || mode == 1) navigateLive(impl_, icao, mode == 1);
}

void FlightMapWebView::setReceived(const std::vector<std::string>& icaoHex) {
    if (!impl_) return;
    std::vector<std::string> clean;
    for (const auto& raw : icaoHex) {
        const std::string id = cleanIcao(raw);
        if (!id.empty() && std::find(clean.begin(), clean.end(), id) == clean.end()) clean.push_back(id);
    }
    impl_->received = std::move(clean);
    if (!impl_->ready || !impl_->webview || mode != 1 || impl_->loadedMode != 1) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - impl_->lastUpdate < std::chrono::seconds(1)) return;
    impl_->lastUpdate = now;
    // Only act on the airplanes.live page we loaded.
    LPWSTR source = nullptr;
    const bool live = SUCCEEDED(impl_->webview->get_Source(&source)) && source &&
        std::wstring(source).rfind(L"https://globe.airplanes.live/", 0) == 0;
    CoTaskMemFree(source);
    if (!live) return;
    std::string script;
    const std::string ids = joinIds(impl_->received);
    // Push when the list changes, and every few seconds in case the page
    // reloaded itself. Strings are validated hex, so the JS is safe to build.
    if (ids != impl_->filterSent || now - impl_->lastFilterPush > std::chrono::seconds(5)) {
        std::string arr = "[";
        for (const auto& id : impl_->received) { if (arr.size() > 1) arr += ','; arr += "'" + id + "'"; }
        arr += "]";
        script += "try{if(typeof icaoFilter!=='undefined'&&icaoFilter){const l=" + arr +
                  ";icaoFilter.length=0;Array.prototype.push.apply(icaoFilter,l.length?l:['000000']);}}catch(e){}";
        impl_->filterSent = ids;
        impl_->lastFilterPush = now;
    }
    if (script.empty()) return;
    const std::wstring wide(script.begin(), script.end());
    impl_->webview->ExecuteScript(wide.c_str(), nullptr);
}

std::string FlightMapWebView::shownIcao() const { return impl_ ? impl_->shownIcao : std::string{}; }

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
void FlightMapWebView::applyMode() {}
void FlightMapWebView::showAircraft(const std::string&) {}
void FlightMapWebView::setReceived(const std::vector<std::string>&) {}
std::string FlightMapWebView::shownIcao() const { return {}; }

#endif // _WIN32
