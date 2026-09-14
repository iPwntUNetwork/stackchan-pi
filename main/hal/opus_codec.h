#pragma once
#include <cstdint>
#include <cstddef>

// Thin shim over espressif/esp_audio_codec's Opus for the xiaozhi voice path.
bool opusEncInit();                                        // 16 kHz mono VOIP
int  opusEncode(const int16_t* pcm, size_t samples,        // 960 @16k
                uint8_t* out, size_t outCap);              // returns pkt size or -1
bool opusDecInit(uint32_t sampleRate);                     // server rate (16k/24k)
int  opusDecode(const uint8_t* pkt, size_t len,            // returns samples or -1
                int16_t* out, size_t outCapSamples);
