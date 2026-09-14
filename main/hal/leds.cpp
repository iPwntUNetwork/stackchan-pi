#include "leds.h"
#include "py32_expander.h"
#include "config_store.h"
#include "led_strip.h"
#include <cstring>
#include <esp_timer.h>

Leds leds;

static uint32_t millisLed() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

static uint16_t hsv565(uint8_t h) {  // h 0..255
    uint8_t region = h / 43;
    uint8_t rem = (h - region * 43) * 6;
    if (rem > 255) rem = 255;
    uint8_t v = 255, q = (uint8_t)((v * (255 - rem)) >> 8), t = (uint8_t)((v * rem) >> 8);
    uint8_t r, g, b;
    switch (region) {
        case 0: r = v; g = t; b = 0; break;
        case 1: r = q; g = v; b = 0; break;
        case 2: r = 0; g = v; b = t; break;
        case 3: r = 0; g = q; b = v; break;
        case 4: r = t; g = 0; b = v; break;
        default: r = v; g = 0; b = q; break;
    }
    return Py32Expander::rgb565(r, g, b);
}

void Leds::begin() {
    if (cfg.neko_count > 0 && cfg.neko_count <= 64) {
        led_strip_config_t sc = {};
        sc.strip_gpio_num = cfg.neko_pin;
        sc.max_leds = cfg.neko_count;
        sc.led_model = LED_MODEL_WS2812;
        sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        sc.flags = {};
        led_strip_rmt_config_t rc = {};
        rc.clk_src = RMT_CLK_SRC_DEFAULT;
        rc.resolution_hz = 10 * 1000 * 1000;
        rc.mem_block_symbols = 64;
        led_strip_handle_t h = nullptr;
        if (led_strip_new_rmt_device(&sc, &rc, &h) == ESP_OK && h) {
            _nekoStrip = h;
            _neko = true;
            for (int i = 0; i < cfg.neko_count; i++) led_strip_set_pixel(h, i, 0, 0, 0);
            led_strip_refresh(h);
        }
    }
}

void Leds::setMood(Mood m) { _mood = m; }

void Leds::overrideColor(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    _ov[0] = r; _ov[1] = g; _ov[2] = b;
    _ovUntil = millisLed() + ms;
}

void Leds::push() {
    uint16_t base[BASE_LED_COUNT];
    uint32_t now = millisLed();
    if (now < _ovUntil || cfg.led_effect == 2) {
        for (int i = 0; i < BASE_LED_COUNT; i++) {
            base[i] = (cfg.led_effect == 2) ? hsv565(_phase + i * 10)
                                            : Py32Expander::rgb565(_ov[0], _ov[1], _ov[2]);
        }
    } else if (cfg.led_effect == 1) {
        memset(base, 0, sizeof(base));
    } else {
        uint8_t r = 0, g = 0, b = 0;
        bool pulse = true;
        switch (_mood) {
            case Mood::Idle:      r = 0;   g = 60;  b = 60;  break;
            case Mood::Thinking:  r = 120; g = 70;  b = 0;   break;
            case Mood::Listening: r = 0;   g = 30;  b = 140; break;
            case Mood::Speaking:  r = 0;   g = 120; b = 20;  break;
            case Mood::Happy:     r = 160; g = 90;  b = 0;   break;
            case Mood::Error:     r = 140; g = 0;   b = 0;   pulse = false; break;
        }
        uint8_t dim = 255;
        if (pulse) {
            int ph = (_phase * 4) & 255;
            dim = 120 + (ph > 127 ? 255 - ph : ph);
        }
        r = r * dim >> 8; g = g * dim >> 8; b = b * dim >> 8;
        uint16_t c = Py32Expander::rgb565(r, g, b);
        for (int i = 0; i < BASE_LED_COUNT; i++) base[i] = c;
    }
    py32.writeLeds(base, BASE_LED_COUNT);

    if (_neko && _nekoStrip) {
        led_strip_handle_t h = (led_strip_handle_t)_nekoStrip;
        for (int i = 0; i < cfg.neko_count; i++) {
            uint16_t c = (cfg.led_effect == 2) ? hsv565(_phase + i * 14)
                         : base[i % BASE_LED_COUNT];
            uint8_t r5 = ((c >> 11) & 0x1F) << 3, g6 = ((c >> 5) & 0x3F) << 2, b5 = (c & 0x1F) << 3;
            led_strip_set_pixel(h, i, r5, g6, b5);
        }
        led_strip_refresh(h);
    }
}

void Leds::tick() {
    uint32_t now = millisLed();
    if (now - _last < 50) return;
    _last = now;
    _phase += 2;
    push();
}
