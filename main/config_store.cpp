#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <esp_crc.h>
#include <cstring>

static const char* TAG = "cfg";
Config cfg;

void copyJsonStr(char* dst, size_t cap, const cJSON* item) {
    if (!item || !cJSON_IsString(item) || !item->valuestring) return;
    strlcpy(dst, item->valuestring, cap);
}

void copyJsonSecret(char* dst, size_t cap, const cJSON* item) {
    if (!item || !cJSON_IsString(item) || !item->valuestring) return;
    const char* s = item->valuestring;
    if (!s[0]) return;
    if (strstr(s, "***")) return;          // masked value from the UI — keep old
    strlcpy(dst, s, cap);
}

void Config::load() {
    nvs_handle_t h;
    if (nvs_open("stackchan", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(Config);
    if (nvs_get_blob(h, "blob", this, &len) == ESP_OK && len == sizeof(Config)) {
        uint32_t want = 0;
        if (nvs_get_u32(h, "crc", &want) == ESP_OK) {
            uint32_t got = esp_crc32_le(0, (const uint8_t*)this, sizeof(Config));
            if (want != got) {
                ESP_LOGW(TAG, "config crc mismatch — defaults");
                *this = Config();
            }
        }
    }
    nvs_close(h);
}

void Config::save() {
    nvs_handle_t h;
    if (nvs_open("stackchan", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t crc = esp_crc32_le(0, (const uint8_t*)this, sizeof(Config));
    nvs_set_blob(h, "blob", this, sizeof(Config));
    nvs_set_u32(h, "crc", crc);
    nvs_commit(h);
    nvs_close(h);
}

static void jput(cJSON* o, const char* k, const char* v, bool mask) {
    if (mask && v[0]) {
        std::string m(v);
        for (size_t i = 3; i < m.size() && i < 6; i++) m[i] = '*';
        cJSON_AddStringToObject(o, k, m.c_str());
    } else {
        cJSON_AddStringToObject(o, k, v);
    }
}

std::string Config::toJson(bool mask) const {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wifi_ssid", wifi_ssid);
    jput(o, "wifi_pass", wifi_pass, mask);
    cJSON_AddStringToObject(o, "tz", tz);
    cJSON_AddNumberToObject(o, "provider", provider);
    jput(o, "openai_key", openai_key, mask);
    cJSON_AddStringToObject(o, "openai_model", openai_model);
    cJSON_AddStringToObject(o, "openai_base", openai_base);
    cJSON_AddStringToObject(o, "openai_stt", openai_stt);
    cJSON_AddStringToObject(o, "openai_tts", openai_tts);
    cJSON_AddStringToObject(o, "openai_voice", openai_voice);
    jput(o, "gemini_key", gemini_key, mask);
    cJSON_AddStringToObject(o, "gemini_model", gemini_model);
    cJSON_AddBoolToObject(o, "gemini_tts", gemini_tts);
    cJSON_AddStringToObject(o, "gemini_tts_model", gemini_tts_model);
    cJSON_AddStringToObject(o, "gemini_voice", gemini_voice);
    cJSON_AddStringToObject(o, "xz_url", xz_url);
    jput(o, "xz_token", xz_token, mask);
    cJSON_AddStringToObject(o, "persona", persona);
    cJSON_AddStringToObject(o, "pi_host", pi_host);
    cJSON_AddNumberToObject(o, "pi_ttyd_port", pi_ttyd_port);
    cJSON_AddNumberToObject(o, "pi_agent_port", pi_agent_port);
    jput(o, "pi_token", pi_token, mask);
    cJSON_AddNumberToObject(o, "servo_mode", servo_mode);
    cJSON_AddNumberToObject(o, "yaw_zero", yaw_zero);
    cJSON_AddNumberToObject(o, "pitch_zero", pitch_zero);
    cJSON_AddNumberToObject(o, "yaw_min", yaw_min);
    cJSON_AddNumberToObject(o, "yaw_max", yaw_max);
    cJSON_AddNumberToObject(o, "pitch_min", pitch_min);
    cJSON_AddNumberToObject(o, "pitch_max", pitch_max);
    cJSON_AddNumberToObject(o, "pwm_yaw_pin", pwm_yaw_pin);
    cJSON_AddNumberToObject(o, "pwm_pitch_pin", pwm_pitch_pin);
    cJSON_AddNumberToObject(o, "servo_speed", servo_speed);
    cJSON_AddBoolToObject(o, "idle_motion", idle_motion);
    cJSON_AddNumberToObject(o, "home_yaw", home_yaw);
    cJSON_AddNumberToObject(o, "home_pitch", home_pitch);
    cJSON_AddNumberToObject(o, "volume", volume);
    cJSON_AddNumberToObject(o, "led_effect", led_effect);
    cJSON_AddNumberToObject(o, "ir_tx_pin", ir_tx_pin);
    cJSON_AddNumberToObject(o, "ir_rx_pin", ir_rx_pin);
    cJSON_AddBoolToObject(o, "nfc_enable", nfc_enable);
    cJSON_AddNumberToObject(o, "nfc_sda", nfc_sda);
    cJSON_AddNumberToObject(o, "nfc_scl", nfc_scl);
    cJSON_AddNumberToObject(o, "neko_count", neko_count);
    cJSON_AddNumberToObject(o, "neko_pin", neko_pin);
    cJSON_AddBoolToObject(o, "cam_overlay", cam_overlay);
    cJSON_AddBoolToObject(o, "cam_stream", cam_stream);
    char* s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

void Config::fromJson(const cJSON* d) {
    if (!d) return;
    copyJsonStr(wifi_ssid, sizeof(wifi_ssid), cJSON_GetObjectItem(d, "wifi_ssid"));
    copyJsonStr(wifi_pass, sizeof(wifi_pass), cJSON_GetObjectItem(d, "wifi_pass"));
    copyJsonStr(tz, sizeof(tz), cJSON_GetObjectItem(d, "tz"));
    auto geti = [&](const char* k, int& dst, int capMin, int capMax) {
        const cJSON* it = cJSON_GetObjectItem(d, k);
        if (it && cJSON_IsNumber(it)) {
            int v = (int)it->valuedouble;
            if (v < capMin) v = capMin;
            if (v > capMax) v = capMax;
            dst = v;
        }
    };
    int t = provider; geti("provider", t, 0, 2); provider = t;
    copyJsonSecret(openai_key, sizeof(openai_key), cJSON_GetObjectItem(d, "openai_key"));
    copyJsonStr(openai_model, sizeof(openai_model), cJSON_GetObjectItem(d, "openai_model"));
    copyJsonStr(openai_base, sizeof(openai_base), cJSON_GetObjectItem(d, "openai_base"));
    copyJsonStr(openai_stt, sizeof(openai_stt), cJSON_GetObjectItem(d, "openai_stt"));
    copyJsonStr(openai_tts, sizeof(openai_tts), cJSON_GetObjectItem(d, "openai_tts"));
    copyJsonStr(openai_voice, sizeof(openai_voice), cJSON_GetObjectItem(d, "openai_voice"));
    copyJsonSecret(gemini_key, sizeof(gemini_key), cJSON_GetObjectItem(d, "gemini_key"));
    copyJsonStr(gemini_model, sizeof(gemini_model), cJSON_GetObjectItem(d, "gemini_model"));
    const cJSON* gt = cJSON_GetObjectItem(d, "gemini_tts");
    if (gt && cJSON_IsBool(gt)) gemini_tts = cJSON_IsTrue(gt);
    copyJsonStr(gemini_tts_model, sizeof(gemini_tts_model), cJSON_GetObjectItem(d, "gemini_tts_model"));
    copyJsonStr(gemini_voice, sizeof(gemini_voice), cJSON_GetObjectItem(d, "gemini_voice"));
    copyJsonStr(xz_url, sizeof(xz_url), cJSON_GetObjectItem(d, "xz_url"));
    copyJsonSecret(xz_token, sizeof(xz_token), cJSON_GetObjectItem(d, "xz_token"));
    copyJsonStr(persona, sizeof(persona), cJSON_GetObjectItem(d, "persona"));
    copyJsonStr(pi_host, sizeof(pi_host), cJSON_GetObjectItem(d, "pi_host"));
    int tp = pi_ttyd_port; geti("pi_ttyd_port", tp, 1, 65535); pi_ttyd_port = tp;
    tp = pi_agent_port; geti("pi_agent_port", tp, 1, 65535); pi_agent_port = tp;
    copyJsonSecret(pi_token, sizeof(pi_token), cJSON_GetObjectItem(d, "pi_token"));
    t = servo_mode; geti("servo_mode", t, 0, 2); servo_mode = t;
    t = yaw_zero; geti("yaw_zero", t, 0, 1023); yaw_zero = t;
    t = pitch_zero; geti("pitch_zero", t, 0, 1023); pitch_zero = t;
    t = yaw_min; geti("yaw_min", t, -90, 90); yaw_min = t;
    t = yaw_max; geti("yaw_max", t, -90, 90); yaw_max = t;
    t = pitch_min; geti("pitch_min", t, -30, 88); pitch_min = t;
    t = pitch_max; geti("pitch_max", t, -30, 88); pitch_max = t;
    t = pwm_yaw_pin; geti("pwm_yaw_pin", t, 1, 48); pwm_yaw_pin = t;
    t = pwm_pitch_pin; geti("pwm_pitch_pin", t, 1, 48); pwm_pitch_pin = t;
    t = servo_speed; geti("servo_speed", t, 50, 3000); servo_speed = t;
    const cJSON* im = cJSON_GetObjectItem(d, "idle_motion");
    if (im && cJSON_IsBool(im)) idle_motion = cJSON_IsTrue(im);
    t = home_yaw; geti("home_yaw", t, -90, 90); home_yaw = t;
    t = home_pitch; geti("home_pitch", t, -30, 88); home_pitch = t;
    t = volume; geti("volume", t, 0, 100); volume = t;
    t = led_effect; geti("led_effect", t, 0, 2); led_effect = t;
    t = ir_tx_pin; geti("ir_tx_pin", t, -1, 48); ir_tx_pin = t;
    t = ir_rx_pin; geti("ir_rx_pin", t, -1, 48); ir_rx_pin = t;
    const cJSON* ne = cJSON_GetObjectItem(d, "nfc_enable");
    if (ne && cJSON_IsBool(ne)) nfc_enable = cJSON_IsTrue(ne);
    t = nfc_sda; geti("nfc_sda", t, 1, 48); nfc_sda = t;
    t = nfc_scl; geti("nfc_scl", t, 1, 48); nfc_scl = t;
    t = neko_count; geti("neko_count", t, 0, 64); neko_count = t;
    t = neko_pin; geti("neko_pin", t, 1, 48); neko_pin = t;
    const cJSON* co = cJSON_GetObjectItem(d, "cam_overlay");
    if (co && cJSON_IsBool(co)) cam_overlay = cJSON_IsTrue(co);
    const cJSON* cs = cJSON_GetObjectItem(d, "cam_stream");
    if (cs && cJSON_IsBool(cs)) cam_stream = cJSON_IsTrue(cs);
}
