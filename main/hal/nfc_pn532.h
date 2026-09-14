#pragma once
#include <cstdint>
#include <string>

// Minimal PN532 NFC reader over I2C (M5 NFC unit on PORT.B).
// Supports: firmware probe, SAM config, MIFARE ISO14443A UID polling.
class NfcPn532 {
public:
    bool begin();                     // returns true if PN532 answers
    bool present() const { return _ok; }
    void tick();                      // poll ~2 Hz
    bool hasTag() const { return _fresh; }
    std::string uidHex() const { return _uid; }
    uint32_t tagTs() const { return _ts; }
    void consume() { _fresh = false; }
private:
    bool writeCmd(const uint8_t* cmd, size_t len);
    int readResp(uint8_t* buf, size_t cap, uint8_t expectCmd);
    bool _ok = false;
    bool _fresh = false;
    std::string _uid;
    uint32_t _ts = 0, _lastPoll = 0;
    void* _dev = nullptr;             // i2c_master_dev_handle_t
};

extern NfcPn532 nfc;
