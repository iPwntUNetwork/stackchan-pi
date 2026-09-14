#pragma once
#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// CoreS3 audio (ES7210 mic + AW88298 amp via M5Unified):
//  - push-to-talk recording into PSRAM (16 kHz mono 16-bit), WAV export
//  - streaming PCM playback through a PSRAM ring (thread-safe feed),
//    chunk-chained to M5.Speaker
//  - mic streaming ring for xiaozhi push-to-talk
class AudioIO {
public:
    bool begin();
    // ---- recording ----
    bool startRecord();
    bool stopRecord();
    bool recording() const { return _rec; }
    uint32_t recordMs() const;
    uint8_t* takeWav(size_t& len);
    const int16_t* pcm() const { return _pcm; }
    size_t pcmSamples() const { return _recLen; }
    // ---- playback (streaming) ----
    bool streamStart(uint32_t rate);
    bool streamFeed(const int16_t* data, size_t samples);
    bool streamEnd();
    bool streamActive() const { return _streamActive; }
    // ---- mic streaming ----
    bool startStreamMic();
    void stopStreamMic();
    size_t streamDrain(int16_t* dst, size_t maxSamples);
    // ---- misc ----
    bool playPcm(const int16_t* data, size_t samples, uint32_t rate, bool blocking = false);
    bool playWav(const uint8_t* wav, size_t len);
    bool playing() const;
    void stopPlayback();
    void tick();
    void setVolume(uint8_t vol0to100);
    void blip(uint16_t freq = 880, uint32_t ms = 60);

private:
    void chainTick();
    int16_t* _pcm = nullptr;
    size_t _recLen = 0;
    volatile bool _rec = false;
    int16_t* _ring = nullptr;
    static constexpr size_t kRingSamples = 128 * 1024;
    size_t _rPos = 0, _wPos = 0;
    uint32_t _rate = 16000;
    volatile bool _pbActive = false;
    volatile bool _streamActive = false;
    uint32_t _prevSec = 0;
    int16_t* _smic = nullptr;
    static constexpr size_t kSmicSize = 9600;
    volatile size_t _smicW = 0, _smicR = 0;
    volatile bool _smicOn = false;
    SemaphoreHandle_t _mux = nullptr;
};

extern AudioIO audio;
// M5.Speaker reference alias (keeps headers light)
#include <M5Unified.h>
#define M5Spe M5.Speaker
