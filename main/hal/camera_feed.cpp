#include "camera_feed.h"
#include "fw_config.h"
#include <M5Unified.h>
#include "esp_camera.h"
#include "img_converters.h"
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <esp_log.h>

CameraFeed camera;
static const char* TAG = "camera";

bool CameraFeed::begin() {
    if (_ready) return true;
    _slot = (uint16_t*)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_slot) return false;
    _mux = xSemaphoreCreateMutex();

    // esp32-camera owns SCCB (G12/G11); M5Unified's lgfx I2C auto-reinitialises
    // on its next access — same coexistence as the M5 reference firmware.
    m5::In_I2C.release();

    camera_config_t c = {};
    c.pin_pwdn = -1;
    c.pin_reset = -1;
    c.pin_xclk = -1;                    // XCLK from onboard oscillator (no GPIO)
    c.pin_sccb_sda = CAM_PIN_SCCB_SDA;
    c.pin_sccb_scl = CAM_PIN_SCCB_SCL;
    c.pin_d7 = CAM_PIN_D7;
    c.pin_d6 = CAM_PIN_D6;
    c.pin_d5 = CAM_PIN_D5;
    c.pin_d4 = CAM_PIN_D4;
    c.pin_d3 = CAM_PIN_D3;
    c.pin_d2 = CAM_PIN_D2;
    c.pin_d1 = CAM_PIN_D1;
    c.pin_d0 = CAM_PIN_D0;
    c.pin_vsync = CAM_PIN_VSYNC;
    c.pin_href = CAM_PIN_HREF;
    c.pin_pclk = CAM_PIN_PCLK;
    c.xclk_freq_hz = 20000000;          // inert when pin_xclk < 0
    c.ledc_timer = LEDC_TIMER_0;
    c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = PIXFORMAT_RGB565;
    c.frame_size = FRAMESIZE_QVGA;
    c.jpeg_quality = 0;
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;
    c.sccb_i2c_port = -1;
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed 0x%x", err);
        heap_caps_free(_slot);
        _slot = nullptr;
        return false;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s) { s->set_hmirror(s, 0); s->set_vflip(s, 0); }
    _ready = true;
    ESP_LOGI(TAG, "GC0308 ready (RGB565 QVGA)");
    return true;
}

void CameraFeed::start() {
    if (!_ready || _run) return;
    _run = true;
    xTaskCreatePinnedToCore(taskTramp, "camtask", 4096, this, 4, nullptr, 0);
}

void CameraFeed::stop() {
    _run = false;
    _ready = false;
}

void CameraFeed::taskTramp(void* p) { ((CameraFeed*)p)->taskLoop(); }

void CameraFeed::taskLoop() {
    uint32_t frames = 0, fpsWin = millis32();
    while (_run) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (fb->len == W * H * 2 && fb->width == W && fb->height == H) {
            if (xSemaphoreTake(_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
                memcpy(_slot, fb->buf, W * H * 2);
                xSemaphoreGive(_mux);
                _ts = millis32();
                frames++;
            }
        }
        esp_camera_fb_return(fb);
        if (millis32() - fpsWin >= 2000) {
            _fps = frames * 1000.0f / (millis32() - fpsWin);
            frames = 0; fpsWin = millis32();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelete(nullptr);
}

bool CameraFeed::getRgb565(uint16_t* dst, uint16_t& w, uint16_t& h, uint32_t& tsMs) {
    if (!_slot) return false;
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(150)) != pdTRUE) return false;
    memcpy(dst, _slot, W * H * 2);
    xSemaphoreGive(_mux);
    w = W; h = H; tsMs = _ts;
    return true;
}

bool CameraFeed::samplePip(uint16_t* dst, int pw, int ph) {
    if (!_slot) return false;
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    for (int r = 0; r < ph; r++) {
        int sy = r * H / ph;
        const uint16_t* src = _slot + (uint32_t)sy * W;
        for (int c = 0; c < pw; c++) {
            dst[r * pw + c] = src[c * W / pw];
        }
    }
    xSemaphoreGive(_mux);
    return true;
}

bool CameraFeed::pushToDisplay(int x, int y, int w, int h) {
    if (!_slot) return false;
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(150)) != pdTRUE) return false;
    M5.Display.startWrite();
    M5.Display.pushImage(x, y, w, h, _slot);
    M5.Display.endWrite();
    xSemaphoreGive(_mux);
    return true;
}

bool CameraFeed::getJpeg(uint8_t** out, size_t* outLen, int quality) {
    *out = nullptr; *outLen = 0;
    if (!_slot) return false;
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    // fmt2jpg wants little-endian RGB565; DVP frames are BE — swap to scratch.
    static uint16_t* tmp = nullptr;
    if (!tmp) tmp = (uint16_t*)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    if (!tmp) { xSemaphoreGive(_mux); return false; }
    for (int i = 0; i < W * H; i++) tmp[i] = __builtin_bswap16(_slot[i]);
    xSemaphoreGive(_mux);
    bool ok = fmt2jpg((uint8_t*)tmp, W * H * 2, W, H, PIXFORMAT_RGB565, quality, out, outLen);
    return ok && *out;
}
