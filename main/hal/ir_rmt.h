#pragma once
#include <cstdint>
#include <cstddef>
#include <driver/rmt_types.h>

// IR remote via RMT (external IR LED on PORT.C; CoreS3 has none built in).
//  - NEC send (covers most remotes)
//  - raw learn: captures whatever the RX unit sees, tries NEC decode, and can
//    replay the exact raw timing for other protocols.
class IrRmt {
public:
    void begin();
    bool sendNec(uint64_t value, uint8_t bits);
    bool sendRaw();                        // replay last learned frame
    bool startLearn();                     // arms RX for 5 s
    // result of learning: nec valid OR raw replay available
    bool pollLearn(uint64_t& value, uint8_t& bits, bool& nec, bool& rawReady);
    bool rxReady() const { return _rxPin >= 0; }
private:
    static void rxTaskTramp(void*);
    void rxTask();
    int _txPin = -1, _rxPin = -1;
    void* _txChan = nullptr;               // rmt_channel_handle_t
    void* _copyEnc = nullptr;              // rmt_encoder_handle_t
    rmt_symbol_word_t _rawSymbols[160];
    size_t _rawCount = 0;
    bool _rawReady = false;
    volatile bool _armed = false;
    uint32_t _armUntil = 0;
};

extern IrRmt irRmt;
