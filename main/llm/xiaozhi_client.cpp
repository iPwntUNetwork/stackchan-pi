#include "xiaozhi_client.h"
#include "../config_store.h"
#include "../hal/audio_io.h"
#include "../hal/opus_codec.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstring>
#include <cstdlib>

static std::string wifiMac() {
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[20] = "";
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

XiaozhiClient xz;
static const char* TAG = "xz";

bool XiaozhiClient::connect(const char* url, const char* token) {
    if (_ws) disconnect();

    // stable per-device client id (uuid stored in nvs)
    if (_clientId.empty()) {
        nvs_handle_t h;
        if (nvs_open("xz", NVS_READWRITE, &h) == ESP_OK) {
            char buf[40] = "";
            size_t len = sizeof(buf);
            if (nvs_get_str(h, "cid", buf, &len) != ESP_OK || !buf[0]) {
                uint8_t r[16];
                esp_fill_random(r, sizeof(r));
                for (int i = 0; i < 16; i++) snprintf(buf + strlen(buf), 3, "%02x", r[i]);
                nvs_set_str(h, "cid", buf);
                nvs_commit(h);
            }
            nvs_close(h);
            _clientId = buf;
        }
    }

    static std::string sHeaders;
    sHeaders = std::string("Authorization: Bearer ") + token +
               "\r\nProtocol-Version: 1\r\nDevice-Id: " + wifiMac() +
               "\r\nClient-Id: " + _clientId + "\r\n";
    esp_websocket_client_config_t wc = {};
    static std::string sUri;
    sUri = url;
    // allow "wss://host:port/path" and "ws://..."
    wc.uri = sUri.c_str();
    wc.buffer_size = 4096;
    wc.reconnect_timeout_ms = 5000;
    wc.network_timeout_ms = 10000;
    wc.headers = sHeaders.c_str();
    if (sUri.rfind("wss://", 0) == 0) {
        wc.crt_bundle_attach = esp_crt_bundle_attach;
    }
    _ws = esp_websocket_client_init(&wc);
    if (!_ws) {
        ESP_LOGE(TAG, "ws init failed");
        _state = State::Error;
        return false;
    }
    esp_websocket_register_events((esp_websocket_client_handle_t)_ws,
                                  WEBSOCKET_EVENT_ANY, xzWsEventHandler, (void*)_ws);
    if (esp_websocket_client_start((esp_websocket_client_handle_t)_ws) != ESP_OK) {
        _state = State::Error;
        return false;
    }
    _state = State::Connecting;
    return true;
}

void XiaozhiClient::disconnect() {
    if (_ws) {
        esp_websocket_client_stop((esp_websocket_client_handle_t)_ws);
        esp_websocket_client_destroy((esp_websocket_client_handle_t)_ws);
        _ws = nullptr;
    }
    _listening = false;
    _state = State::Off;
}

void XiaozhiClient::sendJson(const std::string& s) {
    if (!_ws || _state == State::Off) return;
    esp_websocket_client_send_text((esp_websocket_client_handle_t)_ws,
                                   s.c_str(), s.size(), pdMS_TO_TICKS(1000));
}

void XiaozhiClient::startListening() {
    if (_state.load() < State::Ready) return;
    audio.stopPlayback();
    _listening = true;
    setState(State::Listening);
    sendJson("{\"session_id\":\"\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}");
}

void XiaozhiClient::stopListening() {
    if (!_listening) return;
    _listening = false;
    setState(State::Ready);
    sendJson("{\"session_id\":\"\",\"type\":\"listen\",\"state\":\"stop\"}");
}

void XiaozhiClient::sendWakeWord(const char* text) {
    sendJson("{\"session_id\":\"\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" +
             std::string(text) + "\"}");
}

void XiaozhiClient::sendMicFrame(const int16_t* pcm, size_t samples) {
    if (!_listening || !wsHandle()) return;
    static uint8_t pkt[512];
    static bool encReady = opusEncInit();
    if (!encReady) return;
    if (samples != 960) return;
    int n = opusEncode(pcm, samples, pkt, sizeof(pkt));
    if (n > 0 && _ws) {
        esp_websocket_client_send_bin((esp_websocket_client_handle_t)_ws,
                                      (const char*)pkt, n, pdMS_TO_TICKS(500));
    }
}

void XiaozhiClient::handleText(const char* payload, size_t len) {
    cJSON* jd = cJSON_ParseWithLength(payload, len);
    if (!jd) return;
    cJSON* type = cJSON_GetObjectItem(jd, "type");
    std::string t = type && cJSON_IsString(type) ? type->valuestring : "";
    if (t == "hello") {
        cJSON* ap = cJSON_GetObjectItem(jd, "audio_params");
        cJSON* sr = ap ? cJSON_GetObjectItem(ap, "sample_rate") : nullptr;
        if (sr && cJSON_IsNumber(sr)) _serverRate = (int)sr->valuedouble;
        opusDecInit(_serverRate.load());
        setState(State::Ready);
        ESP_LOGI(TAG, "hello, server rate %d", _serverRate.load());
    } else if (t == "stt") {
        cJSON* x = cJSON_GetObjectItem(jd, "text");
        if (onTranscript && x && cJSON_IsString(x)) onTranscript(x->valuestring);
    } else if (t == "llm") {
        cJSON* x = cJSON_GetObjectItem(jd, "text");
        cJSON* emo = cJSON_GetObjectItem(jd, "emotion");
        if (onAssistantText && x && cJSON_IsString(x) && x->valuestring[0])
            onAssistantText(x->valuestring);
        if (onEmotion && emo && cJSON_IsString(emo)) onEmotion(emo->valuestring);
    } else if (t == "tts") {
        cJSON* st = cJSON_GetObjectItem(jd, "state");
        std::string s = st && cJSON_IsString(st) ? st->valuestring : "";
        if (s == "start") {
            setState(State::ServerSpeaking);
            if (onSpeaking) onSpeaking(true);
        } else if (s == "stop") {
            setState(State::Ready);
            if (onSpeaking) onSpeaking(false);
        }
    } else if (t == "system") {
        cJSON* c = cJSON_GetObjectItem(jd, "command");
        std::string cmd = c && cJSON_IsString(c) ? c->valuestring : "";
        if (cmd == "reboot" || cmd == "update") esp_restart();
    }
    cJSON_Delete(jd);
}

void XiaozhiClient::handleBinary(uint8_t* payload, size_t len) {
    if (_state.load() != State::ServerSpeaking) return;
    static int16_t* pcm = nullptr;
    if (!pcm) pcm = (int16_t*)heap_caps_malloc(11520, MALLOC_CAP_SPIRAM);   // 120 ms @24k
    if (!pcm) return;
    int n = opusDecode(payload, len, pcm, 5760);
    if (n > 0) audio.streamFeed(pcm, n);
}

// ---- esp_event glue ----

void xzWsEventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData) {
    esp_websocket_event_data_t* e = (esp_websocket_event_data_t*)eventData;
    switch (eventId) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ws connected, sending hello");
            {
                const char* hello =
                    "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
                    "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                    "\"channels\":1,\"frame_duration\":60}}";
                esp_websocket_client_send_text((esp_websocket_client_handle_t)handlerArgs,
                                               hello, strlen(hello), pdMS_TO_TICKS(1000));
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (e && e->data_len > 0 && e->data_ptr) {
                if (e->op_code == 0x1) {
                    xz.handleText(e->data_ptr, (size_t)e->data_len);
                } else if (e->op_code == 0x2) {
                    xz.handleBinary((uint8_t*)e->data_ptr, (size_t)e->data_len);
                }
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "ws disconnected");
            if (xz.onError) xz.onError("disconnected");
            break;
        case WEBSOCKET_EVENT_ERROR:
            if (xz.onError) xz.onError("ws error");
            break;
        default:
            break;
    }
}
