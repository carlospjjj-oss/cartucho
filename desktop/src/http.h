// Cartucho PC - HTTPS requests (WinHTTP on Windows, libcurl on Linux).
// Blocking: call off the UI thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cartucho {

struct HttpResult {
    int status = 0;          // 0: could not connect
    std::vector<uint8_t> body;
    bool truncated = false;  // bigger than max_bytes
    std::string text() const { return std::string(body.begin(), body.end()); }
    bool ok() const { return status >= 200 && status < 300 && !truncated; }
};

HttpResult HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                       const std::vector<std::string>& headers, int timeout_ms = 20000, size_t max_bytes = 0);
HttpResult HttpGet(const std::string& url, int timeout_ms = 20000, size_t max_bytes = 0);
std::string UrlEncode(const std::string& s);

}  // namespace cartucho
