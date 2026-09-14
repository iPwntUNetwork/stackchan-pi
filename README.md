# Stack-chan CoreS3 Firmware (ESP-IDF)

Native **ESP-IDF v5.5** firmware for **M5Stack CoreS3** + M5 Stack-chan base.

A desktop-pet robot with three switchable LLM backends (**OpenAI**, **Google
Gemini**, **Xiaozhi**), continuous camera vision, on-device voice (push-to-talk,
Opus), a rendered face UI, full peripheral support (servos, base LEDs, cat-ear
WS2812s, NFC, IR, head touch), and a live shell into its own **Raspberry Pi Zero
W 2** companion via ttyd + a REST agent.

---

## Features

| Area | Details |
|---|---|
| LLM backends | OpenAI (chat SSE + vision b64 + Whisper STT + TTS), Gemini (`:streamGenerateContent?alt=sse`, inline-audio STT, native audio TTS @24 kHz), Xiaozhi (WebSocket voice protocol, Opus 16 kHz/60 ms frames) |
| Voice | ES7210 mic recording, AW88298 playback via M5Unified, push-to-talk, Opus encode/decode (`espressif/esp_audio_codec`) |
| Camera | GC0308 continuous capture loop (QVGA RGB565, JPEG on demand), MJPEG `/stream` endpoint, PiP on face screen, SNAP+ASK photo question |
| Motion | Feetech SCS0009 yaw/pitch on UART1 half-duplex bus (G6/G7 @ 1 M), PWM fallback (G1/G2), smooth tweened motion task |
| Face / UI | ILI9342C 320×240: animated face, chat screen, Pi terminal viewer, camera screen, settings + on-screen keyboard, captive AP portal |
| Pi link | ttyd WebSocket terminal protocol (legacy opcodes) rendered on-device, REST agent (`stackchan_agent.py`) for shell exec/status |
| Peripherals | PY32 I2C expander (servo VM power + 12 base RGB LEDs), Si12T head-touch zones, PN532 NFC on Port A, IR TX/RX via RMT (NEC + raw learn/replay), WS2812 cat-ear chain (Port B, optional) |
| Networking | WiFi STA + AP provisioning portal, mDNS (`stackchan.local`, `raspberrypi.local` resolution), web app on port 80 |

## Directory layout

```
stackchan-idf/
├── CMakeLists.txt            # project file
├── partitions.csv            # 2 × 3 MB OTA + 1.2 MB spiffs (16 MB flash)
├── sdkconfig.defaults        # CoreS3 defaults (quad PSRAM @80M, camera bounce DMA…)
├── components/
│   ├── M5Unified/            # vendored (sibling layout — must stay sibling of M5GFX)
│   └── M5GFX/
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     # managed deps (esp32-camera, esp_websocket_client,
│   │                         #   esp_audio_codec, led_strip, mdns)
│   ├── app_main.cpp          # init order + main task (core 1, 10 ms loop)
│   ├── fw_config.h           # hardware pin map & feature toggles
│   ├── config_store.{h,cpp}  # NVS-backed settings
│   ├── webui.html            # embedded phone UI (served at /)
│   ├── hal/    servo_bus, py32_expander, head_touch, motion, camera_feed,
│   │           audio_io, leds, ir_rmt, nfc_pn532, opus_codec
│   ├── net/    wifi_mgr, captive_dns, http_util, pi_link, web_app
│   ├── llm/    llm_client, openai_client, gemini_client, xiaozhi_client,
│   │           chat_engine (task on core 0)
│   └── ui/     ui, face, keyboard, screens
└── pi/                       # Raspberry Pi companion (see below)
```

## Building

Requires **ESP-IDF v5.5.x** (developed against v5.5.4) with the `esp32s3`
target support. On first use:

```bash
. ${IDF_PATH}/export.sh          # or your usual IDF environment
idf.py set-target esp32s3
idf.py menuconfig                # optional: wifi defaults live in the app, not kconfig
idf.py build
```

Flash & monitor:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

The app is ~1.5 MB; both OTA slots are 3 MB. Managed components are fetched
automatically from the ESP component registry on first configure.

### Continuous integration

`.github/workflows/build.yml` builds the firmware on every push / PR (and on
demand) in the official `espressif/idf:v5.5.4` container, posts a size report
to the run summary, and uploads a flashable bundle (bootloader + partition
table + OTA data + app + `flash_args`) as a workflow artifact. Managed
components are cached between runs keyed on `idf_component.yml` +
`dependencies.lock`. Add a badge once you know the repo slug:

```markdown
[![build](https://github.com/<user>/<repo>/actions/workflows/build.yml/badge.svg)](https://github.com/<user>/<repo>/actions/workflows/build.yml)
```


### sdkconfig notes

Defaults (already in `sdkconfig.defaults`) matter for stability on CoreS3:

- `CONFIG_SPIRAM_MODE_QUAD` + `CONFIG_SPIRAM_SPEED_80M` — octal is not supported on this module
- `CONFIG_CAMERA_PSRAM_DMA=n` + `CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=8192` — bounce mode;
  direct EDMA into quad PSRAM tears RGB565 frames under bus contention
- `CONFIG_FREERTOS_HZ=1000`
- WiFi/lwIP/mbedTLS heap offload to PSRAM enabled for the heavy LLM/audio load

If the M5 speaker output ever crackles after stream restarts, apply
`patches/m5unified.patch` from the reference project (resampler state reset) to
the vendored `components/M5Unified`.

## First boot

1. The device boots into an **AP portal** — WiFi network **`stackchan-setup`**,
   password **`stackchan`**. Captive DNS points everything at it, so open
   `http://192.168.4.1/` (or any URL) and enter
   your WiFi credentials. It reboots onto your network.
2. On the **Settings screen** (nav: Face / Chat / Pi / Cam / Set) pick the LLM
   backend and enter keys/URLs (on-screen keyboard, or the phone UI at
   `http://<device-ip>/` which is usually easier).
   - **OpenAI**: API key (+ optional base URL); STT = Whisper, TTS = OpenAI voice.
   - **Gemini**: API key; streaming chat, audio-in STT, audio-out TTS.
   - **Xiaozhi**: WebSocket URL, access token; realtime voice with Opus frames,
     device id = MAC, client id auto-generated & persisted.

## Raspberry Pi companion (`pi/`)

The robot can operate its own Pi Zero W 2 (Raspberry Pi OS):

- **`stackchan_agent.py`** — small Flask-free stdlib HTTP agent on port **8765**
  exposing `GET /status`, `POST /exec` (authenticated with the shared token via
  `X-Agent-Token`). The firmware uses it to run shell commands on the Pi
  ("robot operates its own Pi").
- **`ttyd.service`** — ttyd on port **7681** (`ttyd -W bash`); the firmware
  speaks the ttyd WebSocket protocol directly and renders the shell on the
  device screen (Pi screen also shows agent status).
- **`setup.sh`** — installs both + systemd units.

On the Pi:

```bash
scp -r pi/ pi@raspberrypi.local:~
ssh pi@raspberrypi.local 'sudo bash ~/pi/setup.sh'
```

Then in firmware config set: Pi host (`raspberrypi.local` or IP), agent token
(default `stackchan` — change it!), and the `pi_token` as `user:pass` for ttyd
basic auth.

Endpoints used by the firmware (all require `X-Agent-Token`):
`GET /status`, `POST /exec`.

## Web UI / API (port 80)

- `/` — phone-oriented control page (chat, PTT, camera, motion pad, settings)
- `/stream` — MJPEG live camera
- `/api/...` — JSON endpoints driving every subsystem (motion, LEDs, IR learn,
  NFC read, terminal connect, backend switching, …)
- mDNS: `stackchan.local`

## Hardware map (CoreS3 + Stack-chan base)

| Peripheral | Pins / notes |
|---|---|
| Servo bus (SCS0009) | UART1 TX G6 / RX G7, 1 Mbps 8N1; yaw ID 1 (zero 460), pitch ID 2 (zero 620); 0.3125°/step |
| Servo power | PY32 expander @0x6F pin 0 (VM enable) |
| Base LEDs | PY32 LED RAM @0x30, 12 × RGB565 |
| Head touch | Si12T @0x68, 3 zones |
| Camera GC0308 | SCCB G12/G11, D0–7 39/40/41/42/15/16/48/47, VSYNC 46, HREF 38, PCLK 45, no XCLK GPIO; call `M5.In_I2C.release()` before `esp_camera_init` |
| Port A (I2C) | G2/G1 — PN532 NFC |
| Port B | G9/G8 — WS2812 cat ears (GPIO configurable) |
| Port C | G17/G18 — IR LED / IR receiver via RMT |
| PWM servo fallback | G1/G2 |

## Credits

- [stack-chan project](https://github.com/stack-chan/stack-chan) — mechanics & servo conventions
- [ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf) — ESP-IDF reference (component layout, sdkconfig gotchas)
- M5Unified / M5GFX, ESP-IDF, esp32-camera, esp_audio_codec — Espressif & M5Stack
