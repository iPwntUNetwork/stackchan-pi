#include "web_app.h"
#include "wifi_mgr.h"
#include "http_util.h"
#include "pi_link.h"
#include "config_store.h"
#include "hal/motion.h"
#include "hal/leds.h"
#include "hal/camera_feed.h"
#include "hal/audio_io.h"
#include "hal/ir_rmt.h"
#include "hal/nfc_pn532.h"
#include "hal/head_touch.h"
#include "llm/llm_client.h"
#include "llm/chat_engine.h"
#include <M5Unified.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include <mdns.h>
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <string>

WebApp webApp;
static const char* TAG = "web";
static httpd_handle_t s_httpd = nullptr;
extern const char UI_HTML[] asm("_binary_webui_html_start");   // embedded via EMBED_FILES
extern const char UI_HTML_END[] asm("_binary_webui_html_end");

// ---------------- helpers ----------------

static void sendJson(httpd_req_t* req, const std::string& s, int code = 200) {
    httpd_resp_set_type(req, "application/json");
    if (code != 200) httpd_resp_set_status(req, (std::to_string(code) + " Error").c_str());
    httpd_resp_send(req, s.c_str(), (int)s.size());
}

static std::string bodyOf(httpd_req_t* req) {
    int len = req->content_len;
    if (len <= 0 || len > 65536) return "";
    std::string body;
    body.resize(len);
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body.data() + got, len - got);
        if (n <= 0) break;
        got += n;
    }
    body.resize(got);
    return body;
}

static cJSON* jsonBody(httpd_req_t* req) {
    std::string body = bodyOf(req);
    if (body.empty()) return nullptr;
    return cJSON_Parse(body.c_str());
}

// ---------------- handlers ----------------

static esp_err_t hRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, UI_HTML, UI_HTML_END - UI_HTML);   // embedded blob has no NUL
}

static void queryDecode(const char* v, char* dst, size_t cap) {
    std::string out;
    for (size_t i = 0; v && v[i]; i++) {
        if (v[i] == '+') out += ' ';
        else if (v[i] == '%' && v[i + 1] && v[i + 2]) {
            char hex[3] = {v[i + 1], v[i + 2], 0};
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else out += v[i];
    }
    strlcpy(dst, out.c_str(), cap);
}

static esp_err_t hConnect(httpd_req_t* req) {
    char ssid[40] = "", pass[70] = "";
    char query[160] = "";
    bool haveQuery = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    if (haveQuery) {
        char v[80] = "";
        if (httpd_query_key_value(query, "ssid", v, sizeof(v)) == ESP_OK) queryDecode(v, ssid, sizeof(ssid));
        v[0] = 0;
        if (httpd_query_key_value(query, "pass", v, sizeof(v)) == ESP_OK) queryDecode(v, pass, sizeof(pass));
    }
    if (!ssid[0]) {
        std::string b = bodyOf(req);
        // form POST (application/x-www-form-urlencoded)
        auto extract = [&](const char* key, char* dst, size_t cap) {
            size_t p = b.find(std::string(key) + "=");
            if (p == std::string::npos) return;
            p += strlen(key) + 1;
            size_t e = b.find('&', p);
            if (e == std::string::npos) e = b.size();
            std::string v = b.substr(p, e - p);
            // urldecode (+ and %xx)
            std::string out;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '+') out += ' ';
                else if (v[i] == '%' && i + 2 < v.size()) {
                    char hex[3] = {v[i + 1], v[i + 2], 0};
                    out += (char)strtol(hex, nullptr, 16);
                    i += 2;
                } else out += v[i];
            }
            strlcpy(dst, out.c_str(), cap);
        };
        extract("ssid", ssid, sizeof(ssid));
        extract("pass", pass, sizeof(pass));
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<html><body style='font-family:sans-serif;background:#111;color:#eee'>"
                         "<h2>Connecting... the robot will join your network in a moment.</h2></body></html>",
                    HTTPD_RESP_USE_STRLEN);
    if (ssid[0]) {
        wifi.applyCredentials(ssid, pass);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t hStream(httpd_req_t* req) {
    esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    if (res != ESP_OK) return res;
    uint8_t* jpeg = nullptr;
    size_t len = 0;
    int fail = 0;
    while (fail < 10) {
        if (!camera.getJpeg(&jpeg, &len, 12)) {
            fail++;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        fail = 0;
        std::string head = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                           std::to_string(len) + "\r\n\r\n";
        if (httpd_resp_send_chunk(req, head.c_str(), head.size()) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, (const char*)jpeg, len) != ESP_OK) { free(jpeg); break; }
        free(jpeg);
        if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t hCapture(httpd_req_t* req) {
    uint8_t* jpeg = nullptr;
    size_t len = 0;
    if (camera.getJpeg(&jpeg, &len, 12)) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_send(req, (const char*)jpeg, len);
        free(jpeg);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera offline");
    return ESP_OK;
}

static esp_err_t hStatus(httpd_req_t* req) {
    float yaw, pitch;
    motion.getYawPitch(yaw, pitch);
    cJSON* d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "ip", wifi.ip().c_str());
    cJSON_AddNumberToObject(d, "rssi", wifi.rssi());
    cJSON_AddNumberToObject(d, "heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(d, "psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(d, "battery", M5.Power.getBatteryLevel());
    cJSON_AddNumberToObject(d, "provider", cfg.provider);
    cJSON_AddStringToObject(d, "status", chat.statusText().c_str());
    cJSON_AddNumberToObject(d, "servo_backend", (int)motion.backend());
    cJSON_AddNumberToObject(d, "yaw", yaw);
    cJSON_AddNumberToObject(d, "pitch", pitch);
    cJSON_AddNumberToObject(d, "cam_fps", camera.fps());
    cJSON_AddStringToObject(d, "pi_host", cfg.pi_host);
    cJSON_AddBoolToObject(d, "pi_ttyd", pi.terminalConnected());
    cJSON_AddBoolToObject(d, "nfc", nfc.present());
    cJSON_AddBoolToObject(d, "head_touch", headTouch.present());
    cJSON_AddStringToObject(d, "tz", cfg.tz);
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(d, "version", app->version);
    char* s = cJSON_PrintUnformatted(d);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(d);
    sendJson(req, out);
    return ESP_OK;
}

static esp_err_t hConfigGet(httpd_req_t* req) {
    sendJson(req, cfg.toJson(true));
    return ESP_OK;
}

static esp_err_t hConfigPost(httpd_req_t* req) {
    cJSON* d = jsonBody(req);
    if (!d) {
        sendJson(req, "{\"error\":\"bad json\"}", 400);
        return ESP_OK;
    }
    std::string oldSsid(cfg.wifi_ssid);
    cfg.fromJson(d);
    cfg.save();
    cJSON_Delete(d);
    audio.setVolume(cfg.volume);
    if (std::string(cfg.wifi_ssid) != oldSsid && cfg.wifi_ssid[0]) {
        sendJson(req, "{\"ok\":true,\"note\":\"wifi changed; rebooting\"}");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    } else {
        sendJson(req, "{\"ok\":true}");
    }
    return ESP_OK;
}

static esp_err_t hLook(httpd_req_t* req) {
    cJSON* d = jsonBody(req);
    if (!d) {
        sendJson(req, "{\"error\":\"bad json\"}", 400);
        return ESP_OK;
    }
    cJSON* y = cJSON_GetObjectItem(d, "yaw");
    cJSON* p = cJSON_GetObjectItem(d, "pitch");
    motion.setTarget(y ? (float)y->valuedouble : cfg.home_yaw,
                     p ? (float)p->valuedouble : (float)cfg.home_pitch);
    motion.idleKick();
    cJSON_Delete(d);
    sendJson(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t hSay(httpd_req_t* req) {
    cJSON* d = jsonBody(req);
    if (!d) return sendJson(req, "{\"error\":\"bad json\"}", 400), ESP_OK;
    cJSON* t = cJSON_GetObjectItem(d, "text");
    std::string text = t && cJSON_IsString(t) ? t->valuestring : "";
    cJSON_Delete(d);
    if (text.empty()) return sendJson(req, "{\"error\":\"no text\"}", 400), ESP_OK;
    LlmClient* llm = (cfg.provider == 1) ? geminiClient() : openAiClient();
    size_t len = 0;
    uint8_t* wav = llm->tts(text, len);
    if (wav) {
        audio.playWav(wav, len);
        free(wav);
        sendJson(req, "{\"ok\":true}");
    } else {
        sendJson(req, "{\"ok\":false,\"note\":\"tts unavailable\"}", 502);
    }
    return ESP_OK;
}

static esp_err_t hChat(httpd_req_t* req) {
    cJSON* d = jsonBody(req);
    if (!d) return sendJson(req, "{\"error\":\"bad json\"}", 400), ESP_OK;
    cJSON* t = cJSON_GetObjectItem(d, "text");
    std::string text = t && cJSON_IsString(t) ? t->valuestring : "";
    cJSON* ph = cJSON_GetObjectItem(d, "photo");
    bool photo = ph && cJSON_IsTrue(ph);
    cJSON_Delete(d);
    if (text.empty()) return sendJson(req, "{\"error\":\"no text\"}", 400), ESP_OK;
    chat.submitText(text, photo);
    sendJson(req, "{\"queued\":true}");
    return ESP_OK;
}

static esp_err_t hChatLast(httpd_req_t* req) {
    cJSON* d = cJSON_CreateObject();
    chat.lockHist();
    cJSON* h = cJSON_AddArrayToObject(d, "history");
    for (auto& m : chat.history.msgs) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", m.role.c_str());
        cJSON_AddStringToObject(o, "text", m.text.c_str());
        cJSON_AddItemToArray(h, o);
    }
    chat.unlockHist();
    cJSON_AddStringToObject(d, "status", chat.statusText().c_str());
    cJSON_AddStringToObject(d, "partial", chat.partial().c_str());
    char* s = cJSON_PrintUnformatted(d);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(d);
    sendJson(req, out);
    return ESP_OK;
}

static esp_err_t hPtt(httpd_req_t* req) {
    std::string body = bodyOf(req);
    chat.xzPushToTalk(body == "down");
    sendJson(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t hPiExec(httpd_req_t* req) {
    cJSON* d = jsonBody(req);
    if (!d) return sendJson(req, "{\"error\":\"bad json\"}", 400), ESP_OK;
    cJSON* c = cJSON_GetObjectItem(d, "cmd");
    std::string out = pi.agentExec(c && cJSON_IsString(c) ? c->valuestring : "uname -a");
    cJSON_Delete(d);
    sendJson(req, out);
    return ESP_OK;
}

static esp_err_t hPiStatus(httpd_req_t* req) {
    sendJson(req, pi.agentStatus());
    return ESP_OK;
}

static esp_err_t hIr(httpd_req_t* req) {
    std::string body = bodyOf(req);
    if (body == "learn") {
        bool ok = irRmt.startLearn();
        sendJson(req, ok ? "{\"learning\":true}" : "{\"error\":\"no rx\"}");
        return ESP_OK;
    }
    cJSON* d = jsonBody(req);
    if (!d) return sendJson(req, "{\"error\":\"bad json\"}", 400), ESP_OK;
    cJSON* v = cJSON_GetObjectItem(d, "value");
    cJSON* b = cJSON_GetObjectItem(d, "bits");
    cJSON* raw = cJSON_GetObjectItem(d, "raw");
    bool ok = false;
    if (raw && cJSON_IsTrue(raw)) ok = irRmt.sendRaw();
    else if (v) ok = irRmt.sendNec((uint64_t)v->valuedouble, b ? (uint8_t)b->valuedouble : 32);
    cJSON_Delete(d);
    sendJson(req, ok ? "{\"sent\":true}" : "{\"error\":\"no tx / nothing learned\"}");
    return ESP_OK;
}

static esp_err_t hIrPoll(httpd_req_t* req) {
    uint64_t value;
    uint8_t bits;
    bool nec, rawReady;
    if (irRmt.pollLearn(value, bits, nec, rawReady)) {
        char buf[96];
        if (nec) snprintf(buf, sizeof(buf), "{\"value\":%" PRIu64 ",\"bits\":%u,\"type\":0}", value, bits);
        else snprintf(buf, sizeof(buf), "{\"raw\":true,\"note\":\"captured; send with raw:true\"}");
        sendJson(req, buf);
    } else {
        sendJson(req, "{\"value\":0}");
    }
    return ESP_OK;
}

static esp_err_t hNfc(httpd_req_t* req) {
    std::string s = std::string("{\"present\":") + (nfc.present() ? "true" : "false") +
                    ",\"uid\":\"" + nfc.uidHex() + "\"}";
    sendJson(req, s);
    return ESP_OK;
}

static esp_err_t hLeds(httpd_req_t* req) {
    cJSON* d = jsonBody(req);
    if (!d) return sendJson(req, "{\"error\":\"bad json\"}", 400), ESP_OK;
    cJSON* e = cJSON_GetObjectItem(d, "effect");
    if (e && cJSON_IsNumber(e)) cfg.led_effect = (uint8_t)e->valuedouble % 3;
    cJSON* r = cJSON_GetObjectItem(d, "r");
    if (r && cJSON_IsNumber(r)) {
        cJSON* g = cJSON_GetObjectItem(d, "g");
        cJSON* b = cJSON_GetObjectItem(d, "b");
        cJSON* ms = cJSON_GetObjectItem(d, "ms");
        leds.overrideColor((uint8_t)r->valuedouble, g ? (uint8_t)g->valuedouble : 0,
                           b ? (uint8_t)b->valuedouble : 0, ms ? (uint32_t)ms->valuedouble : 3000);
    }
    cJSON_Delete(d);
    cfg.save();
    sendJson(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t hServo(httpd_req_t* req) {
    cJSON* d = jsonBody(req);
    if (!d) return sendJson(req, "{\"error\":\"bad json\"}", 400), ESP_OK;
    cJSON* c = cJSON_GetObjectItem(d, "cmd");
    std::string cmd = c && cJSON_IsString(c) ? c->valuestring : "";
    cJSON_Delete(d);
    bool ok = false;
    if (cmd == "center") { ok = motion.center(); motion.idleKick(); }
    else if (cmd == "torque_on") ok = motion.setTorque(true);
    else if (cmd == "torque_off") ok = motion.setTorque(false);
    else if (cmd == "power_on") ok = motion.setPower(true);
    else if (cmd == "power_off") ok = motion.setPower(false);
    sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t hReboot(httpd_req_t* req) {
    sendJson(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// ---- OTA: POST /api/ota with raw .bin body ----

static esp_err_t hOta(httpd_req_t* req) {
    int total = req->content_len;
    if (total <= 0 || total > 3 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_OK;
    }
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_OK;
    }
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_OK;
    }
    char buf[4096];
    int got = 0, remaining = total;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? sizeof(buf) : remaining);
        if (n <= 0) break;
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return ESP_OK;
        }
        got += n;
        remaining -= n;
    }
    if (remaining != 0 || esp_ota_end(ota) != ESP_OK ||
        esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota finish failed");
        return ESP_OK;
    }
    sendJson(req, "{\"ok\":true,\"note\":\"flashed; rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// 404 → captive redirect in AP mode
static esp_err_t err404(httpd_req_t* req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// ---------------- start ----------------

void WebApp::begin() {
    if (_started) return;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 12288;
    hc.max_uri_handlers = 24;
    hc.recv_wait_timeout = 5;
    hc.send_wait_timeout = 5;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t uris[] = {
        {"/", HTTP_GET, hRoot, nullptr},
        {"/connect", HTTP_GET, hConnect, nullptr},
        {"/connect", HTTP_POST, hConnect, nullptr},
        {"/stream", HTTP_GET, hStream, nullptr},
        {"/capture.jpg", HTTP_GET, hCapture, nullptr},
        {"/api/status", HTTP_GET, hStatus, nullptr},
        {"/api/config", HTTP_GET, hConfigGet, nullptr},
        {"/api/config", HTTP_POST, hConfigPost, nullptr},
        {"/api/look", HTTP_POST, hLook, nullptr},
        {"/api/say", HTTP_POST, hSay, nullptr},
        {"/api/chat", HTTP_POST, hChat, nullptr},
        {"/api/chat/last", HTTP_GET, hChatLast, nullptr},
        {"/api/ptt", HTTP_POST, hPtt, nullptr},
        {"/api/pi/exec", HTTP_POST, hPiExec, nullptr},
        {"/api/pi/status", HTTP_GET, hPiStatus, nullptr},
        {"/api/ir", HTTP_POST, hIr, nullptr},
        {"/api/ir/poll", HTTP_GET, hIrPoll, nullptr},
        {"/api/nfc", HTTP_GET, hNfc, nullptr},
        {"/api/leds", HTTP_POST, hLeds, nullptr},
        {"/api/servo", HTTP_POST, hServo, nullptr},
        {"/api/reboot", HTTP_POST, hReboot, nullptr},
        {"/api/ota", HTTP_POST, hOta, nullptr},
    };
    for (auto& u : uris) httpd_register_uri_handler(s_httpd, &u);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, err404);

    mdns_init();
    mdns_hostname_set(HOSTNAME_BASE);
    mdns_instance_name_set("Stack-chan");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);

    _started = true;
    ESP_LOGI(TAG, "web ui on http://%s/ (ota: POST /api/ota)", wifi.ip().c_str());
}
