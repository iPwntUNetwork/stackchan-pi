#include "ir_rmt.h"
#include "config_store.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include <cstring>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

IrRmt irRmt;
static const char* TAG = "ir";
static IrRmt* g_ir = nullptr;

static uint32_t millisIr() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

// ---- RX done callback (IDF 5 rmt API) ----
static rmt_symbol_word_t g_rxBuf[192];
static volatile size_t g_rxSymbols = 0;
static volatile bool g_rxDone = false;

static bool ir_rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t* e, void* ud) {
    if (e->num_symbols > 0 && e->num_symbols <= 192) {
        memcpy((void*)g_rxBuf, e->received_symbols, e->num_symbols * sizeof(rmt_symbol_word_t));
        g_rxSymbols = e->num_symbols;
        g_rxDone = true;
    }
    return false;
}

void IrRmt::begin() {
    g_ir = this;
    _txPin = cfg.ir_tx_pin;
    _rxPin = cfg.ir_rx_pin;
    if (_txPin >= 0 && _txPin < 48) {
        rmt_tx_channel_config_t tc = {};
        tc.gpio_num = (gpio_num_t)_txPin;
        tc.clk_src = RMT_CLK_SRC_DEFAULT;
        tc.resolution_hz = 1 * 1000 * 1000;   // 1 us per tick
        tc.mem_block_symbols = 64;
        tc.trans_queue_depth = 2;
        rmt_channel_handle_t ch = nullptr;
        if (rmt_new_tx_channel(&tc, &ch) == ESP_OK && ch) {
            rmt_carrier_config_t cc = {};
            cc.frequency_hz = 38000;
            cc.duty_cycle = 0.25f;
            if (rmt_apply_carrier(ch, &cc) == ESP_OK) {
                rmt_copy_encoder_config_t ec = {};
                rmt_encoder_handle_t enc = nullptr;
                if (rmt_new_copy_encoder(&ec, &enc) == ESP_OK && enc) {
                    rmt_enable(ch);
                    _txChan = ch;
                    _copyEnc = enc;
                    ESP_LOGI(TAG, "TX on G%d (NEC, 38 kHz carrier)", _txPin);
                }
            }
            if (!_txChan) rmt_del_channel(ch);
        }
    }
    if (_rxPin >= 0 && _rxPin < 48) {
        xTaskCreatePinnedToCore(rxTaskTramp, "irrx", 4096, this, 3, nullptr, 0);
        ESP_LOGI(TAG, "RX on G%d", _rxPin);
    }
}

bool IrRmt::sendNec(uint64_t value, uint8_t bits) {
    if (!_txChan || !_copyEnc) return false;
    if (!bits || bits > 64) bits = 32;
    rmt_symbol_word_t sym[2 + 64 + 1];
    int n = 0;
    sym[n].level0 = 1; sym[n].duration0 = 9000; sym[n].level1 = 0; sym[n].duration1 = 4500; n++;
    for (int i = (int)bits - 1; i >= 0; i--) {
        bool one = (value >> i) & 1;
        sym[n].level0 = 1; sym[n].duration0 = 560;
        sym[n].level1 = 0; sym[n].duration1 = one ? 1690 : 560;
        n++;
    }
    sym[n].level0 = 1; sym[n].duration0 = 560; sym[n].level1 = 0; sym[n].duration1 = 40000; n++;
    rmt_transmit_config_t tcfg = {};
    tcfg.loop_count = 0;
    esp_err_t err = rmt_transmit((rmt_channel_handle_t)_txChan, (rmt_encoder_handle_t)_copyEnc,
                                 sym, n * sizeof(rmt_symbol_word_t), &tcfg);
    rmt_tx_wait_all_done((rmt_channel_handle_t)_txChan, 200);
    return err == ESP_OK;
}

bool IrRmt::sendRaw() {
    if (!_txChan || !_copyEnc || !_rawReady) return false;
    rmt_transmit_config_t tcfg = {};
    tcfg.loop_count = 0;
    esp_err_t err = rmt_transmit((rmt_channel_handle_t)_txChan, (rmt_encoder_handle_t)_copyEnc,
                                 _rawSymbols, _rawCount * sizeof(rmt_symbol_word_t), &tcfg);
    rmt_tx_wait_all_done((rmt_channel_handle_t)_txChan, 300);
    return err == ESP_OK;
}

bool IrRmt::startLearn() {
    if (_rxPin < 0) return false;
    _armed = true;
    _armUntil = millisIr() + 5000;
    return true;
}

bool IrRmt::pollLearn(uint64_t& value, uint8_t& bits, bool& nec, bool& rawReady) {
    value = 0; bits = 0; nec = false; rawReady = false;
    if (!_armed) return false;
    if (g_rxDone && g_rxSymbols > 20) {
        _armed = false;
        g_rxDone = false;
        // stash raw (mark+space pairs as received, in 1 us ticks)
        _rawCount = g_rxSymbols > 160 ? 160 : g_rxSymbols;
        memcpy(_rawSymbols, (const void*)g_rxBuf, _rawCount * sizeof(rmt_symbol_word_t));
        _rawReady = true;
        // try NEC decode: find 9 ms leader
        int start = -1;
        for (size_t i = 0; i < _rawCount; i++) {
            if (_rawSymbols[i].duration0 > 7000 && _rawSymbols[i].duration0 < 11000) { start = (int)i; break; }
        }
        if (start >= 0 && _rawCount > (size_t)start + 16) {
            uint64_t v = 0;
            int nb = 0;
            for (size_t i = start + 1; i < _rawCount && nb < 64; i++) {
                uint16_t mark = _rawSymbols[i].duration0;
                uint16_t space = _rawSymbols[i].duration1;
                if (mark < 300 || mark > 900) break;
                if (space < 200) break;
                v <<= 1;
                if (space > 1000) v |= 1;
                nb++;
            }
            if (nb >= 24) {
                value = v;
                bits = (uint8_t)nb;
                nec = true;
                return true;
            }
        }
        rawReady = true;   // non-NEC protocol — raw replay still possible
        return true;
    }
    if (millisIr() > _armUntil) _armed = false;
    return false;
}

void IrRmt::rxTaskTramp(void* p) { ((IrRmt*)p)->rxTask(); }

void IrRmt::rxTask() {
    rmt_rx_channel_config_t rc = {};
    rc.gpio_num = (gpio_num_t)_rxPin;
    rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = 1 * 1000 * 1000;
    rc.mem_block_symbols = 128;
    rmt_channel_handle_t rx = nullptr;
    if (rmt_new_rx_channel(&rc, &rx) != ESP_OK || !rx) {
        ESP_LOGW(TAG, "rx channel failed");
        vTaskDelete(nullptr);
        return;
    }
    static const rmt_rx_event_callbacks_t cbs = { .on_recv_done = ir_rx_done_cb };
    rmt_rx_register_event_callbacks(rx, &cbs, nullptr);
    rmt_enable(rx);

    rmt_receive_config_t rcfg = {};
    rcfg.signal_range_min_ns = 2000;
    rcfg.signal_range_max_ns = 20 * 1000 * 1000;

    while (true) {
        if (_armed && !_rawReady) {
            if (g_rxDone) {
                // frame delivered — leave to pollLearn
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (rmt_receive(rx, g_rxBuf, sizeof(g_rxBuf), &rcfg) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(60));
        }
    }
}
