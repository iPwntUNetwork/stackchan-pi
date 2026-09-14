#pragma once
#include <cstdint>
#include "config_store.h"

// Head motion: SCS0009 serial servos (official base) or PWM servos (LEDC,
// custom bases). Angle limits, speed limits, idle glances, torque auto-save.
enum class ServoBackend : uint8_t { None, Scs, Pwm };

class Motion {
public:
    bool begin();
    void tick();  // main loop

    bool setTarget(float yawDeg, float pitchDeg, float degPerSec = 0);
    bool center(float degPerSec = 0) { return setTarget(cfg.home_yaw, cfg.home_pitch, degPerSec); }
    void idleKick();
    bool enabled() const { return _backend != ServoBackend::None; }
    ServoBackend backend() const { return _backend; }
    void getYawPitch(float& yaw, float& pitch) { yaw = _curYaw; pitch = _curPitch; }
    bool setTorque(bool on);
    bool setPower(bool on);
    void nudge(float yawAmp, float pitchAmp);

private:
    float clampYaw(float v) const;
    float clampPitch(float v) const;
    bool scsWrite(float yaw, float pitch, uint16_t speed);
    void applyIdle(uint32_t now);

    ServoBackend _backend = ServoBackend::None;
    float _curYaw = 0, _curPitch = 0;
    float _tgtYaw = 0, _tgtPitch = 0;
    bool  _pwmMoving = false;
    uint32_t _pwmStart = 0, _pwmDur = 0;
    float _pwmFromYaw = 0, _pwmFromPitch = 0;
    uint32_t _lastUserMove = 0, _nextIdleAt = 0;
    uint32_t _lastWrite = 0;
    bool _torqueOn = false;
};

extern Motion motion;

uint32_t millis32();   // shared helper
