#pragma once
#include <cstdint>

enum class Mood : uint8_t { Idle, Thinking, Listening, Speaking, Happy, Error };

// LED status engine: 12 base WS2812 (via PY32 expander) + optional cat-ear
// chain (espressif/led_strip on a free GPIO).
class Leds {
public:
    void begin();
    void tick();
    void setMood(Mood m);
    void overrideColor(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);
private:
    void push();
    Mood _mood = Mood::Idle;
    uint32_t _ovUntil = 0;
    uint8_t _ov[3] = {0, 0, 0};
    uint32_t _last = 0;
    uint8_t _phase = 0;
    bool _neko = false;
    void* _nekoStrip = nullptr;   // led_strip_handle_t
};

extern Leds leds;
