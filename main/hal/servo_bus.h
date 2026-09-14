#pragma once
#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>

// Feetech SCS0009 serial-bus servo driver (Stack-chan base).
// Framing verified against reference firmware:
//   FF FF | ID | LEN | INSTR | PARAMS... | CHK
//   LEN = #params + 2; CHK = ~(id+len+instr+params)
//   Torque reg 0x28, Goal pos 0x2A (big-endian, +time+speed), Read pos 0x38.

class ScsBus {
public:
    bool begin(uint8_t uart, int tx, int rx, uint32_t baud, bool echoCancel);
    void end();

    int transact(uint8_t id, uint8_t instr, const uint8_t* params, size_t n,
                 uint8_t* respParams, size_t respCap, uint32_t timeoutMs = 40);

    bool ping(uint8_t id);
    bool enableTorque(uint8_t id, bool on);
    bool writeGoalPosition(uint8_t id, uint16_t raw, uint16_t timeMs, uint16_t speed);

    bool ok() const { return _ok; }
private:
    bool sendOnly(uint8_t id, uint8_t instr, const uint8_t* params, size_t n);
    int _uart = -1;
    bool _echo = false;
    bool _ok = false;
};

extern ScsBus scsBus;
