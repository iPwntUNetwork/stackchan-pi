#include "audio_io.h"
#include "fw_config.h"
#include "config_store.h"
#include <cstring>
#include <esp_heap_caps.h>
#include <freertos/task.h>

AudioIO audio;

bool AudioIO::begin() {
    _mux = xSemaphoreCreateMutex();
    _pcm = (int16_t*)heap_caps_malloc(MIC_MAX_SECONDS * MIC_RATE * 2, MALLOC_CAP_SPIRAM);
    _ring = (int16_t*)heap_caps_malloc(kRingSamples * 2, MALLOC_CAP_SPIRAM);
    _smic = (int16_t*)heap_caps_malloc(kSmicSize * 2, MALLOC_CAP_SPIRAM);
    if (!_pcm || !_ring || !_smic) return false;
    M5.Speaker.setVolume(cfg.volume * 255 / 100);
    M5.Speaker.begin();
    auto mc = M5.Mic.config();
    mc.sample_rate = MIC_RATE;
    mc.magnification = 16;
    M5.Mic.config(mc);
    M5.Mic.begin();
    return true;
}

void AudioIO::setVolume(uint8_t vol) { M5.Speaker.setVolume(vol * 255 / 100); }

void AudioIO::blip(uint16_t freq, uint32_t ms) {
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(100)) != pdTRUE) return;
    M5.Speaker.tone(freq, ms);
    xSemaphoreGive(_mux);
}

// ---------- recording ----------

bool AudioIO::playing() const { return _pbActive || M5.Speaker.isRunning(); }

bool AudioIO::startRecord() {
    if (_rec || !_pcm || !M5.Mic.isEnabled()) return false;
    stopPlayback();
    _recLen = 0;
    _rec = true;
    return true;
}

bool AudioIO::stopRecord() {
    if (!_rec) return false;
    _rec = false;
    return _recLen > 0;
}

uint32_t AudioIO::recordMs() const { return (uint32_t)(_recLen * 1000ULL / MIC_RATE); }

static size_t writeWavHeader(uint8_t* dst, uint32_t samples, uint32_t rate) {
    uint32_t dataSize = samples * 2;
    uint32_t fileSize = 36 + dataSize;
    memcpy(dst, "RIFF", 4); memcpy(dst + 4, &fileSize, 4); memcpy(dst + 8, "WAVE", 4);
    memcpy(dst + 12, "fmt ", 4);
    uint32_t fmtLen = 16; memcpy(dst + 16, &fmtLen, 4);
    uint16_t af = 1, ch = 1; memcpy(dst + 20, &af, 2); memcpy(dst + 22, &ch, 2);
    uint32_t sr = rate; memcpy(dst + 24, &sr, 4);
    uint32_t br = rate * 2; memcpy(dst + 28, &br, 4);
    uint16_t ba = 2, bb = 16; memcpy(dst + 32, &ba, 2); memcpy(dst + 34, &bb, 2);
    memcpy(dst + 36, "data", 4); memcpy(dst + 40, &dataSize, 4);
    return 44;
}

uint8_t* AudioIO::takeWav(size_t& len) {
    if (!_recLen) { len = 0; return nullptr; }
    uint8_t* wav = (uint8_t*)heap_caps_malloc(44 + _recLen * 2, MALLOC_CAP_SPIRAM);
    if (!wav) { len = 0; return nullptr; }
    writeWavHeader(wav, _recLen, MIC_RATE);
    memcpy(wav + 44, _pcm, _recLen * 2);
    len = 44 + _recLen * 2;
    return wav;
}

// ---------- playback ----------

bool AudioIO::streamStart(uint32_t rate) {
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    M5.Speaker.stop();
    _rate = rate;
    _rPos = _wPos = 0;
    _prevSec = 0;
    _streamActive = true;
    _pbActive = true;
    xSemaphoreGive(_mux);
    return true;
}

bool AudioIO::streamFeed(const int16_t* data, size_t samples) {
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    size_t avail = kRingSamples - (_wPos - _rPos);
    if (samples > avail) samples = avail;
    size_t wmod = _wPos % kRingSamples;
    size_t first = kRingSamples - wmod;
    if (first > samples) first = samples;
    memcpy(_ring + wmod, data, first * 2);
    if (samples > first) memcpy(_ring, data + first, (samples - first) * 2);
    _wPos += samples;
    xSemaphoreGive(_mux);
    return true;
}

bool AudioIO::streamEnd() {
    _streamActive = false;
    return true;
}

void AudioIO::stopPlayback() {
    if (xSemaphoreTake(_mux, pdMS_TO_TICKS(300)) != pdTRUE) return;
    _pbActive = false;
    _streamActive = false;
    _rPos = _wPos;
    M5.Speaker.stop();
    xSemaphoreGive(_mux);
}

bool AudioIO::playPcm(const int16_t* data, size_t samples, uint32_t rate, bool blocking) {
    if (!data || !samples || !M5.Speaker.isEnabled()) return false;
    if (!streamStart(rate)) return false;
    streamFeed(data, samples);
    streamEnd();
    if (blocking) {
        while (playing()) vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

bool AudioIO::playWav(const uint8_t* wav, size_t len) {
    if (len < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;
    uint32_t rate = 16000;
    int64_t dataOff = -1; uint32_t dataLen = 0;
    size_t p = 12;
    while (p + 8 <= len) {
        uint32_t clen; memcpy(&clen, wav + p + 4, 4);
        if (!memcmp(wav + p, "fmt ", 4) && clen >= 16) {
            uint16_t af, bits;
            memcpy(&af, wav + p + 8, 2);
            memcpy(&bits, wav + p + 22, 2);
            memcpy(&rate, wav + p + 12, 4);
            if (af != 1 || bits != 16) return false;
        } else if (!memcmp(wav + p, "data", 4)) {
            dataOff = p + 8;
            dataLen = clen;
            if (dataOff + dataLen > (int64_t)len) dataLen = len - dataOff;
            break;
        }
        p += 8 + clen + (clen & 1);
    }
    if (dataOff < 0 || dataLen < 2) return false;
    return playPcm((const int16_t*)(wav + dataOff), dataLen / 2, rate);
}

void AudioIO::chainTick() {
    if (!_pbActive) return;
    if (xSemaphoreTake(_mux, 0) != pdTRUE) return;
    uint64_t consumed = (uint64_t)_prevSec * _rate;
    bool drained = !_streamActive && consumed >= _wPos && !M5.Speaker.isRunning();
    if (drained) { _pbActive = false; xSemaphoreGive(_mux); return; }
    size_t avail = (size_t)(_wPos - (size_t)consumed);
    if (avail == 0) { xSemaphoreGive(_mux); return; }
    const size_t chunk = _rate / 5;
    if (avail > chunk) avail = chunk;
    size_t rmod = (size_t)consumed % kRingSamples;
    size_t contig = kRingSamples - rmod;
    if (contig < avail) avail = contig;
    if (avail < _rate / 20 && _streamActive) { xSemaphoreGive(_mux); return; }
    _prevSec = M5.Speaker.playRaw(_ring + rmod, avail, _rate, false, _prevSec, true);
    xSemaphoreGive(_mux);
}

// ---------- mic streaming ----------

bool AudioIO::startStreamMic() {
    if (!M5.Mic.isEnabled() || !_smic) return false;
    stopPlayback();
    _smicR = 0;
    _smicW = 0;
    _smicOn = true;
    return true;
}

void AudioIO::stopStreamMic() { _smicOn = false; }

size_t AudioIO::streamDrain(int16_t* dst, size_t maxSamples) {
    size_t n = 0;
    while (n < maxSamples) {
        if (_smicR == _smicW) break;
        dst[n++] = _smic[_smicR];
        _smicR = (_smicR + 1) % kSmicSize;
    }
    return n;
}

void AudioIO::tick() {
    if (_smicOn) {
        if (M5.Mic.isEnabled()) {
            M5.Mic.record(_smic + _smicW, 1600, MIC_RATE);
            _smicW += 1600;
            if (_smicW >= kSmicSize) _smicW = 0;
        } else {
            _smicOn = false;
        }
    } else if (_rec) {
        if (!_pcm || _recLen + MIC_RATE / 10 > MIC_MAX_SECONDS * MIC_RATE) {
            if (_recLen + MIC_RATE / 10 > MIC_MAX_SECONDS * MIC_RATE) _rec = false;
            return;
        }
        if (M5.Mic.isEnabled()) {
            M5.Mic.record(_pcm + _recLen, MIC_RATE / 10, MIC_RATE);
            _recLen += MIC_RATE / 10;
        } else _rec = false;
    }
    chainTick();
}
