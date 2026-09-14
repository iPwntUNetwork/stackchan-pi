#pragma once
#include <esp_event.h>
#include <esp_websocket_client.h>
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>

// Xiaozhi voice assistant (WebSocket transport + Opus 16 kHz mono, 60 ms
// frames). Protocol per 78/xiaozhi-esp32 docs.
class XiaozhiClient {
public:
    enum class State : uint8_t { Off, Connecting, Ready, Listening, ServerSpeaking, Error };

    bool connect(const char* url, const char* token);   // any thread
    void disconnect();
    bool connected() const { return _state.load() >= State::Ready; }
    State state() const { return _state.load(); }
    void setState(State s) { _state.store(s); }
    uint16_t serverRate() const { return (uint16_t)_serverRate; }

    void startListening();
    void stopListening();
    void sendWakeWord(const char* text = "Hello Xiaozhi");
    void sendMicFrame(const int16_t* pcm, size_t samples);  // 960 samples @16k

    void handleText(const char* payload, size_t len);
    void handleBinary(uint8_t* payload, size_t len);

    std::function<void(const std::string&)> onTranscript;
    std::function<void(const std::string&)> onAssistantText;
    std::function<void(const std::string&)> onEmotion;
    std::function<void(bool)> onSpeaking;
    std::function<void(const std::string&)> onError;

    void* wsHandle() const { return _ws; }   // for event glue

private:
    void sendJson(const std::string& s);

    void* _ws = nullptr;                    // esp_websocket_client_handle_t
    std::atomic<State> _state{State::Off};
    std::atomic<int> _serverRate{24000};
    bool _listening = false;
    std::string _clientId;
};

extern XiaozhiClient xz;
void xzWsEventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData);
