#pragma once
#include <string>
#include <cstdint>

enum class Screen : uint8_t { Face = 0, Chat, Pi, Cam, Settings, Count };

// Screen manager: nav bar, status bar, per-screen draw + touch routing.
class Ui {
public:
    void begin();
    void tick();
    void setScreen(Screen s);
    Screen screen() const { return _screen; }
    void invalidate() { _dirty = true; }
    bool keyboardOpen();
    void toast(const std::string& msg, uint32_t ms = 1500);

    static constexpr int W = 320, H = 240;
    static constexpr int NAV_H = 30, BAR_H = 18;
    static constexpr int CONTENT_Y = BAR_H;
    static constexpr int CONTENT_H = H - BAR_H - NAV_H;

private:
    void drawBar();
    void drawNav();
    bool handleNav();
    int _navX = -1, _navY = -1;
    Screen _screen = Screen::Face;
    bool _dirty = true;
    uint32_t _toastUntil = 0;
    std::string _toast;
};

extern Ui ui;
