#include "http_util.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <mdns.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include <esp_heap_caps.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>
#include <cctype>
#include <cstring>

namespace http {

static const char* TAG = "http";

bool resolveHost(const char* name, std::string& outIp) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &res) == 0 && res) {
        char ip[20];
        struct sockaddr_in* a = (struct sockaddr_in*)res->ai_addr;
        strlcpy(ip, inet_ntoa(a->sin_addr), sizeof(ip));
        freeaddrinfo(res);
        outIp = ip;
        return true;
    }
    // mDNS fallback for *.local
    std::string n(name);
    if (n.size() > 6 && n.rfind(".local") == n.size() - 6) {
        esp_ip4_addr_t ip;
        n.erase(n.size() - 6);
        if (mdns_query_a(n.c_str(), 3000, &ip) == ESP_OK) {
            char s[20];
            snprintf(s, sizeof(s), IPSTR, IP2STR(&ip));
            outIp = s;
            return true;
        }
    }
    return false;
}

// shared body reader: cb per byte chunk; returns status code, -1 on transport err
static int perform(esp_http_client_handle_t cli, const std::string* body,
                   std::function<bool(const uint8_t*, int)> onChunk,
                   size_t maxBytes) {
    int writeLen = body ? (int)body->size() : 0;
    esp_err_t err = esp_http_client_open(cli, writeLen);
    if (err != ESP_OK) return -1;
    if (writeLen && body) {
        size_t off = 0;
        while (off < body->size()) {
            int w = esp_http_client_write(cli, body->data() + off, (int)(body->size() - off));
            if (w <= 0) { esp_http_client_close(cli); return -1; }
            off += w;
        }
    }
    int64_t hdrLen = esp_http_client_fetch_headers(cli);
    if (hdrLen < 0) { esp_http_client_close(cli); return -1; }
    int status = esp_http_client_get_status_code(cli);
    if (onChunk) {
        uint8_t buf[1024];
        size_t total = 0;
        while (true) {
            int n = esp_http_client_read(cli, (char*)buf, sizeof(buf));
            if (n < 0) break;
            if (n == 0) {
                if (!esp_http_client_is_complete_data_received(cli)) break;
                break;
            }
            total += n;
            if (!onChunk(buf, n)) break;
            if (total >= maxBytes) break;
        }
    }
    esp_http_client_close(cli);
    return status;
}

static esp_http_client_handle_t makeClient(const char* url, uint32_t timeoutMs) {
    esp_http_client_config_t c = {};
    c.url = url;
    c.timeout_ms = timeoutMs;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.buffer_size = 4096;
    c.buffer_size_tx = 2048;
    c.keep_alive_enable = false;
    return esp_http_client_init(&c);
}

int postJson(const char* url, const Headers& headers, const std::string& body,
             std::string& response, uint32_t timeoutMs, size_t maxResp) {
    esp_http_client_handle_t cli = makeClient(url, timeoutMs);
    if (!cli) return -1;
    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    for (auto& h : headers) esp_http_client_set_header(cli, h.k.c_str(), h.v.c_str());
    response.clear();
    response.reserve(2048);
    int code = perform(cli, &body, [&](const uint8_t* d, int n) {
        response.append((const char*)d, n);
        return response.size() < maxResp;
    }, maxResp);
    esp_http_client_cleanup(cli);
    return code;
}

int postStream(const char* url, const Headers& headers, const std::string& body,
               LineCb onLine, uint32_t timeoutMs) {
    esp_http_client_handle_t cli = makeClient(url, timeoutMs);
    if (!cli) return -1;
    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    for (auto& h : headers) esp_http_client_set_header(cli, h.k.c_str(), h.v.c_str());
    std::string lineBuf;
    lineBuf.reserve(4096);
    int code = perform(cli, &body, [&](const uint8_t* d, int n) {
        for (int i = 0; i < n; i++) {
            if (d[i] == '\n') {
                if (!lineBuf.empty() && lineBuf.back() == '\r') lineBuf.pop_back();
                if (!lineBuf.empty() && !onLine(lineBuf)) return false;
                lineBuf.clear();
            } else if (lineBuf.size() < 16384) {
                lineBuf += (char)d[i];
            }
        }
        return true;
    }, 8000000);
    if (code == 200 && !lineBuf.empty()) onLine(lineBuf);
    esp_http_client_cleanup(cli);
    return code;
}

int postBinary(const char* url, const Headers& headers, const std::string& body,
               uint8_t** out, size_t* outLen, uint32_t timeoutMs, size_t maxOut) {
    *out = nullptr; *outLen = 0;
    esp_http_client_handle_t cli = makeClient(url, timeoutMs);
    if (!cli) return -1;
    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    for (auto& h : headers) esp_http_client_set_header(cli, h.k.c_str(), h.v.c_str());
    uint8_t* buf = (uint8_t*)heap_caps_malloc(maxOut, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(cli); return -2; }
    size_t n = 0;
    int code = perform(cli, &body, [&](const uint8_t* d, int got) {
        if (n + got > maxOut) return false;
        memcpy(buf + n, d, got);
        n += got;
        return true;
    }, maxOut);
    esp_http_client_cleanup(cli);
    if (code == 200 && n > 0) { *out = buf; *outLen = n; }
    else free(buf);
    return code;
}

int postMultipart(const char* url, const Headers& headers, const char* boundary,
                  const std::string& fields, const char* fileField, const char* filename,
                  const char* mime, const uint8_t* data, size_t dataLen,
                  std::string& response, uint32_t timeoutMs) {
    esp_http_client_handle_t cli = makeClient(url, timeoutMs);
    if (!cli) return -1;
    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type",
                               (std::string("multipart/form-data; boundary=") + boundary).c_str());
    for (auto& h : headers) esp_http_client_set_header(cli, h.k.c_str(), h.v.c_str());

    std::string pre = "--" + std::string(boundary) + "\r\n" + fields +
                      "Content-Disposition: form-data; name=\"" + fileField +
                      "\"; filename=\"" + filename + "\"\r\nContent-Type: " + mime + "\r\n\r\n";
    std::string post = "\r\n--" + std::string(boundary) + "--\r\n";

    response.clear();
    int code = -1;
    esp_err_t err = esp_http_client_open(cli, (int)(pre.size() + dataLen + post.size()));
    if (err == ESP_OK) {
        if (esp_http_client_write(cli, pre.data(), (int)pre.size()) > 0) {
            const size_t CH = 4096;
            for (size_t off = 0; off < dataLen; off += CH) {
                size_t n = dataLen - off > CH ? CH : dataLen - off;
                if (esp_http_client_write(cli, (const char*)data + off, (int)n) <= 0) break;
            }
            esp_http_client_write(cli, post.data(), (int)post.size());
        }
        if (esp_http_client_fetch_headers(cli) >= 0) {
            code = esp_http_client_get_status_code(cli);
            uint8_t buf[1024];
            while (response.size() < 65536) {
                int n = esp_http_client_read(cli, (char*)buf, sizeof(buf));
                if (n < 0) break;
                if (n == 0) break;
                response.append((const char*)buf, n);
            }
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return code;
}

int get(const char* url, const Headers& headers, std::string& response, uint32_t timeoutMs) {
    esp_http_client_handle_t cli = makeClient(url, timeoutMs);
    if (!cli) return -1;
    esp_http_client_set_method(cli, HTTP_METHOD_GET);
    for (auto& h : headers) esp_http_client_set_header(cli, h.k.c_str(), h.v.c_str());
    response.clear();
    int code = perform(cli, nullptr, [&](const uint8_t* d, int n) {
        response.append((const char*)d, n);
        return response.size() < 65536;
    }, 65536);
    esp_http_client_cleanup(cli);
    return code;
}

}  // namespace http
