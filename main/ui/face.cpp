#include "face.h"
#include <M5Unified.h>
#include <esp_timer.h>
#include <cstdlib>
#include <cmath>

Face face;
#include "../hal/audio_io.h"

static constexpr uint16_t BG = 0x1082;
static constexpr uint16_t EYE = 0xFFFF;
static constexpr uint16_t PUP = 0x0021;
static constexpr uint16_t MOUTH = 0xFFFF;

static inline uint32_t millisF() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

void Face::setEmotion(Emotion e) { _emotion = e; }
void Face::setGaze(float dx, float dy) {
    _gdx = dx < -1 ? -1 : dx > 1 ? 1 : dx;
    _gdy = dy < -1 ? -1 : dy > 1 ? 1 : dy;
}

void Face::tickSpeech() {
    if (millisF() > _nextFlap) {
        _mouthOpen = !_mouthOpen;
        _nextFlap = millisF() + 90 + rand() % 90;
    }
}

void Face::render(int cx, int cy) {
    auto& d = M5.Display;
    uint32_t now = millisF();
    if (now == _lastFrame) return;
    _lastFrame = now;

    d.fillScreen(BG);
    float breathe = sinf(now / 900.0f) * 1.5f;

    if (now > _nextBlink) { _blinkUntil = now + 110; _nextBlink = now + 2200 + rand() % 4200; }
    bool blinking = now < _blinkUntil;
    _blink += ((blinking ? 0.08f : 1.0f) - _blink) * 0.45f;

    if (now > _nextSaccade) {
        _pdx = (rand() % 201 - 100) / 100.0f * 0.45f;
        _pdy = (rand() % 201 - 100) / 100.0f * 0.35f;
        _nextSaccade = now + 600 + rand() % 2600;
    }
    float gdx = _pdx * 0.7f + _gdx * 0.3f;
    float gdy = _pdy * 0.7f + _gdy * 0.3f;

    Emotion e = _emotion.load();
    int eyeRx = 17, eyeRy = 24;
    if (e == Emotion::Surprised) { eyeRx = 20; eyeRy = 28; }
    if (e == Emotion::Sleepy) eyeRy = 12;
    int openH = (int)(eyeRy * 2 * _blink);
    if (openH < 3) openH = 3;

    int eyeY = cy + (int)breathe;
    int lx = cx - 42, rx = cx + 42;

    if (e == Emotion::Angry) {
        d.drawLine(lx - 16, eyeY - 30, lx + 12, eyeY - 24, MOUTH);
        d.drawLine(rx - 12, eyeY - 24, rx + 16, eyeY - 30, MOUTH);
    } else if (e == Emotion::Sad) {
        d.drawLine(lx - 14, eyeY - 24, lx + 12, eyeY - 30, MOUTH);
        d.drawLine(rx - 12, eyeY - 30, rx + 14, eyeY - 24, MOUTH);
    }

    d.fillEllipse(lx, eyeY, eyeRx, openH / 2.0f, EYE);
    d.fillEllipse(rx, eyeY, eyeRx, openH / 2.0f, EYE);
    if (openH > 8) {
        int pdx = (int)(gdx * 8), pdy = (int)(gdy * 7);
        if (e == Emotion::Doubt) pdy = -2;
        d.fillEllipse(lx + pdx, eyeY + pdy, 7.5f, 8.5f, PUP);
        d.fillEllipse(rx + pdx, eyeY + pdy, 7.5f, 8.5f, PUP);
        d.fillCircle(lx + pdx + 3, eyeY + pdy - 3, 2.2f, EYE);
        d.fillCircle(rx + pdx + 3, eyeY + pdy - 3, 2.2f, EYE);
    }

    int my = eyeY + 42;
    if (e == Emotion::Happy) {
        d.drawArc(cx, my - 12, 20, 16, 200, 340, MOUTH);
    } else if (e == Emotion::Sad) {
        d.drawArc(cx, my + 6, 20, 16, 20, 160, MOUTH);
    } else if (e == Emotion::Surprised) {
        d.fillEllipse(cx, my + 2, 8, 11, PUP);
        d.drawEllipse(cx, my + 2, 8, 11, MOUTH);
    } else if (e == Emotion::Sleepy) {
        d.drawLine(cx - 12, my, cx + 12, my, MOUTH);
    } else if (e == Emotion::Angry) {
        d.drawLine(cx - 14, my - 2, cx + 14, my + 3, MOUTH);
    } else if (_mouthOpen && M5.Speaker.isRunning()) {
        d.fillEllipse(cx, my + 2, 7, 9, PUP);
        d.drawEllipse(cx, my + 2, 7, 9, MOUTH);
    } else {
        d.drawLine(cx - 13, my + 2, cx + 13, my + 2, MOUTH);
    }
}
