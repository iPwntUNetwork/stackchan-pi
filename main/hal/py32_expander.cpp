#include "py32_expander.h"
#include <M5Unified.h>

Py32Expander py32;
static constexpr uint32_t kFreq = PY32_I2C_FREQ;
static constexpr uint8_t kLedCfgRefresh = 0x40;

bool Py32Expander::wr8(uint8_t reg, uint8_t v) {
    return m5::In_I2C.writeRegister8(_addr, reg, v, kFreq);
}
bool Py32Expander::wrBuf(uint8_t reg, const uint8_t* buf, size_t n) {
    return m5::In_I2C.writeRegister(_addr, reg, buf, n, kFreq);
}

bool Py32Expander::probe() {
    _ok = false;
    uint8_t ver = m5::In_I2C.readRegister8(_addr, PY32_REG_VERSION, kFreq);
    if (ver == 0x00 || ver == 0xFF) return false;
    _ok = true;
    setLedCount(BASE_LED_COUNT);
    return true;
}

bool Py32Expander::setServoPower(bool on) {
    if (!_ok) return false;
    uint8_t m = m5::In_I2C.readRegister8(_addr, PY32_REG_GPIO_MODE_L, kFreq);
    m |= (1 << PY32_PIN_VM_EN);
    if (!wr8(PY32_REG_GPIO_MODE_L, m)) return false;
    uint8_t o = m5::In_I2C.readRegister8(_addr, PY32_REG_GPIO_OUT_L, kFreq);
    if (on) o |= (1 << PY32_PIN_VM_EN); else o &= ~(1 << PY32_PIN_VM_EN);
    return wr8(PY32_REG_GPIO_OUT_L, o);
}

void Py32Expander::setLedCount(uint8_t n) {
    if (!_ok || n > PY32_MAX_LEDS) n = PY32_MAX_LEDS;
    _count = n;
    wr8(PY32_REG_LED_CFG, n & 0x3F);
}

void Py32Expander::writeLeds(const uint16_t* rgb565, uint8_t count) {
    if (!_ok || !count) return;
    if (count > _count) count = _count;
    static uint8_t buf[PY32_MAX_LEDS * 2];
    for (uint8_t i = 0; i < count; i++) {
        buf[i * 2 + 0] = rgb565[i] & 0xFF;         // little-endian RGB565
        buf[i * 2 + 1] = rgb565[i] >> 8;
    }
    if (!wrBuf(PY32_REG_LED_RAM, buf, count * 2)) return;
    wr8(PY32_REG_LED_CFG, (_count & 0x3F) | kLedCfgRefresh);   // latch
}
