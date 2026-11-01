#include "http.h"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace cartucho {

#ifdef _WIN32

namespace {
std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
}  // namespace

HttpResult HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                       const std::vector<std::string>& headers, int timeout_ms, size_t max_bytes) {
    HttpResult r;
    std::wstring wurl = W(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    wchar_t extra[4096] = {0};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
        return r;
    HINTERNET s = WinHttpOpen(L"Cartucho", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(L"Cartucho", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        return r;
    WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    std::wstring full = std::wstring(path) + extra;
    HINTERNET q = c ? WinHttpOpenRequest(c, W(method).c_str(), full.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (q) {
        DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(q, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
        std::wstring hdrs;
        bool agent = false;
        for (const auto& h : headers) {
            hdrs += W(h) + L"\r\n";
            agent = agent || h.rfind("User-Agent:", 0) == 0;
        }
        if (!agent) hdrs += L"User-Agent: Cartucho\r\n";
        if (WinHttpSendRequest(q, hdrs.c_str(), static_cast<DWORD>(-1L), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(q, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            r.status = static_cast<int>(code);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(q, &avail) || avail == 0)
                    break;
                const size_t at = r.body.size();
                r.body.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(q, r.body.data() + at, avail, &got)) {
                    r.body.resize(at);
                    break;
                }
                r.body.resize(at + got);
                if (max_bytes && r.body.size() > max_bytes) {
                    r.truncated = true;
                    break;
                }
            }
        }
        WinHttpCloseHandle(q);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return r;
}

#else

// Linux (test builds): libcurl, loaded at run time so no headers are needed.
namespace {
using CURL = void;
struct Curl {
    void* lib = nullptr;
    CURL* (*init)() = nullptr;
    int (*setopt)(CURL*, int, ...) = nullptr;
    int (*perform)(CURL*) = nullptr;
    int (*getinfo)(CURL*, int, ...) = nullptr;
    void (*cleanup)(CURL*) = nullptr;
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
    bool ok = false;
    Curl() {
        for (const char* n : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"}) {
            lib = dlopen(n, RTLD_NOW);
            if (lib) break;
        }
        if (!lib) return;
        init = reinterpret_cast<decltype(init)>(dlsym(lib, "curl_easy_init"));
        setopt = reinterpret_cast<decltype(setopt)>(dlsym(lib, "curl_easy_setopt"));
        perform = reinterpret_cast<decltype(perform)>(dlsym(lib, "curl_easy_perform"));
        getinfo = reinterpret_cast<decltype(getinfo)>(dlsym(lib, "curl_easy_getinfo"));
        cleanup = reinterpret_cast<decltype(cleanup)>(dlsym(lib, "curl_easy_cleanup"));
        slist_append = reinterpret_cast<decltype(slist_append)>(dlsym(lib, "curl_slist_append"));
        slist_free_all = reinterpret_cast<decltype(slist_free_all)>(dlsym(lib, "curl_slist_free_all"));
        ok = init && setopt && perform && getinfo && cleanup && slist_append && slist_free_all;
    }
};
// The libcurl option numbers used.
constexpr int kUrl = 10002, kWriteFunction = 20011, kWriteData = 10001, kFollow = 52, kTimeoutMs = 155,
              kUserAgent = 10018, kHttpHeader = 10023, kPostFields = 10015, kPostSize = 60, kCustomRequest = 10036,
              kNoSignal = 99, kResponseCode = 0x200002;
struct Sink { std::vector<uint8_t>* out; size_t max; bool trunc; };
size_t Write(char* p, size_t s, size_t n, void* u) {
    auto* k = static_cast<Sink*>(u);
    k->out->insert(k->out->end(), p, p + s * n);
    if (k->max && k->out->size() > k->max) { k->trunc = true; return 0; }
    return s * n;
}
}  // namespace

HttpResult HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                       const std::vector<std::string>& headers, int timeout_ms, size_t max_bytes) {
    static Curl curl;
    HttpResult r;
    if (!curl.ok) return r;
    CURL* h = curl.init();
    if (!h) return r;
    Sink sink{&r.body, max_bytes, false};
    void* hl = nullptr;
    for (const auto& x : headers) hl = curl.slist_append(hl, x.c_str());
    curl.setopt(h, kUrl, url.c_str());
    curl.setopt(h, kWriteFunction, &Write);
    curl.setopt(h, kWriteData, &sink);
    curl.setopt(h, kFollow, 1L);
    curl.setopt(h, kTimeoutMs, static_cast<long>(timeout_ms));
    curl.setopt(h, kNoSignal, 1L);
    curl.setopt(h, kUserAgent, "Cartucho");
    if (hl) curl.setopt(h, kHttpHeader, hl);
    if (method != "GET") {
        curl.setopt(h, kCustomRequest, method.c_str());
        curl.setopt(h, kPostFields, body.c_str());
        curl.setopt(h, kPostSize, static_cast<long>(body.size()));
    }
    curl.perform(h);
    long code = 0;
    curl.getinfo(h, kResponseCode, &code);
    r.status = static_cast<int>(code);
    r.truncated = sink.trunc;
    if (hl) curl.slist_free_all(hl);
    curl.cleanup(h);
    return r;
}

#endif

HttpResult HttpGet(const std::string& url, int timeout_ms, size_t max_bytes) {
    return HttpRequest("GET", url, "", {}, timeout_ms, max_bytes);
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

}  // namespace cartucho
