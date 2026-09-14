#include "keyboard.h"
#include <M5Unified.h>
#include <cctype>
#include <cstring>

Keyboard kbd;

static const char* R_DIGITS = "1234567890-/";
static const char* R1 = "qwertyuiop";
static const char* R2 = "asdfghjkl";
static const char* R3 = "zxcvbnm";
static constexpr int KW = 32, KH = 26;

void Keyboard::begin(KeyCb cb, KeyCb specialCb) { _cb = cb; _specialCb = specialCb; }

void Keyboard::open(bool terminalMode) {
    _open = true;
    _terminal = terminalMode;
    _ctrl = _shift = false;
    _y0 = 240 - H;
    draw();
}

void Keyboard::close() { _open = false; M5.Display.fillRect(0, _y0, 320, H, 0x0000); }

static void drawKey(int x, int y, int w, const char* label, bool active) {
    auto& d = M5.Display;
    d.fillRect(x + 1, y + 1, w - 2, KH - 2, active ? 0x33B2 : 0x18E3);
    d.drawRect(x + 1, y + 1, w - 2, KH - 2, 0x4228);
    d.setTextColor(active ? 0xFFFF : 0xC618, active ? 0x33B2 : 0x18E3);
    d.setTextSize(1);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString(label, x + w / 2, y + KH / 2 + 1);
    d.setTextDatum(textdatum_t::top_left);
}

void Keyboard::draw() {
    if (!_open) return;
    auto& d = M5.Display;
    auto dch = [](int x, int y, char c, bool up) {
        char s[2] = {(char)(up ? toupper((unsigned char)c) : c), 0};
        drawKey(x, y, KW, s, false);
    };
    d.fillRect(0, _y0, 320, H, 0x0000);
    int y = _y0 + 2;
    drawKey(0, y, KW, "123", false);
    for (int i = 0; i < 10; i++) dch(KW + i * KW, y, R_DIGITS[i], false);
    y += KH;
    for (int i = 0; i < 10; i++) dch(i * KW, y, R1[i], _shift);
    y += KH;
    drawKey(0, y, KW / 2, "^", _shift);
    for (int i = 0; i < 9; i++) dch(KW / 2 + i * KW, y, R2[i], _shift);
    y += KH;
    for (int i = 0; i < 7; i++) dch(i * KW, y, R3[i], _shift);
    drawKey(7 * KW, y, KW + 16, "<-", false);
    y += KH;
    int x = 0;
    drawKey(x, y, 28, "C", _ctrl); x += 28;
    drawKey(x, y, 28, "T", false); x += 28;
    drawKey(x, y, 28, "E", false); x += 28;
    drawKey(x, y, 24, "<", false); x += 24;
    drawKey(x, y, 24, "v", false); x += 24;
    drawKey(x, y, 24, "^", false); x += 24;
    drawKey(x, y, 24, ">", false); x += 24;
    drawKey(x, y, 320 - x - 56, "space", false); x += 320 - x - 56;
    drawKey(x, y, 28, "OK", false); x += 28;
    drawKey(x, y, 28, "X", false);
}

bool Keyboard::handle(int x, int y, bool clicked) {
    if (!_open || !clicked) return false;
    int row = (y - _y0) / KH;
    if (y < _y0) return false;
    auto sp = [&](uint8_t s) { if (_specialCb) _specialCb(nullptr, s); };
    auto ch = [&](char c) {
        if (_ctrl && c >= 'a' && c <= 'z') {
            char cc = (char)(c - 'a' + 1);
            char s2[2] = {cc, 0};
            if (_cb) _cb(s2, 1);
            _ctrl = false;
            draw();
            return;
        }
        char s2[2] = {(char)(_shift ? toupper((unsigned char)c) : c), 0};
        if (_cb) _cb(s2, 1);
        if (_shift) { _shift = false; draw(); }
    };
    int col = x / KW;
    switch (row) {
        case 0:
            if (x < KW) { sp(Close); return true; }
            if (col - 1 >= 0 && col - 1 < 10) ch(R_DIGITS[col - 1]);
            return true;
        case 1:
            if (col < 10) ch(R1[col]);
            return true;
        case 2:
            if (x < KW / 2) { _shift = !_shift; draw(); return true; }
            col = (x - KW / 2) / KW;
            if (col < 9) ch(R2[col]);
            return true;
        case 3:
            if (col < 7) { ch(R3[col]); return true; }
            if (x >= 7 * KW) { sp(Backspace); return true; }
            return true;
        case 4:
            if (x < 28) { _ctrl = !_ctrl; draw(); return true; }
            if (x < 56) { sp(Tab); return true; }
            if (x < 84) { sp(Esc); return true; }
            if (x < 108) { sp(Left); return true; }
            if (x < 132) { sp(Down); return true; }
            if (x < 156) { sp(Up); return true; }
            if (x < 180) { sp(Right); return true; }
            if (x < 320 - 56) { ch(' '); return true; }
            if (x < 320 - 28) { sp(Enter); return true; }
            sp(Close);
            return true;
        default:
            return false;
    }
}
