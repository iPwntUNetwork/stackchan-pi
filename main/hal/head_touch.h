#pragma once
#include <cstdint>

// Si12T capacitive head-touch sensor (@0x68 internal I2C), 3 zones.
// Petting the head -> happy reaction; holding -> push-to-talk.
class HeadTouch {
public:
    bool probe();
    bool present() const { return _ok; }
    void tick();  // ~10 Hz
    enum Event : uint8_t { None = 0, Pet = 1, HoldTalk = 2 };
    Event consumeEvent() { Event e = _evt; _evt = None; return e; }
    bool holdActive() const { return _holdActive; }
private:
    bool _ok = false;
    uint32_t _touchStart = 0;
    bool _petted = false;
    Event _evt = None;
    bool _holdActive = false;
};

extern HeadTouch headTouch;
