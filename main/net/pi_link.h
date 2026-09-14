#pragma once
#include <string>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include "fw_config.h"
#include <freertos/semphr.h>
#include <esp_event.h>
#include <esp_websocket_client.h>

// Raspberry Pi Zero W 2 link:
//  - ttyd WebSocket terminal (raw shell rendered on the device screen)
//  - stackchan-agent REST proxy (exec / status / camera / files)
class PiLink {
public:
    void begin();
    void connectTerminal();
    void disconnectTerminal();
    bool terminalConnected() const { return _connected; }

    void sendKeys(const std::string& s);
    enum Special : uint8_t { KeyEnter, KeyTab, KeyCtrlC, KeyEsc, KeyUp, KeyDown, KeyLeft, KeyRight };
    void sendSpecial(uint8_t key);

    static constexpr uint8_t COLS = 52, ROWS = 16, MAXLINES = 48;
    void copyScreen(char out[ROWS][COLS + 1]);
    bool newOutput() { bool v = _dirty; _dirty = false; return v; }

    std::string agentUrl(const char* path);
    std::string agentStatus();
    std::string agentExec(const std::string& cmd);
    bool agentReachable(uint32_t maxAgeMs = 15000) const {
        return _lastAgentOk && millis32() - _lastAgentOk < maxAgeMs;
    }

private:
    static void wsEvent(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData);
    static void taskTramp(void*);
    void taskLoop();
    void feedOutput(uint8_t* data, size_t len);
    void commitLine();   // caller holds _mux

    void* _ws = nullptr;               // esp_websocket_client_handle_t
    SemaphoreHandle_t _mux = nullptr;
    volatile bool _connected = false;
    char _lines[MAXLINES][COLS + 1];
    uint8_t _head = 0;
    uint8_t _col = 0;
    bool _ansi = false;
    uint8_t _ansiState = 0;
    volatile bool _dirty = false;
    volatile uint32_t _lastAgentOk = 0;
    char _b64cred[96] = {0};
    bool _inited = false;
};

extern PiLink pi;
