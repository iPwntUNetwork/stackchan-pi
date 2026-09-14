#pragma once
#include <cstdint>
#include <cstddef>
#include "fw_config.h"

// PY32 IO-expander on the M5 Stack-chan base (@0x6F internal I2C).
// Owns the servo VM power enable and the 12 base WS2812 LEDs
// (I2C RAM RGB565-LE + latch via LED_CFG refresh bit).
class Py32Expander {
public:
    bool probe();
    bool present() const { return _ok; }
    bool setServoPower(bool on);
    void setLedCount(uint8_t n);
    void writeLeds(const uint16_t* rgb565, uint8_t count);
    static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
private:
    bool wr8(uint8_t reg, uint8_t v);
    bool wrBuf(uint8_t reg, const uint8_t* buf, size_t n);
    bool _ok = false;
    uint8_t _addr = PY32_ADDR;
    uint8_t _count = BASE_LED_COUNT;
};

extern Py32Expander py32;
