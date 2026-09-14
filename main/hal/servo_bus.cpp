#include "servo_bus.h"
#include "driver/uart.h"
#include <cstring>

ScsBus scsBus;
static constexpr size_t kMaxParams = 32;

static uint8_t checksum(const uint8_t* b, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += b[i];
    return (uint8_t)(~s);
}

bool ScsBus::begin(uint8_t uart, int tx, int rx, uint32_t baud, bool echoCancel) {
    _uart = uart;
    _echo = echoCancel;
    uart_config_t uc = {};
    uc.baud_rate = (int)baud;
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    if (uart_driver_install((uart_port_t)_uart, 256, 0, 0, nullptr, 0) != ESP_OK) return false;
    if (uart_param_config((uart_port_t)_uart, &uc) != ESP_OK) return false;
    if (uart_set_pin((uart_port_t)_uart, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
    _ok = true;
    return true;
}

void ScsBus::end() {
    if (_uart >= 0) { uart_driver_delete((uart_port_t)_uart); _uart = -1; _ok = false; }
}

bool ScsBus::sendOnly(uint8_t id, uint8_t instr, const uint8_t* params, size_t n) {
    if (!_ok || n > kMaxParams) return false;
    uint8_t tx[kMaxParams + 6];
    tx[0] = 0xFF; tx[1] = 0xFF; tx[2] = id;
    tx[3] = (uint8_t)(n + 2);
    tx[4] = instr;
    if (n) memcpy(tx + 5, params, n);
    tx[5 + n] = checksum(tx + 2, 3 + n);
    size_t total = 6 + n;
    uart_flush_input((uart_port_t)_uart);
    int w = uart_write_bytes((uart_port_t)_uart, tx, total);
    if (w != (int)total) return false;
    uart_wait_tx_done((uart_port_t)_uart, pdMS_TO_TICKS(40));
    if (_echo) {
        uint8_t echo[kMaxParams + 6];
        int got = uart_read_bytes((uart_port_t)_uart, echo, total, pdMS_TO_TICKS(40));
        if (got != (int)total) return false;
    }
    return true;
}

int ScsBus::transact(uint8_t id, uint8_t instr, const uint8_t* params, size_t n,
                     uint8_t* respParams, size_t respCap, uint32_t timeoutMs) {
    if (!sendOnly(id, instr, params, n)) return -1;
    uint8_t hdr[4];
    size_t have = 0;
    uint32_t start = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    auto nowMs = [&]() { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); };
    while (have < 4) {
        int got = uart_read_bytes((uart_port_t)_uart, hdr + have, 4 - have, pdMS_TO_TICKS(timeoutMs));
        if (got > 0) { have += got; continue; }
        if (nowMs() - start > timeoutMs) return -1;
    }
    if (hdr[0] != 0xFF || hdr[1] != 0xFF) return -1;
    if (hdr[2] != id && hdr[2] != 0xFE) return -1;
    uint8_t len = hdr[3];
    if (len < 2 || len > 2 + kMaxParams) return -1;
    uint8_t body[64];
    have = 0;
    while (have < len) {
        int got = uart_read_bytes((uart_port_t)_uart, body + have, len - have, pdMS_TO_TICKS(timeoutMs));
        if (got > 0) { have += got; continue; }
        if (nowMs() - start > 3 * timeoutMs) return -1;
    }
    uint8_t err = body[0];
    if (err & 0x7F) return -1;
    size_t pcount = len - 2;
    uint32_t s = (uint32_t)id + len + err;
    for (size_t i = 1; i < 1 + pcount; i++) s += body[i];
    if ((uint8_t)(~s) != body[1 + pcount]) return -1;
    if (pcount > respCap) pcount = respCap;
    memcpy(respParams, body + 1, pcount);
    return (int)pcount;
}

bool ScsBus::ping(uint8_t id) {
    uint8_t scratch[8];
    return transact(id, 0x01, nullptr, 0, scratch, sizeof(scratch)) >= 0;
}

bool ScsBus::enableTorque(uint8_t id, bool on) {
    uint8_t p[2] = { 0x28, (uint8_t)(on ? 1 : 0) };
    uint8_t scratch[8];
    return transact(id, 0x03, p, 2, scratch, sizeof(scratch)) >= 0;
}

bool ScsBus::writeGoalPosition(uint8_t id, uint16_t raw, uint16_t timeMs, uint16_t speed) {
    if (raw > 1023) raw = 1023;
    uint8_t p[7] = {
        0x2A,
        (uint8_t)((raw >> 8) & 0xFF), (uint8_t)(raw & 0xFF),
        (uint8_t)((timeMs >> 8) & 0xFF), (uint8_t)(timeMs & 0xFF),
        (uint8_t)((speed >> 8) & 0xFF), (uint8_t)(speed & 0xFF),
    };
    uint8_t scratch[8];
    return transact(id, 0x03, p, 7, scratch, sizeof(scratch)) >= 0;
}
