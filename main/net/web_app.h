#pragma once
#include <cstdint>

// HTTP control plane on esp_http_server: control-panel UI, REST API, MJPEG
// stream, OTA firmware upload, mDNS + captive portal redirect.
class WebApp {
public:
    void begin();   // spawns server + worker task
private:
    bool _started = false;
};

extern WebApp webApp;
