#include "screens.h"
#include "keyboard.h"
#include "face.h"
#include "../config_store.h"
#include "../llm/chat_engine.h"
#include "../llm/xiaozhi_client.h"
#include "../net/pi_link.h"
#include "../net/wifi_mgr.h"
#include "../hal/camera_feed.h"
#include "../hal/motion.h"
#include "../hal/audio_io.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdio>

// ---------------- shared helpers ----------------

static void pipDraw(int px, int py, int pw, int ph) {
    if (!camera.ready()) return;
    static uint16_t* pip = nullptr;
    if (!pip) pip = (uint16_t*)heap_caps_malloc(pw * ph * 2, MALLOC_CAP_SPIRAM);
    if (!pip) return;
    if (!camera.samplePip(pip, pw, ph)) return;
    M5.Display.pushImage(px, py, pw, ph, pip);
}

static const uint16_t C_BTN = 0x18E3, C_BTN_ON = 0x33B2, C_BG = 0x0000;

static void btn(int x, int y, int w, int h, const char* label, bool active) {
    auto& d = M5.Display;
    d.fillRect(x, y, w, h, active ? C_BTN_ON : C_BTN);
    d.drawRect(x, y, w, h, 0x4228);
    d.setTextColor(0xFFFF, active ? C_BTN_ON : C_BTN);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString(label, x + w / 2, y + h / 2);
    d.setTextDatum(textdatum_t::top_left);
}

static bool hit(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

// ================= FACE =================

static bool faceTalkHeld = false;

void scrFaceDraw(bool dirty) {
    auto& d = M5.Display;
    face.render(160, 96);
    if (cfg.cam_overlay) pipDraw(320 - 100, Ui::BAR_H + 4, 96, 72);
    d.fillRect(90, 178, 140, 26, faceTalkHeld ? 0xF800 : C_BTN);
    d.drawRect(90, 178, 140, 26, 0x4228);
    d.setTextColor(0xFFFF, faceTalkHeld ? 0xF800 : C_BTN);
    d.setTextDatum(textdatum_t::middle_center);
    std::string lbl = cfg.provider == 2
        ? (faceTalkHeld ? "LISTENING..." : "HOLD TO TALK")
        : (faceTalkHeld ? "REC " + std::to_string(audio.recordMs() / 1000) + "s" : "HOLD TO TALK");
    d.drawString(lbl.c_str(), 160, 191);
    d.setTextDatum(textdatum_t::top_left);
}

void scrFaceTouch(int x, int y, bool pressed, bool clicked) {
    bool inZone = y >= 174 && y < 208 && x >= 80 && x < 240;
    if (pressed && inZone && !faceTalkHeld) {
        faceTalkHeld = true;
        if (cfg.provider == 2) chat.xzPushToTalk(true);
        else chat.startVoice();
    } else if (!pressed && faceTalkHeld) {
        faceTalkHeld = false;
        if (cfg.provider == 2) chat.xzPushToTalk(false);
        else chat.stopVoice();
    }
    if (pressed && !inZone && x >= 0) {
        float gdx = (x - 160) / 160.0f;
        float gdy = (y - 110) / 120.0f;
        face.setGaze(gdx, gdy);
    }
}

// ================= CHAT =================

static std::string chatInput;
static bool chatPhoto = false;

static void chatLogRender() {
    auto& d = M5.Display;
    d.fillRect(0, Ui::BAR_H, 320, Ui::CONTENT_H - 30, C_BG);
    d.setTextSize(1);
    d.setTextColor(0x8410, C_BG);
    d.drawString(("[" + std::string(cfg.provider == 1 ? "gemini" : cfg.provider == 2 ? "xiaozhi" : "openai") + "] " +
                     chat.statusText()).c_str(), 4, Ui::BAR_H + 2);

    std::string body;
    chat.lockHist();
    for (auto& m : chat.history.msgs) {
        body += (m.role == "user" ? "\x01You: " : "\x02Chan: ") + m.text + "\n";
    }
    chat.unlockHist();
    if (!chat.partial().empty()) body += "\x02" + chat.partial();

    std::string lines[16];
    int nlines = 0;
    size_t start = 0;
    while (start < body.size() && nlines < 15) {
        size_t nl = body.find('\n', start);
        size_t end = (nl == std::string::npos) ? body.size() : nl;
        size_t pos = start;
        while (pos < end && nlines < 15) {
            size_t chunk = (end - pos > 50) ? 50 : end - pos;
            lines[nlines++] = body.substr(pos, chunk);
            pos += chunk;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    int shown = nlines > 11 ? 11 : nlines;
    for (int i = 0; i < shown; i++) {
        const std::string& L = lines[nlines - shown + i];
        uint16_t color = 0xC618;
        if (L.size() && (unsigned char)L[0] == 1) color = 0x005A;
        else if (L.size() && (unsigned char)L[0] == 2) color = 0x07E0;
        std::string clean = L;
        if (!clean.empty() && (unsigned char)clean[0] <= 2) clean.erase(0, 1);
        d.setTextColor(color, C_BG);
        d.drawString(clean.c_str(), 2, Ui::BAR_H + 14 + i * 14);
    }
}

void scrChatDraw(bool dirty) {
    static std::string lastStatus;
    std::string st = chat.statusText();
    if (dirty || st != lastStatus || !chat.partial().empty()) {
        lastStatus = st;
        chatLogRender();
    }
    auto& d = M5.Display;
    int y = 240 - Ui::NAV_H - 28;
    d.fillRect(0, y, 320, 28, 0x101018);
    btn(2, y + 2, 44, 24, "MIC", audio.recording());
    btn(50, y + 2, 34, 24, "PIC", chatPhoto);
    d.fillRect(88, y + 2, 228, 24, 0x0000);
    d.drawRect(88, y + 2, 228, 24, 0x4228);
    d.setTextColor(0xFFFF, 0x0000);
    d.drawString(chatInput.empty() ? "tap to type..." : (chatInput + "_").c_str(), 92, y + 9);
    if (cfg.provider == 2 && xz.state() == XiaozhiClient::State::Off) {
        btn(240, Ui::BAR_H + 14, 76, 22, "Connect", false);
    }
}

void scrChatTouch(int x, int y, bool pressed, bool clicked) {
    if (!clicked) return;
    int rowY = 240 - Ui::NAV_H - 28;
    if (hit(x, y, 2, rowY + 2, 44, 24)) {
        if (cfg.provider == 2) { ui.toast("Use PTT (xiaozhi)"); return; }
        if (audio.recording()) chat.stopVoice();
        else chat.startVoice();
        return;
    }
    if (hit(x, y, 50, rowY + 2, 34, 24)) {
        chatPhoto = !chatPhoto;
        ui.invalidate();
        return;
    }
    if (cfg.provider == 2 && hit(x, y, 240, Ui::BAR_H + 14, 76, 22)) {
        if (cfg.xz_url[0] && cfg.xz_token[0]) chat.xzConnect();
        else ui.toast("Set xiaozhi url+token");
        return;
    }
    if (hit(x, y, 88, rowY + 2, 228, 24)) {
        kbd.open(false);
        return;
    }
}

// ================= PI TERMINAL =================

void scrPiDraw(bool dirty) {
    auto& d = M5.Display;
    d.fillRect(0, Ui::BAR_H, 320, Ui::CONTENT_H, C_BG);
    d.fillRect(0, Ui::BAR_H, 320, 20, 0x101018);
    d.setTextColor(pi.terminalConnected() ? 0x07E0 : 0xF800, 0x101018);
    std::string host(cfg.pi_host);
    d.drawString((host + ":" + std::to_string(cfg.pi_ttyd_port) +
                     (pi.terminalConnected() ? "  [live]" : "  [down]")).c_str(), 3, Ui::BAR_H + 6);
    btn(230, Ui::BAR_H + 22, 86, 22, pi.terminalConnected() ? "DISCONNECT" : "CONNECT", false);
    if (pi.agentReachable()) {
        d.setTextColor(0x07E0, 0x101018);
        d.drawString("agent OK", 130, Ui::BAR_H + 6);
    }
    if (!pi.terminalConnected()) {
        d.setTextColor(0x8410, C_BG);
        d.drawString("Connect to the Pi's ttyd server for a", 6, Ui::BAR_H + 52);
        d.drawString("full root shell, or use the web UI.", 6, Ui::BAR_H + 64);
        return;
    }
    static char scr[PiLink::ROWS][PiLink::COLS + 1];
    bool fresh = dirty || pi.newOutput();
    static uint32_t lastRepaint = 0;
    if (fresh || millis32() - lastRepaint > 400) {
        lastRepaint = millis32();
        pi.copyScreen(scr);
        d.setTextSize(1);
        for (int r = 0; r < PiLink::ROWS; r++) {
            d.setTextColor(0xC618, 0x0000);
            int y = Ui::BAR_H + 48 + r * 8;
            if (y > 240 - Ui::NAV_H - 34) break;
            char padded[PiLink::COLS + 2];
            memcpy(padded, scr[r], PiLink::COLS);
            int len = strlen(padded);
            memset(padded + len, ' ', PiLink::COLS - len);
            padded[PiLink::COLS] = 0;
            d.drawString(padded, 2, y);
        }
        int ky = 240 - Ui::NAV_H - 30;
        btn(2, ky, 40, 24, "C-c", false);
        btn(46, ky, 40, 24, "Tab", false);
        btn(90, ky, 40, 24, "^", false);
        btn(134, ky, 40, 24, "Ent", false);
        btn(178, ky, 70, 24, "KB", false);
        btn(252, ky, 66, 24, "Clear", false);
    }
}

void scrPiTouch(int x, int y, bool pressed, bool clicked) {
    if (!clicked) return;
    if (hit(x, y, 230, Ui::BAR_H + 22, 86, 22)) {
        if (pi.terminalConnected()) pi.disconnectTerminal();
        else pi.connectTerminal();
        ui.invalidate();
        return;
    }
    if (!pi.terminalConnected()) return;
    int ky = 240 - Ui::NAV_H - 30;
    if (hit(x, y, 2, ky, 40, 24)) { pi.sendSpecial(PiLink::KeyCtrlC); return; }
    if (hit(x, y, 46, ky, 40, 24)) { pi.sendSpecial(PiLink::KeyTab); return; }
    if (hit(x, y, 90, ky, 40, 24)) { pi.sendSpecial(PiLink::KeyUp); return; }
    if (hit(x, y, 134, ky, 40, 24)) { pi.sendSpecial(PiLink::KeyEnter); return; }
    if (hit(x, y, 178, ky, 70, 24)) { kbd.open(true); return; }
    if (hit(x, y, 252, ky, 66, 24)) {
        pi.sendKeys("clear");
        pi.sendSpecial(PiLink::KeyEnter);
        return;
    }
}

// ================= CAMERA =================

void scrCamDraw(bool dirty) {
    if (camera.ready()) {
        camera.pushToDisplay(0, 0, 320, 240);
    } else {
        M5.Display.fillRect(0, 0, 320, 240, 0x0000);
        M5.Display.setTextColor(0xF800);
        M5.Display.drawString("camera offline", 110, 110);
    }
    auto& d = M5.Display;
    d.fillRect(0, 150, 320, 90, 0x0000);
    float yaw, pitch;
    motion.getYawPitch(yaw, pitch);
    char buf[48];
    snprintf(buf, sizeof(buf), "yaw %.0f  pitch %.0f  %.1ffps", yaw, pitch, camera.fps());
    d.setTextColor(0x05DF, 0x0000);
    d.drawString(buf, 6, 152);
    d.drawRect(6, 166, 308, 16, 0x4228);
    int px = 6 + (int)((yaw + 90) / 180.0f * 306);
    d.fillRect(px - 2, 166, 5, 16, 0x05DF);
    d.drawRect(6, 186, 308, 16, 0x4228);
    int ppx = 6 + (int)((float)(pitch - cfg.pitch_min) / (cfg.pitch_max - cfg.pitch_min) * 306);
    d.fillRect(ppx - 2, 186, 5, 16, 0x05DF);
    btn(6, 208, 100, 24, "SNAP+ASK", false);
    btn(112, 208, 90, 24, "CENTER", false);
    d.setTextColor(0x8410, 0x0000);
    d.drawString(("http://" + wifi.ip() + "/stream").c_str(), 208, 216);
}

void scrCamTouch(int x, int y, bool pressed, bool clicked) {
    if (!clicked) return;
    if (y >= 166 && y < 182) {
        float yaw = (x - 6) / 306.0f * 180.0f - 90.0f;
        motion.setTarget(yaw, cfg.home_pitch);
        motion.idleKick();
        return;
    }
    if (y >= 186 && y < 202) {
        float pitch = cfg.pitch_min + (x - 6) / 306.0f * (cfg.pitch_max - cfg.pitch_min);
        float cyaw, cpitch;
        motion.getYawPitch(cyaw, cpitch);
        motion.setTarget(cyaw, pitch);
        motion.idleKick();
        return;
    }
    if (hit(x, y, 6, 208, 100, 24)) {
        chat.submitText("Describe what you see in one short sentence.", true);
        ui.setScreen(Screen::Chat);
        return;
    }
    if (hit(x, y, 112, 208, 90, 24)) { motion.center(); motion.idleKick(); }
}

// ================= SETTINGS =================

void scrSettingsDraw(bool dirty) {
    auto& d = M5.Display;
    d.fillRect(0, Ui::BAR_H, 320, Ui::CONTENT_H, C_BG);
    d.setTextSize(1);
    int by = Ui::BAR_H;
    d.setTextColor(0x005A, C_BG);
    d.drawString("LLM provider:", 4, by + 4);
    btn(90, by, 74, 22, "OpenAI", cfg.provider == 0);
    btn(168, by, 74, 22, "Gemini", cfg.provider == 1);
    btn(246, by, 70, 22, "Xiaozhi", cfg.provider == 2);

    d.setTextColor(0x005A, C_BG);
    d.drawString("Volume:", 4, by + 30);
    d.drawRect(70, by + 28, 180, 16, 0x4228);
    int vx = 70 + (int)(cfg.volume / 100.0f * 178);
    d.fillRect(70, by + 28, vx - 70, 16, 0x2A5A);
    btn(254, by + 28, 62, 16, "TEST", false);

    d.setTextColor(0x005A, C_BG);
    d.drawString("Head:", 4, by + 54);
    btn(50, by + 52, 62, 20, "Center", false);
    btn(116, by + 52, 62, 20, "Yaw -5", false);
    btn(182, by + 52, 62, 20, "Yaw +5", false);
    btn(248, by + 52, 68, 20, "Idle on/off", cfg.idle_motion);

    d.setTextColor(0x005A, C_BG);
    d.drawString("Power:", 4, by + 82);
    btn(50, by + 80, 62, 20, "Torque on", false);
    btn(116, by + 80, 62, 20, "Torque off", false);
    btn(182, by + 80, 80, 20, "Cam on/off", cfg.cam_overlay);
    btn(266, by + 80, 50, 20, "Reboot", false);

    d.setTextColor(0xC618, C_BG);
    d.drawString(("Full config: http://" + wifi.ip() + "/").c_str(), 4, by + 112);
    d.drawString((std::string("Backend: ") + (motion.backend() == ServoBackend::Scs ? "SCS0009 bus (G6/G7)" :
                 motion.backend() == ServoBackend::Pwm ? "PWM servos" : "none")).c_str(), 4, by + 126);
    d.drawString(("WiFi: " + std::string(cfg.wifi_ssid) + "  " + wifi.ip()).c_str(), 4, by + 140);
    if (wifi.state() == WifiState::ApPortal) {
        d.drawString(("AP portal: " + wifi.apSsid() + " pass: stackchan").c_str(), 4, by + 154);
    }
}

void scrSettingsTouch(int x, int y, bool pressed, bool clicked) {
    if (!clicked) return;
    int by = Ui::BAR_H;
    if (hit(x, y, 90, by, 74, 22)) { chat.setProvider(0); ui.toast("OpenAI"); return; }
    if (hit(x, y, 168, by, 74, 22)) { chat.setProvider(1); ui.toast("Gemini"); return; }
    if (hit(x, y, 246, by, 70, 22)) {
        chat.setProvider(2);
        if (cfg.xz_url[0] && cfg.xz_token[0]) chat.xzConnect();
        ui.toast("Xiaozhi");
        return;
    }
    if (hit(x, y, 70, by + 28, 180, 16)) {
        cfg.volume = (x - 70) * 100 / 178;
        audio.setVolume(cfg.volume);
        cfg.save();
        ui.invalidate();
        return;
    }
    if (hit(x, y, 254, by + 28, 62, 16)) { audio.blip(880, 80); return; }
    if (hit(x, y, 50, by + 52, 62, 20)) { motion.center(); motion.idleKick(); return; }
    if (hit(x, y, 116, by + 52, 62, 20)) { float yp, pp; motion.getYawPitch(yp, pp); motion.setTarget(yp - 5, pp); motion.idleKick(); return; }
    if (hit(x, y, 182, by + 52, 62, 20)) { float yp, pp; motion.getYawPitch(yp, pp); motion.setTarget(yp + 5, pp); motion.idleKick(); return; }
    if (hit(x, y, 248, by + 52, 68, 20)) { cfg.idle_motion = !cfg.idle_motion; cfg.save(); ui.invalidate(); return; }
    if (hit(x, y, 50, by + 80, 62, 20)) { motion.setTorque(true); ui.toast("torque on"); return; }
    if (hit(x, y, 116, by + 80, 62, 20)) { motion.setTorque(false); ui.toast("torque off"); return; }
    if (hit(x, y, 182, by + 80, 80, 20)) { cfg.cam_overlay = !cfg.cam_overlay; cfg.save(); ui.invalidate(); return; }
    if (hit(x, y, 266, by + 80, 50, 20)) { vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); return; }
}

// ================= keyboard routing =================

static void kbdText(const char* s, uint8_t len) {
    (void)len;
    if (ui.screen() == Screen::Chat) {
        chatInput += s;
        ui.invalidate();
    } else if (ui.screen() == Screen::Pi) {
        pi.sendKeys(s);
    }
}

static void kbdSpecial(const char* s, uint8_t key) {
    (void)s;
    if (key == Keyboard::Close) { kbd.close(); ui.invalidate(); return; }
    if (ui.screen() == Screen::Pi) {
        switch (key) {
            case Keyboard::Enter: pi.sendSpecial(PiLink::KeyEnter); break;
            case Keyboard::Tab: pi.sendSpecial(PiLink::KeyTab); break;
            case Keyboard::Esc: pi.sendSpecial(PiLink::KeyEsc); break;
            case Keyboard::Up: pi.sendSpecial(PiLink::KeyUp); break;
            case Keyboard::Down: pi.sendSpecial(PiLink::KeyDown); break;
            case Keyboard::Left: pi.sendSpecial(PiLink::KeyLeft); break;
            case Keyboard::Right: pi.sendSpecial(PiLink::KeyRight); break;
            case Keyboard::Backspace: pi.sendKeys("\x7f"); break;
            default: break;
        }
    } else if (ui.screen() == Screen::Chat) {
        if (key == Keyboard::Enter) {
            if (!chatInput.empty()) {
                chat.submitText(chatInput, chatPhoto);
                chatInput = "";
            }
            kbd.close();
            ui.invalidate();
        } else if (key == Keyboard::Backspace) {
            if (!chatInput.empty()) chatInput.pop_back();
            ui.invalidate();
        }
    }
}

void kbdInit() { kbd.begin(kbdText, kbdSpecial); }
