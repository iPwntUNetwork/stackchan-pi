#include "motion.h"
#include "servo_bus.h"
#include "py32_expander.h"
#include <M5Unified.h>
#include <esp_timer.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdlib>

Motion motion;

uint32_t millis32() { return (uint32_t)(esp_timer_get_time() / 1000LL); }

// SCS0009: 1 step = 0.3125 deg
static uint16_t degToRaw(float deg, int16_t zero) {
    float r = deg * 16.0f / 5.0f + zero;
    if (r < 0) r = 0;
    if (r > 1023) r = 1023;
    return (uint16_t)r;
}

// ---- LEDC PWM servo fallback ----
static const int kPwmChYaw = 0, kPwmChPitch = 1;
static bool pwmInited = false;

static void pwmWrite(int ch, float deg) {
    float us = 1500.0f + deg * (1000.0f / 90.0f);
    if (us < PWM_US_MIN) us = PWM_US_MIN;
    if (us > PWM_US_MAX) us = PWM_US_MAX;
    uint32_t duty = (uint32_t)(us / 20000.0f * 16384.0f);   // 14-bit @ 50 Hz
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
}

static bool pwmInit(int pin, int ch) {
    ledc_channel_t c = (ledc_channel_t)ch;
    ledc_channel_config_t cc = {};
    cc.gpio_num = pin;
    cc.speed_mode = LEDC_LOW_SPEED_MODE;
    cc.channel = c;
    cc.timer_sel = LEDC_TIMER_1;
    cc.duty = 16384 * 1500 / 20000;   // center
    cc.intr_type = LEDC_INTR_DISABLE;
    if (ledc_channel_config(&cc) != ESP_OK) return false;
    return true;
}

float Motion::clampYaw(float v) const {
    if (v < cfg.yaw_min) v = cfg.yaw_min;
    if (v > cfg.yaw_max) v = cfg.yaw_max;
    return v;
}
float Motion::clampPitch(float v) const {
    if (v < cfg.pitch_min) v = cfg.pitch_min;
    if (v > cfg.pitch_max) v = cfg.pitch_max;
    return v;
}

bool Motion::begin() {
    bool wantScs = (cfg.servo_mode == 0 || cfg.servo_mode == 1);
    bool wantPwm = (cfg.servo_mode == 0 || cfg.servo_mode == 2);
    if (wantScs && scsBus.begin(SCS_UART_NUM, SCS_TX_PIN, SCS_RX_PIN, SCS_BAUD, SCS_ECHO_CANCEL)) {
        if (py32.present()) { py32.setServoPower(true); vTaskDelay(pdMS_TO_TICKS(250)); }
        if (scsBus.ping(SCS_YAW_ID) || scsBus.ping(SCS_PITCH_ID)) {
            _backend = ServoBackend::Scs;
            scsBus.enableTorque(SCS_YAW_ID, true);
            scsBus.enableTorque(SCS_PITCH_ID, true);
            _torqueOn = true;
            _tgtYaw = _curYaw = cfg.home_yaw;
            _tgtPitch = _curPitch = cfg.home_pitch;
            scsWrite(_curYaw, _curPitch, cfg.servo_speed);
            ESP_LOGI("motion", "SCS0009 bus servos detected");
            return true;
        }
        scsBus.end();
    }
    if (wantPwm) {
        ledc_timer_config_t tc = {};
        tc.speed_mode = LEDC_LOW_SPEED_MODE;
        tc.timer_num = LEDC_TIMER_1;
        tc.duty_resolution = LEDC_TIMER_14_BIT;
        tc.freq_hz = 50;
        tc.clk_cfg = LEDC_AUTO_CLK;
        if (ledc_timer_config(&tc) == ESP_OK &&
            pwmInit(cfg.pwm_yaw_pin, kPwmChYaw) && pwmInit(cfg.pwm_pitch_pin, kPwmChPitch)) {
            pwmInited = true;
            _backend = ServoBackend::Pwm;
            _tgtYaw = _curYaw = cfg.home_yaw;
            _tgtPitch = _curPitch = cfg.home_pitch;
            pwmWrite(kPwmChYaw, _curYaw);
            pwmWrite(kPwmChPitch, _curPitch);
            ESP_LOGI("motion", "PWM servos on G%d/G%d", cfg.pwm_yaw_pin, cfg.pwm_pitch_pin);
            return true;
        }
    }
    ESP_LOGW("motion", "no servos found (head disabled)");
    return false;
}

bool Motion::setTorque(bool on) {
    if (_backend != ServoBackend::Scs) return false;
    bool a = scsBus.enableTorque(SCS_YAW_ID, on);
    bool b = scsBus.enableTorque(SCS_PITCH_ID, on);
    if (a && b) { _torqueOn = on; return true; }
    return false;
}

bool Motion::setPower(bool on) {
    if (!py32.present()) return false;
    bool r = py32.setServoPower(on);
    if (r && on) vTaskDelay(pdMS_TO_TICKS(250));
    return r;
}

bool Motion::scsWrite(float yaw, float pitch, uint16_t speed) {
    uint32_t now = millis32();
    if (now - _lastWrite < 12) return false;
    bool ok = scsBus.writeGoalPosition(SCS_YAW_ID, degToRaw(yaw, cfg.yaw_zero), 0, speed);
    ok &= scsBus.writeGoalPosition(SCS_PITCH_ID, degToRaw(pitch, cfg.pitch_zero), 0, speed);
    _lastWrite = millis32();
    return ok;
}

bool Motion::setTarget(float yawDeg, float pitchDeg, float degPerSec) {
    _tgtYaw = clampYaw(yawDeg);
    _tgtPitch = clampPitch(pitchDeg);
    _lastUserMove = millis32();
    _nextIdleAt = 0;
    if (!_torqueOn && _backend == ServoBackend::Scs) setTorque(true);
    uint16_t speed = cfg.servo_speed;
    if (degPerSec > 0) {
        float s = degPerSec / 0.15f;
        if (s < 30) s = 30;
        if (s > 3000) s = 3000;
        speed = (uint16_t)s;
    }
    if (_backend == ServoBackend::Scs) {
        return scsWrite(_tgtYaw, _tgtPitch, speed);
    } else if (_backend == ServoBackend::Pwm) {
        float dist = fabsf(_tgtYaw - _curYaw) + fabsf(_tgtPitch - _curPitch);
        float dps = degPerSec > 0 ? degPerSec : (cfg.servo_speed * 0.15f);
        _pwmDur = (uint32_t)(dist / (dps > 5 ? dps : 60) * 1000.0f);
        if (_pwmDur < 120) _pwmDur = 120;
        if (_pwmDur > 3000) _pwmDur = 3000;
        _pwmStart = millis32();
        _pwmFromYaw = _curYaw; _pwmFromPitch = _curPitch;
        _pwmMoving = true;
        return true;
    }
    return false;
}

void Motion::nudge(float yawAmp, float pitchAmp) {
    float y = clampYaw(_tgtYaw + (rand() % 201 - 100) / 100.0f * yawAmp);
    float p = clampPitch(_tgtPitch + (rand() % 201 - 100) / 100.0f * pitchAmp);
    if (_backend == ServoBackend::Scs) scsWrite(y, p, cfg.servo_speed * 2);
    else if (_backend == ServoBackend::Pwm) setTarget(y, p, 200);
}

void Motion::idleKick() { _lastUserMove = millis32(); _nextIdleAt = 0; }

void Motion::applyIdle(uint32_t now) {
    if (!cfg.idle_motion) return;
    if (now - _lastUserMove < 8000) return;
    if (_nextIdleAt == 0) _nextIdleAt = now + 4000 + rand() % 5000;
    if (now >= _nextIdleAt) {
        float yaw = clampYaw(cfg.home_yaw + rand() % 57 - 28);
        float pitch = clampPitch(cfg.home_pitch + rand() % 25 - 12);
        setTarget(yaw, pitch, 0);
        _lastUserMove = now - 6000;
        _nextIdleAt = now + 5000 + rand() % 7000;
    }
}

void Motion::tick() {
    uint32_t now = millis32();
    if (_backend == ServoBackend::Pwm && _pwmMoving) {
        uint32_t el = now - _pwmStart;
        float t = _pwmDur ? (float)el / _pwmDur : 1.0f;
        if (t >= 1.0f) { t = 1.0f; _pwmMoving = false; }
        float e = t * t * (3 - 2 * t);
        _curYaw = _pwmFromYaw + (_tgtYaw - _pwmFromYaw) * e;
        _curPitch = _pwmFromPitch + (_tgtPitch - _pwmFromPitch) * e;
        pwmWrite(kPwmChYaw, _curYaw);
        pwmWrite(kPwmChPitch, _curPitch);
    }
    applyIdle(now);
    static uint32_t torqueCheck = 0;
    if (_backend == ServoBackend::Scs && _torqueOn && now - _lastUserMove > 90000 &&
        now - torqueCheck > 5000) {
        torqueCheck = now;
        setTorque(false);
    }
}
