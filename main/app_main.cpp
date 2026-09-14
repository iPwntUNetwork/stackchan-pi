// ============================================================
//  Stack-chan CoreS3 — ESP-IDF firmware
//  LLM assistant (OpenAI / Gemini / Xiaozhi) + Raspberry Pi
//  terminal + camera loop + servos + LEDs + IR + NFC.
// ============================================================
#include <M5Unified.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "config_store.h"
#include "net/wifi_mgr.h"
#include "net/web_app.h"
#include "net/pi_link.h"
#include "hal/motion.h"
#include "hal/py32_expander.h"
#include "hal/camera_feed.h"
#include "hal/audio_io.h"
#include "hal/leds.h"
#include "hal/ir_rmt.h"
#include "hal/nfc_pn532.h"
#include "hal/head_touch.h"
#include "llm/chat_engine.h"
#include "llm/xiaozhi_client.h"
#include "llm/llm_client.h"
#include "ui/ui.h"
#include "ui/screens.h"
#include "ui/keyboard.h"
#include "ui/face.h"

static const char* TAG = "app";

static void bootScreen(const char* line) {
    auto& d = M5.Display;
    d.fillScreen(0x1082);
    d.setTextColor(0xFFFF, 0x1082);
    d.setTextSize(2);
    d.drawString("Stack-chan", 88, 90);
    d.setTextSize(1);
    d.setTextColor(0x05DF, 0x1082);
    static int pw = 10;
    d.fillRect(60, 120, 200, 6, 0x2127);
    pw = pw < 200 ? pw + 12 : 200;
    d.fillRect(60, 120, pw, 6, 0x05DF);
    d.drawString(line, 60, 140);
    ESP_LOGI(TAG, "%s", line);
}

static bool webStarted = false;
static bool xzAuto = false;
static uint32_t lastNfcTs = 0;

// ---- main loop task (replaces Arduino loop()) ----

static void mainTask(void*) {
    while (true) {
        M5.update();
        wifi.tick();
        audio.tick();
        headTouch.tick();
        nfc.tick();
        leds.tick();
        motion.tick();
        chat.tick();
        ui.tick();

        if (!webStarted && (wifi.connected() || wifi.state() == WifiState::ApPortal)) {
            webApp.begin();
            webStarted = true;
        }

        if (!xzAuto && cfg.provider == 2 && wifi.connected() &&
            cfg.xz_url[0] && cfg.xz_token[0]) {
            xzAuto = true;
            chat.xzConnect();
        }
        if (cfg.provider != 2) xzAuto = false;

        // head-touch events
        static bool headHoldPrev = false;
        HeadTouch::Event ev = headTouch.consumeEvent();
        if (ev == HeadTouch::Pet) {
            face.setEmotion(Emotion::Happy);
            leds.setMood(Mood::Happy);
            motion.nudge(8, 6);
            motion.idleKick();
            ESP_LOGI(TAG, "head pat!");
        }
        if (headTouch.holdActive() && !headHoldPrev) {
            if (cfg.provider == 2) chat.xzPushToTalk(true);
            else chat.startVoice();
        }
        if (!headTouch.holdActive() && headHoldPrev) {
            if (cfg.provider == 2) chat.xzPushToTalk(false);
            else chat.stopVoice();
        }
        headHoldPrev = headTouch.holdActive();

        // NFC tap reaction
        if (nfc.hasTag() && nfc.tagTs() != lastNfcTs) {
            lastNfcTs = nfc.tagTs();
            std::string uid = nfc.uidHex();
            nfc.consume();
            ui.toast("NFC: " + uid, 2000);
            ESP_LOGI(TAG, "nfc tag %s", uid.c_str());
            if (cfg.provider != 2) {
                chat.submitText("[event] Someone tapped an NFC tag (UID " + uid +
                                ") on me. React with one short cute sentence!", false);
            }
        }

        // mood follows chat state
        switch (chat.state()) {
            case ChatEngine::State::Thinking:
            case ChatEngine::State::Transcribing:
                face.setEmotion(Emotion::Doubt);
                leds.setMood(Mood::Thinking);
                break;
            case ChatEngine::State::Speaking:
            case ChatEngine::State::XzServerSpeaking:
                face.tickSpeech();
                face.setEmotion(Emotion::Happy);
                leds.setMood(Mood::Speaking);
                break;
            case ChatEngine::State::Recording:
            case ChatEngine::State::XzListening:
                face.setEmotion(Emotion::Surprised);
                leds.setMood(Mood::Listening);
                break;
            case ChatEngine::State::Error:
                face.setEmotion(Emotion::Sad);
                leds.setMood(Mood::Error);
                break;
            default:
                leds.setMood(Mood::Idle);
                break;
        }

        float yaw, pitch;
        motion.getYawPitch(yaw, pitch);
        face.setGaze(yaw / 90.0f, -pitch / 30.0f);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    auto m5cfg = M5.config();
    M5.begin(m5cfg);
    M5.Display.setRotation(1);
    bootScreen("loading config...");
    cfg.load();

    bootScreen("hardware init...");
    py32.probe();
    leds.begin();
    audio.begin();
    headTouch.probe();   // before camera grabs internal I2C
    nfc.begin();
    irRmt.begin();
    motion.begin();

    bootScreen("camera...");
    if (cfg.cam_stream) {
        camera.begin();
        camera.start();
    }

    bootScreen("network...");
    wifi.begin();
    ui.begin();
    kbdInit();
    chat.begin();
    pi.begin();

    bootScreen("ready!");
    motion.center();
    leds.setMood(Mood::Idle);

    xTaskCreatePinnedToCore(mainTask, "main", 12288, nullptr, 3, nullptr, 1);
}
