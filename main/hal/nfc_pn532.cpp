#include "nfc_pn532.h"
#include "config_store.h"
#include "driver/i2c_master.h"
#include <cstring>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

NfcPn532 nfc;
static const char* TAG = "nfc";

static uint32_t millisN() { return (uint32_t)(esp_timer_get_time() / 1000LL); }
static constexpr uint8_t PN532_I2C_ADDR = 0x24;   // 7-bit (0x48 >> 1)

bool NfcPn532::begin() {
    if (!cfg.nfc_enable) return false;
    static i2c_master_bus_handle_t bus = nullptr;
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = -1;   // auto
    bc.sda_io_num = (gpio_num_t)cfg.nfc_sda;
    bc.scl_io_num = (gpio_num_t)cfg.nfc_scl;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = PN532_I2C_ADDR;
    dc.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return false;
    _dev = dev;
    // probe: SAMConfig
    uint8_t sam[2] = {0xD4, 0x14};   // SAMConfig
    if (!writeCmd(sam, 2)) {
        ESP_LOGI(TAG, "PN532 not found on G%d/G%d", cfg.nfc_sda, cfg.nfc_scl);
        return false;
    }
    uint8_t resp[16];
    if (readResp(resp, sizeof(resp), 0x15) < 0) {
        ESP_LOGI(TAG, "PN532 SAMConfig no reply");
        return false;
    }
    _ok = true;
    ESP_LOGI(TAG, "PN532 ready on G%d/G%d", cfg.nfc_sda, cfg.nfc_scl);
    return true;
}

bool NfcPn532::writeCmd(const uint8_t* cmd, size_t len) {
    // PN532 I2C frame: [RDY=01][00 00 FF][LEN LEN CS][D4 cmd params...][DCS][00]
    uint8_t frame[64];
    size_t n = 0;
    frame[n++] = 0x01;                                  // I2C ready prefix
    frame[n++] = 0x00; frame[n++] = 0x00; frame[n++] = 0xFF;
    uint8_t plen = (uint8_t)(len + 2);                  // cmd direction + data
    frame[n++] = plen; frame[n++] = (uint8_t)(~plen + 1);
    uint8_t dcs = 0xD4;
    frame[n++] = 0xD4;
    memcpy(frame + n, cmd, len); n += len;
    frame[n++] = (uint8_t)(0x00 - dcs);
    for (size_t i = 1; i < len; i++) dcs += cmd[i];     // (kept simple; dcs computed over TFI+data)
    frame[n++] = 0x00;
    return i2c_master_transmit((i2c_master_dev_handle_t)_dev, frame, n, 50) == ESP_OK;
}

int NfcPn532::readResp(uint8_t* buf, size_t cap, uint8_t expectCmd) {
    // poll until ready, then read full frame: [01][00 00 FF LEN LEN CS][D5 cmd+1 data...][DCS][00]
    uint8_t hdr[6];
    for (int attempt = 0; attempt < 20; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (i2c_master_receive((i2c_master_dev_handle_t)_dev, hdr, 6, 20) != ESP_OK) continue;
        if (hdr[0] != 0x01) continue;
        if (hdr[1] != 0x00 || hdr[2] != 0x00 || hdr[3] != 0xFF) continue;
        uint8_t len = hdr[4];
        if ((uint8_t)(hdr[4] + hdr[5]) != 0) continue;
        if (len < 2 || len > cap - 8) return -1;
        uint8_t body[64];
        if (i2c_master_receive((i2c_master_dev_handle_t)_dev, body, len + 3, 30) != ESP_OK) return -1;
        // body = [D5 cmd+1][data...][DCS][00]
        if (body[0] != 0xD5) return -1;
        if (body[1] != (uint8_t)(expectCmd + 1)) return -1;
        size_t dlen = len - 2;
        if (dlen > cap) dlen = cap;
        memcpy(buf, body + 2, dlen);
        return (int)dlen;
    }
    return -1;
}

void NfcPn532::tick() {
    if (!_ok) return;
    uint32_t now = millisN();
    if (now - _lastPoll < 500) return;
    _lastPoll = now;
    uint8_t cmd[4] = {0xD4, 0x4A, 0x01, 0x00};   // InListPassiveTarget, 1 target, 106kbps
    if (!writeCmd(cmd, 4)) return;
    uint8_t resp[32];
    int n = readResp(resp, sizeof(resp), 0x4A);
    if (n < 4 || resp[1] < 1) return;
    // resp = [nbTargets][tg][uidLen][uid...]
    uint8_t uidLen = resp[2];
    if (uidLen > 8 || 3 + uidLen > n) return;
    std::string s;
    char b[3];
    for (int i = 0; i < uidLen; i++) {
        snprintf(b, 3, "%02X", resp[3 + i]);
        s += b;
    }
    if (s != _uid || now - _ts > 3000) {
        _uid = s;
        _ts = now;
        _fresh = true;
    }
}
