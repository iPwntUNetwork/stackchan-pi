#pragma once
#include <string>
#include <atomic>
#include "llm_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Async chat worker: owns the LLM pipelines (text / voice / vision) and the
// xiaozhi voice session. UI + web API submit jobs; worker streams results.
class ChatEngine {
public:
    enum class State : uint8_t {
        Idle, Recording, Transcribing, Thinking, Speaking,
        XzOff, XzConnecting, XzListening, XzServerSpeaking, Error
    };

    void begin();
    void tick();   // main loop: xiaozhi mic pump

    void submitText(const std::string& text, bool withPhoto = false);
    void startVoice();
    void stopVoice();
    void abort() { _abort = true; }

    void xzConnect();
    void xzDisconnect();
    void xzPushToTalk(bool down);

    State state() const { return _state.load(); }
    std::string statusText() const;
    std::string lastUser() { return _lastUser; }
    std::string lastReply() { return _lastReply; }
    std::string partial() { return _partial; }
    Emotion emotion() const { return _emotion.load(); }
    ChatHistory history;
    void lockHist() { xSemaphoreTake(_histMux, portMAX_DELAY); }
    void unlockHist() { xSemaphoreGive(_histMux); }

    bool setProvider(uint8_t p);

private:
    static void taskTramp(void*);
    void workerLoop();
    void processTextJob(const std::string& userText, bool withPhoto);
    void processVoiceJob();

    enum class JobKind : uint8_t { Text, Voice, XzConnect };
    struct Job { JobKind kind; std::string text; bool photo; };
    QueueHandle_t _jobs = nullptr;
    std::atomic<State> _state{State::Idle};
    std::atomic<Emotion> _emotion{Emotion::Neutral};
    SemaphoreHandle_t _histMux = nullptr;
    std::string _lastUser, _lastReply, _partial, _lastError;
    volatile bool _abort = false;
    bool _xzPush = false;
    volatile bool _xzTalkReq = false;
};

extern ChatEngine chat;
