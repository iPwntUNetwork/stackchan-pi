#include "head_touch.h"
#include "fw_config.h"
#include <M5Unified.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

HeadTouch headTouch;
static constexpr uint32_t kFreq = SI12T_I2C_FREQ;

static inline uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

bool HeadTouch::probe() {
    _ok = false;
    static const uint8_t enableRegs[] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    for (uint8_t r : enableRegs)
        if (!m5::In_I2C.writeRegister8(SI12T_ADDR, r, 0x00, kFreq)) return false;
    if (!m5::In_I2C.writeRegister8(SI12T_ADDR, 0x09, 0x01, kFreq)) return false;  // CTRL2 reset
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!m5::In_I2C.writeRegister8(SI12T_ADDR, 0x08, 0x25, kFreq)) return false;  // CTRL1
    if (!m5::In_I2C.writeRegister8(SI12T_ADDR, 0x02, 0x33, kFreq)) return false;  // sensitivity
    _ok = true;
    return true;
}

void HeadTouch::tick() {
    if (!_ok) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 100) return;
    last = now;
    uint8_t v = m5::In_I2C.readRegister8(SI12T_ADDR, SI12T_REG_OUTPUT1, kFreq);
    uint8_t f = v & 3, m = (v >> 2) & 3, b = (v >> 4) & 3;
    if (f != 0 && f == m && m == b) { f = m = b = 0; }   // RFI filter
    bool touched = (m >= 2 || b >= 2);

    if (touched) {
        if (_touchStart == 0) _touchStart = now;
        if (!_petted && now - _touchStart > 350 && !_holdActive) {
            _petted = true;
            _evt = Pet;
        }
        if (!_holdActive && now - _touchStart > 900) {
            _holdActive = true;
            _evt = HoldTalk;
        }
    } else {
        _holdActive = false;
        _touchStart = 0;
        _petted = false;
    }
}
