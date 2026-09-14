#include "ui.h"
#include "screens.h"
#include "keyboard.h"
#include "../config_store.h"
#include "../llm/chat_engine.h"
#include "../net/pi_link.h"
#include "../net/wifi_mgr.h"
#include <M5Unified.h>
#include <esp_timer.h>
#include <cstring>

Ui ui;
#include "../hal/audio_io.h"

void Ui::begin() {
    auto& d = M5.Display;
    d.setRotation(1);
    d.fillScreen(0x0000);
}

void Ui::setScreen(Screen s) {
    if (s == _screen) return;
    if (kbd.isOpen()) kbd.close();
    _screen = s;
    _dirty = true;
}

bool Ui::keyboardOpen() { return kbd.isOpen(); }

void Ui::toast(const std::string& msg, uint32_t ms) {
    _toast = msg;
    _toastUntil = (uint32_t)(esp_timer_get_time() / 1000LL) + ms;
    _dirty = true;
}

bool Ui::handleNav() {
    if (_navX < 0) return false;
    if (_navY < H - NAV_H) return false;
    int w = W / (int)Screen::Count;
    int idx = _navX / w;
    if (idx >= 0 && idx < (int)Screen::Count) {
        setScreen((Screen)idx);
        return true;
    }
    return false;
}

void Ui::drawBar() {
    auto& d = M5.Display;
    d.fillRect(0, 0, W, BAR_H, 0x0000);
    d.drawFastHLine(0, BAR_H, W, 0x2127);
    d.setTextSize(1);
    if (wifi.connected()) {
        d.setTextColor(0x07E0, 0x0000);
        d.drawString("WiFi", 4, 5);
    } else if (wifi.state() == WifiState::ApPortal) {
        d.setTextColor(0xFD20, 0x0000);
        d.drawString(("AP:" + wifi.apSsid()).c_str(), 4, 5);
    } else {
        d.setTextColor(0xF800, 0x0000);
        d.drawString("WiFi x", 4, 5);
    }
    static const char* P[] = {"OpenAI", "Gemini", "XiaoZhi"};
    d.setTextColor(0x055A, 0x0000);
    d.drawString(P[cfg.provider % 3], 60, 5);
    d.setTextColor(0x8410, 0x0000);
    d.drawString(chat.statusText().c_str(), 130, 5);
    d.setTextColor(pi.terminalConnected() ? 0x07E0 : 0x4228, 0x0000);
    d.drawString("Pi", W - 78, 5);
    int bat = M5.Power.getBatteryLevel();
    d.setTextColor(bat > 20 ? 0x07E0 : 0xF800, 0x0000);
    d.drawString((std::to_string(bat) + "%").c_str(), W - 46, 5);
}

void Ui::drawNav() {
    auto& d = M5.Display;
    d.fillRect(0, H - NAV_H, W, NAV_H, 0x1082);
    d.drawFastHLine(0, H - NAV_H, W, 0x2127);
    static const char* N[] = {"Face", "Chat", "Pi", "Cam", "Set"};
    int w = W / (int)Screen::Count;
    d.setTextSize(1);
    for (int i = 0; i < (int)Screen::Count; i++) {
        bool on = (i == (int)_screen);
        d.setTextColor(on ? 0x05DF : 0x630C, 0x1082);
        d.setTextDatum(textdatum_t::middle_center);
        d.drawString(N[i], i * w + w / 2, H - NAV_H / 2);
        d.setTextDatum(textdatum_t::top_left);
    }
}

void Ui::tick() {
    auto& d = M5.Display;
    bool pressed = M5.Touch.getCount() > 0;
    static bool wasPressed = false;
    static int lx = -1, ly = -1;
    int x = -1, y = -1;
    if (pressed) {
        auto t = M5.Touch.getDetail();
        x = t.x;
        y = t.y;
    }
    bool clicked = wasPressed && !pressed;
    if (pressed) { lx = x; ly = y; }

    if (kbd.isOpen()) {
        if (pressed || clicked) kbd.handle(pressed ? x : lx, pressed ? y : ly, clicked);
        if (!kbd.isOpen()) _dirty = true;
        wasPressed = pressed;
        return;
    }
    _navX = lx; _navY = ly;
    if (clicked && handleNav()) { _navX = -1; wasPressed = pressed; return; }
    _navX = -1;

    switch (_screen) {
        case Screen::Face: scrFaceTouch(x, y, pressed, clicked); break;
        case Screen::Chat: scrChatTouch(x, y, pressed, clicked); break;
        case Screen::Pi: scrPiTouch(x, y, pressed, clicked); break;
        case Screen::Cam: scrCamTouch(x, y, pressed, clicked); break;
        case Screen::Settings: scrSettingsTouch(x, y, pressed, clicked); break;
        default: break;
    }
    wasPressed = pressed;

    bool dirty = _dirty;
    _dirty = false;
    switch (_screen) {
        case Screen::Face: scrFaceDraw(dirty); break;
        case Screen::Chat: scrChatDraw(dirty); break;
        case Screen::Pi: scrPiDraw(dirty); break;
        case Screen::Cam: scrCamDraw(dirty); break;
        case Screen::Settings: scrSettingsDraw(dirty); break;
        default: break;
    }

    static uint32_t lastBar = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (now - lastBar > 1000) {
        lastBar = now;
        drawBar();
        drawNav();
    }
    if (!_toast.empty() && now < _toastUntil) {
        d.fillRect(40, CONTENT_Y + 8, 240, 20, 0x33B2);
        d.drawRect(40, CONTENT_Y + 8, 240, 20, 0xFFFF);
        d.setTextColor(0xFFFF, 0x33B2);
        d.setTextDatum(textdatum_t::middle_center);
        d.drawString(_toast.c_str(), 160, CONTENT_Y + 18);
        d.setTextDatum(textdatum_t::top_left);
    } else if (!_toast.empty()) {
        _toast = "";
        _dirty = true;
    }
}
