#include "pi_link.h"
#include "config_store.h"
#include "http_util.h"
#include "hal/motion.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <cstring>
#include <cinttypes>

PiLink pi;
static const char* TAG = "pi";

void PiLink::begin() {
    if (_inited) return;
    _inited = true;
    _mux = xSemaphoreCreateMutex();
    memset(_lines, ' ', sizeof(_lines));
    for (int i = 0; i < MAXLINES; i++) _lines[i][COLS] = 0;
    // ttyd basic auth + AuthToken (pi_token format "user:pass", optional)
    if (strchr(cfg.pi_token, ':')) {
        size_t olen = 0;
        mbedtls_base64_encode(nullptr, 0, &olen, (const uint8_t*)cfg.pi_token,
                              strlen(cfg.pi_token));
        mbedtls_base64_encode((uint8_t*)_b64cred, sizeof(_b64cred), &olen,
                              (const uint8_t*)cfg.pi_token, strlen(cfg.pi_token));
    }
}

char g_piAuthB64[96] = {0};

void PiLink::wsEvent(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData) {
    esp_websocket_event_data_t* e = (esp_websocket_event_data_t*)eventData;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handlerArgs;
    switch (eventId) {
        case WEBSOCKET_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "ttyd connected");
            // ttyd handshake: JSON init (+ AuthToken when secured)
            std::string init = "{\"columns\":" + std::to_string(PiLink::COLS) +
                               ",\"rows\":" + std::to_string(PiLink::ROWS);
            if (g_piAuthB64[0]) init += ",\"AuthToken\":\"" + std::string(g_piAuthB64) + "\"";
            init += "}";
            esp_websocket_client_send_text(client, init.c_str(), init.size(), pdMS_TO_TICKS(1000));
            break;
        }
        case WEBSOCKET_EVENT_DATA:
            // text frame with ttyd output: '0' + base64
            if (e && e->op_code == 0x1 && e->data_len >= 2 && e->data_ptr &&
                e->data_ptr[0] == '0') {
                size_t len = (size_t)e->data_len;
                uint8_t* buf = (uint8_t*)malloc(len * 3 / 4 + 4);
                if (buf) {
                    size_t olen = 0;
                    mbedtls_base64_decode(buf, len * 3 / 4 + 4, &olen,
                                          (const uint8_t*)e->data_ptr + 1, len - 1);
                    if (olen) pi.feedOutput(buf, olen);
                    free(buf);
                }
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "ttyd disconnected");
            break;
        default:
            break;
    }
}

void PiLink::connectTerminal() {
    begin();
    if (_ws) {
        esp_websocket_client_start((esp_websocket_client_handle_t)_ws);
        return;
    }
    strlcpy(g_piAuthB64, _b64cred, sizeof(g_piAuthB64));
    std::string hostIp;
    std::string host = cfg.pi_host;
    http::resolveHost(cfg.pi_host, hostIp);
    if (!hostIp.empty()) host = hostIp;

    esp_websocket_client_config_t wc = {};
    static std::string sUri;
    sUri = "ws://" + host + ":" + std::to_string(cfg.pi_ttyd_port) + "/ws";
    wc.uri = sUri.c_str();
    wc.buffer_size = 4096;
    wc.reconnect_timeout_ms = 4000;
    wc.network_timeout_ms = 8000;
    static std::string sHeaders;
    if (_b64cred[0]) sHeaders = "Authorization: Basic " + std::string(_b64cred) + "\r\n";
    if (!sHeaders.empty()) wc.headers = sHeaders.c_str();
    _ws = esp_websocket_client_init(&wc);
    if (!_ws) { ESP_LOGE(TAG, "ws init failed"); return; }
    esp_websocket_register_events((esp_websocket_client_handle_t)_ws,
                                  WEBSOCKET_EVENT_ANY, wsEvent, (void*)_ws);
    esp_websocket_client_start((esp_websocket_client_handle_t)_ws);
}

void PiLink::disconnectTerminal() {
    if (_ws) esp_websocket_client_stop((esp_websocket_client_handle_t)_ws);
    _connected = false;
}

void PiLink::taskTramp(void* p) { ((PiLink*)p)->taskLoop(); }

void PiLink::taskLoop() {}   // esp_websocket_client runs its own task

void PiLink::sendKeys(const std::string& s) {
    if (!_ws) return;
    std::string pkt = "0" + s;
    esp_websocket_client_send_text((esp_websocket_client_handle_t)_ws,
                                   pkt.c_str(), pkt.size(), pdMS_TO_TICKS(500));
}

void PiLink::sendSpecial(uint8_t key) {
    switch (key) {
        case KeyEnter: sendKeys("\r"); break;
        case KeyTab: sendKeys("\t"); break;
        case KeyCtrlC: sendKeys("\x03"); break;
        case KeyEsc: sendKeys("\x1b"); break;
        case KeyUp: sendKeys("\x1b[A"); break;
        case KeyDown: sendKeys("\x1b[B"); break;
        case KeyRight: sendKeys("\x1b[C"); break;
        case KeyLeft: sendKeys("\x1b[D"); break;
    }
}

void PiLink::copyScreen(char out[ROWS][COLS + 1]) {
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(200)) != pdTRUE) return;
    for (uint8_t r = 0; r < ROWS; r++) {
        uint8_t idx = (_head + (MAXLINES - ROWS) + r) % MAXLINES;
        memcpy(out[r], _lines[idx], COLS + 1);
        for (int c = COLS - 1; c >= 0; c--) {
            if (out[r][c] == ' ') out[r][c] = 0; else break;
        }
    }
    xSemaphoreGive(_mux);
}

void PiLink::commitLine() {   // caller holds _mux
    _head = (_head + 1) % MAXLINES;
    memset(_lines[_head], ' ', COLS);
    _lines[_head][COLS] = 0;
    _col = 0;
    _dirty = true;
}

void PiLink::feedOutput(uint8_t* data, size_t len) {
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(200)) != pdTRUE) return;
    uint8_t cur = (_head + MAXLINES - 1) % MAXLINES;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (_ansi) {
            if (_ansiState == 0 && c == '[') { _ansiState = 1; }
            else if (_ansiState == 1) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { _ansi = false; _ansiState = 0; }
            }
            continue;
        }
        switch (c) {
            case 0x1B: _ansi = true; _ansiState = 0; break;
            case '\n': commitLine(); cur = (_head + MAXLINES - 1) % MAXLINES; break;
            case '\r': _col = 0; break;
            case 0x08: if (_col > 0) _col--; break;
            case '\t': {
                uint8_t next = (_col / 8 + 1) * 8;
                if (next < COLS) _col = next;
                break;
            }
            default:
                if (c >= 32 && c < 127) {
                    if (_col >= COLS) { commitLine(); cur = (_head + MAXLINES - 1) % MAXLINES; }
                    _lines[cur][_col++] = (char)c;
                    _dirty = true;
                }
                break;
        }
    }
    xSemaphoreGive(_mux);
}

// ---- agent REST ----

std::string PiLink::agentUrl(const char* path) {
    std::string host = cfg.pi_host, hostIp;
    if (http::resolveHost(cfg.pi_host, hostIp)) host = hostIp;
    return "http://" + host + ":" + std::to_string(cfg.pi_agent_port) + path;
}

std::string PiLink::agentStatus() {
    std::string resp;
    http::Headers h;
    h.push_back({"X-Agent-Token", cfg.pi_token});
    int code = http::get(agentUrl("/status").c_str(), h, resp);
    if (code == 200) { _lastAgentOk = millis32(); return resp; }
    return "{\"error\":\"http " + std::to_string(code) + "\"}";
}

std::string PiLink::agentExec(const std::string& cmd) {
    std::string body = "{\"cmd\":\"" + cmd + "\"}";
    std::string resp;
    http::Headers h;
    h.push_back({"X-Agent-Token", cfg.pi_token});
    h.push_back({"Content-Type", "application/json"});
    int code = http::postJson(agentUrl("/exec").c_str(), h, body, resp, 15000, 100000);
    if (code == 200) { _lastAgentOk = millis32(); return resp; }
    return "{\"error\":\"http " + std::to_string(code) + "\"}";
}
