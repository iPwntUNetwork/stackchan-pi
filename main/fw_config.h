#pragma once

#include <cstdint>
uint32_t millis32();   // ms since boot, wraps (defined in hal/motion.cpp)
// ============================================================
//  Stack-chan CoreS3 — hardware map & feature config (ESP-IDF)
//
//  Target: M5Stack CoreS3 + M5Stack Stack-chan base
//   - Camera (GC0308): SCCB on internal I2C G12/G11, D0..D7 =
//     39/40/41/42/15/16/48/47, VSYNC 46, HREF 38, PCLK 45, no XCLK GPIO.
//   - Servo bus: UART1 half-duplex, TX=G6 RX=G7, 1 Mbps 8N1,
//     yaw ID=1 (zero 460), pitch ID=2 (zero 620), 0.3125 deg/step.
//   - PY32 expander @0x6F: pin0 = servo VM power EN,
//     0x24 LED cfg (count|refresh), 0x30 LED RAM RGB565-LE (12 base LEDs).
//   - Head touch: Si12T @0x68, OUTPUT1 reg 0x10, 3 zones, 2 bits each.
//   - Ports: PORT.A = G1/G2, PORT.B = G8/G9 (yellow=G9 white=G8),
//     PORT.C = G17/G18 (yellow=G17 white=G18).
// ============================================================

// ---- Servo bus (official M5 Stack-chan base) ----
#define SCS_UART_NUM       1
#define SCS_TX_PIN         6
#define SCS_RX_PIN         7
#define SCS_BAUD           1000000
#define SCS_ECHO_CANCEL    false   // M5 base: dedicated RX. true = single-wire Takao base
#define SCS_YAW_ID         1
#define SCS_PITCH_ID       2

// ---- PWM servo fallback (custom bases, PORT.A) ----
#define PWM_YAW_PIN        1
#define PWM_PITCH_PIN      2
#define PWM_US_MIN         500
#define PWM_US_MAX         2500

// ---- Camera (CoreS3 GC0308) ----
#define CAM_PIN_SCCB_SDA   12
#define CAM_PIN_SCCB_SCL   11
#define CAM_PIN_D0         39
#define CAM_PIN_D1         40
#define CAM_PIN_D2         41
#define CAM_PIN_D3         42
#define CAM_PIN_D4         15
#define CAM_PIN_D5         16
#define CAM_PIN_D6         48
#define CAM_PIN_D7         47
#define CAM_PIN_VSYNC      46
#define CAM_PIN_HREF       38
#define CAM_PIN_PCLK       45

// ---- PY32 IO expander (Stack-chan base) ----
#define PY32_ADDR          0x6F
#define PY32_I2C_FREQ      100000
#define PY32_PIN_VM_EN     0
#define PY32_REG_VERSION   0x02
#define PY32_REG_GPIO_MODE_L 0x03
#define PY32_REG_GPIO_OUT_L  0x05
#define PY32_REG_LED_CFG    0x24
#define PY32_REG_LED_RAM    0x30
#define PY32_MAX_LEDS      32
#define BASE_LED_COUNT     12

// ---- Head touch (Si12T) ----
#define SI12T_ADDR         0x68
#define SI12T_REG_OUTPUT1  0x10
#define SI12T_I2C_FREQ     100000

// ---- External units ----
#define IR_TX_PIN_DEFAULT  17    // PORT.C yellow
#define IR_RX_PIN_DEFAULT  18    // PORT.C white
#define NFC_SDA_PIN        9     // PORT.B yellow
#define NFC_SCL_PIN        8     // PORT.B white
#define NEKO_PIN_DEFAULT   9
#define NEKO_COUNT_DEFAULT 18

// ---- Audio ----
#define MIC_RATE           16000
#define MIC_MAX_SECONDS    8
#define TTS_MAX_BYTES      (900 * 1024)

// ---- Misc ----
#define HOSTNAME_BASE      "stackchan"
