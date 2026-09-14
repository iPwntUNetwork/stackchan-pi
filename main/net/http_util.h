#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// esp_http_client wrapper: JSON POST (collect), SSE line streaming,
// binary download (PSRAM), multipart upload, GET.
namespace http {

struct Header { std::string k, v; };
typedef std::vector<Header> Headers;
typedef std::function<bool(const std::string& line)> LineCb;

int postJson(const char* url, const Headers& headers, const std::string& body,
             std::string& response, uint32_t timeoutMs = 30000, size_t maxResp = 300000);

int postStream(const char* url, const Headers& headers, const std::string& body,
               LineCb onLine, uint32_t timeoutMs = 60000);

int postMultipart(const char* url, const Headers& headers, const char* boundary,
                  const std::string& fields, const char* fileField, const char* filename,
                  const char* mime, const uint8_t* data, size_t dataLen,
                  std::string& response, uint32_t timeoutMs = 45000);

int postBinary(const char* url, const Headers& headers, const std::string& body,
               uint8_t** out, size_t* outLen, uint32_t timeoutMs = 45000,
               size_t maxOut = 900000);

int get(const char* url, const Headers& headers, std::string& response,
        uint32_t timeoutMs = 8000);

bool resolveHost(const char* name, std::string& outIp);   // dns + mdns fallback

}  // namespace http
