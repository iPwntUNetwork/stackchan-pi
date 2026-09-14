#pragma once
#include <functional>
#include <cstdint>

// On-screen keyboard with terminal extras (Ctrl/Tab/Esc/arrows).
class Keyboard {
public:
    typedef std::function<void(const char* s, uint8_t len)> KeyCb;
    enum Special : uint8_t { Backspace, Enter, Ctrl, Tab, Esc, Up, Down, Left, Right, Shift, Close };

    void begin(KeyCb cb, KeyCb specialCb);
    void draw();
    bool handle(int x, int y, bool clicked);
    void open(bool terminalMode = false);
    void close();
    bool isOpen() const { return _open; }
    static constexpr int H = 5 * 26 + 4;
private:
    KeyCb _cb = nullptr, _specialCb = nullptr;
    bool _open = false;
    bool _terminal = false;
    bool _ctrl = false;
    bool _shift = false;
    int _y0 = 0;
};

extern Keyboard kbd;
