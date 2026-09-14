#include "opus_codec.h"
#include "esp_log.h"

// espressif/esp_audio_codec Opus encoder/decoder wrappers
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"

static const char* TAG = "opus";
static void* s_enc = nullptr;   // esp_audio_enc_handle_t (opus)
static void* s_dec = nullptr;   // esp_audio_dec_handle_t (opus)

bool opusEncInit() {
    if (s_enc) return true;
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = 16000;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = 24000;
    cfg.complexity = 1;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.enable_fec = false;
    cfg.enable_dtx = false;
    cfg.enable_vbr = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_enc) != ESP_AUDIO_ERR_OK || !s_enc) {
        ESP_LOGE(TAG, "enc open failed");
        s_enc = nullptr;
        return false;
    }
    return true;
}

int opusEncode(const int16_t* pcm, size_t samples, uint8_t* out, size_t outCap) {
    if (!s_enc) return -1;
    esp_audio_enc_in_frame_t in = {};
    in.buffer = (uint8_t*)pcm;
    in.len = (uint32_t)(samples * 2);
    esp_audio_enc_out_frame_t outf = {};
    outf.buffer = out;
    outf.len = (uint32_t)outCap;
    if (esp_audio_enc_process((esp_audio_enc_handle_t)s_enc, &in, &outf) != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return (int)outf.encoded_bytes;
}

bool opusDecInit(uint32_t sampleRate) {
    if (s_dec) {
        esp_opus_dec_close(s_dec);
        s_dec = nullptr;
    }
    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate = sampleRate;
    cfg.channel = 1;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;  // treat as 60 ms
    cfg.self_delimited = false;
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &s_dec) != ESP_AUDIO_ERR_OK || !s_dec) {
        ESP_LOGE(TAG, "dec open failed");
        s_dec = nullptr;
        return false;
    }
    return true;
}

int opusDecode(const uint8_t* pkt, size_t len, int16_t* out, size_t outCapSamples) {
    if (!s_dec) return -1;
    esp_audio_dec_in_raw_t in = {};
    in.buffer = (uint8_t*)pkt;
    in.len = (uint32_t)len;
    esp_audio_dec_out_frame_t of = {};
    of.buffer = (uint8_t*)out;
    of.len = (uint32_t)(outCapSamples * 2);
    esp_audio_err_t err = esp_audio_dec_process((esp_audio_dec_handle_t)s_dec, &in, &of);
    if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) return -1;
    return (int)(of.decoded_size / 2);   // 16-bit mono samples
}
