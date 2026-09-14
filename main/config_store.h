#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include "fw_config.h"
#include "cJSON.h"

// All persistent settings, NVS-backed (blob + crc), JSON export/import for the
// web UI. Fixed char arrays keep the blob layout stable across saves.
struct Config {
    // wifi
    char    wifi_ssid[33] = "";
    char    wifi_pass[65] = "";
    char    tz[64] = "AEST-10AEDT,M10.1.0,M4.1.0/3";
    // llm
    uint8_t provider = 0;   // 0=OpenAI 1=Gemini 2=Xiaozhi
    char    openai_key[128] = "";
    char    openai_model[48] = "gpt-4o-mini";
    char    openai_base[96] = "https://api.openai.com/v1";
    char    openai_stt[48] = "whisper-1";
    char    openai_tts[48] = "tts-1";
    char    openai_voice[24] = "alloy";
    char    gemini_key[128] = "";
    char    gemini_model[48] = "gemini-2.5-flash";
    bool    gemini_tts = true;
    char    gemini_tts_model[64] = "gemini-2.5-flash-preview-tts";
    char    gemini_voice[24] = "Kore";
    char    xz_url[128] = "wss://api.tenclass.net/xiaozhi/v1/ws/";
    char    xz_token[128] = "";
    char    persona[256] = "You are Stack-chan, a cute small desk robot with a face, expressive eyes and a moving head. Be warm, playful and concise (1-3 short sentences). You can move your head when excited.";
    // pi
    char    pi_host[64] = "raspberrypi.local";
    uint16_t pi_ttyd_port = 7681;
    uint16_t pi_agent_port = 8765;
    char    pi_token[64] = "stackchan";
    // servos
    uint8_t servo_mode = 0; // 0=auto, 1=force SCS, 2=force PWM
    int16_t yaw_zero = 460, pitch_zero = 620;
    int16_t yaw_min = -40, yaw_max = 40;
    int16_t pitch_min = -10, pitch_max = 25;
    int16_t pwm_yaw_pin = PWM_YAW_PIN, pwm_pitch_pin = PWM_PITCH_PIN;
    uint16_t servo_speed = 600;
    bool    idle_motion = true;
    int16_t home_yaw = 0, home_pitch = 8;
    // audio / leds / units
    uint8_t volume = 70;
    uint8_t led_effect = 0;
    uint8_t ir_tx_pin = IR_TX_PIN_DEFAULT;
    uint8_t ir_rx_pin = IR_RX_PIN_DEFAULT;
    bool    nfc_enable = true;
    uint8_t nfc_sda = NFC_SDA_PIN, nfc_scl = NFC_SCL_PIN;
    uint8_t neko_count = 0;
    uint8_t neko_pin = NEKO_PIN_DEFAULT;
    bool    cam_overlay = true;
    bool    cam_stream = true;

    void load();
    void save();
    std::string toJson(bool maskSecrets) const;   // web UI out
    void fromJson(const cJSON* in);               // web UI in (secrets kept if masked/empty)
};

extern Config cfg;

void copyJsonStr(char* dst, size_t cap, const cJSON* item);
void copyJsonSecret(char* dst, size_t cap, const cJSON* item);
