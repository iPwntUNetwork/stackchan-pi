#pragma once
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// CoreS3 GC0308 camera (DVP, RGB565 QVGA). Continuous capture in a dedicated
// task; latest frame goes into a PSRAM slot for: PIP / MJPEG / vision.
class CameraFeed {
public:
    bool begin();       // call after M5.begin(); releases In_I2C (lgfx auto-reinits)
    void start();
    void stop();

    bool getRgb565(uint16_t* dst, uint16_t& w, uint16_t& h, uint32_t& tsMs);
    bool samplePip(uint16_t* dst, int pw, int ph);
    bool pushToDisplay(int x = 0, int y = 0, int w = 320, int h = 240);
    bool getJpeg(uint8_t** out, size_t* outLen, int quality = 14);

    bool ready() const { return _ready; }
    float fps() const { return _fps; }

    static constexpr uint16_t W = 320;
    static constexpr uint16_t H = 240;

private:
    static void taskTramp(void*);
    void taskLoop();

    uint16_t* _slot = nullptr;
    SemaphoreHandle_t _mux = nullptr;
    volatile bool _ready = false;
    volatile bool _run = false;
    volatile uint32_t _ts = 0;
    float _fps = 0;
};

extern CameraFeed camera;
