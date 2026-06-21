#include "update/version_check.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <cctype>
#include <cstdlib>
#include <vector>

namespace {

// Extract a JSON string value for "key": "..." (returns "" for null/missing).
std::string jsonString(const std::string& body, const std::string& key)
{
    std::string pat = "\"" + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return "";
    size_t c = body.find(':', k + pat.size());
    if (c == std::string::npos) return "";
    size_t i = c + 1;
    while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
    if (i >= body.size() || body[i] != '"') return ""; // null / number / bool
    ++i;
    std::string out;
    for (; i < body.size() && body[i] != '"'; ++i)
    {
        if (body[i] == '\\' && i + 1 < body.size()) ++i;
        out += body[i];
    }
    return out;
}

bool jsonBoolTrue(const std::string& body, const std::string& key)
{
    std::string pat = "\"" + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return false;
    size_t c = body.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t i = c + 1;
    while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
    return body.compare(i, 4, "true") == 0;
}

// Compare dotted-numeric versions: returns >0 if a>b, <0 if a<b, 0 equal.
int cmpVersion(const std::string& a, const std::string& b)
{
    auto next = [](const std::string& s, size_t& p) -> long {
        size_t e = s.find('.', p);
        std::string tok = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = (e == std::string::npos) ? s.size() : e + 1;
        return std::strtol(tok.c_str(), nullptr, 10);
    };
    size_t pa = 0, pb = 0;
    while (pa < a.size() || pb < b.size())
    {
        long va = (pa < a.size()) ? next(a, pa) : 0;
        long vb = (pb < b.size()) ? next(b, pb) : 0;
        if (va != vb) return (va > vb) ? 1 : -1;
    }
    return 0;
}

} // namespace

VersionCheck::~VersionCheck()
{
    if (thread_.joinable())
        thread_.join();
}

void VersionCheck::start(const std::string& slug, const std::string& localVersion)
{
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true))
        return;
    state_.store(Checking);
    thread_ = std::thread(&VersionCheck::run, this, slug, localVersion);
}

std::string VersionCheck::latestVersion() { std::lock_guard<std::mutex> lk(mtx_); return latest_; }
std::string VersionCheck::productUrl() { std::lock_guard<std::mutex> lk(mtx_); return url_; }
std::string VersionCheck::error() { std::lock_guard<std::mutex> lk(mtx_); return err_; }

#if defined(_WIN32)
void VersionCheck::run(std::string slug, std::string localVersion)
{
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(mtx_);
        err_ = e;
        state_.store(Error);
    };

    std::wstring path = L"/api/version.php?slug=";
    for (char c : slug) path += (wchar_t)c;

    HINTERNET hS = WinHttpOpen(L"InmarScope",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) { fail("WinHttpOpen failed"); return; }
    WinHttpSetTimeouts(hS, 8000, 8000, 8000, 8000);

    HINTERNET hC = WinHttpConnect(hS, L"sarahsforge.dev", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); fail("connect failed"); return; }

    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); fail("open request failed"); return; }

    std::string body;
    bool ok = WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hR, nullptr);
    if (ok)
    {
        DWORD avail = 0;
        do
        {
            avail = 0;
            if (!WinHttpQueryDataAvailable(hR, &avail) || avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hR, buf.data(), avail, &read) || read == 0) break;
            body.append(buf.data(), read);
        } while (avail > 0);
    }
    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);

    if (!ok || body.empty()) { fail("request failed"); return; }

    std::string latest = jsonString(body, "version");
    std::string url = jsonString(body, "product_url");
    bool flag = jsonBoolTrue(body, "update_available");

    bool update = flag || (!latest.empty() && cmpVersion(latest, localVersion) > 0);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        latest_ = latest;
        url_ = url;
    }
    state_.store(update ? UpdateAvailable : UpToDate);
}
#else
void VersionCheck::run(std::string, std::string)
{
    std::lock_guard<std::mutex> lk(mtx_);
    err_ = "unsupported platform";
    state_.store(Error);
}
#endif
