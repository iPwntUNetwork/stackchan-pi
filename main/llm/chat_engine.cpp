#include "chat_engine.h"
#include "../config_store.h"
#include "../hal/audio_io.h"
#include "../hal/camera_feed.h"
#include "../hal/leds.h"
#include "xiaozhi_client.h"
#include <freertos/task.h>
#include <cstring>

ChatEngine chat;

void ChatEngine::begin() {
    _jobs = xQueueCreate(4, sizeof(Job));
    _histMux = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(taskTramp, "chat", 16384, this, 3, nullptr, 0);
}

std::string ChatEngine::statusText() const {
    switch (_state.load()) {
        case State::Idle: return "idle";
        case State::Recording: return "recording";
        case State::Transcribing: return "transcribing";
        case State::Thinking: return "thinking";
        case State::Speaking: return "speaking";
        case State::XzOff: return "xiaozhi: off";
        case State::XzConnecting: return "xiaozhi: connecting";
        case State::XzListening: return "xiaozhi: listening";
        case State::XzServerSpeaking: return "xiaozhi: speaking";
        case State::Error: return "error: " + _lastError;
    }
    return "";
}

void ChatEngine::submitText(const std::string& text, bool withPhoto) {
    Job j{JobKind::Text, text, withPhoto};
    xQueueSend(_jobs, &j, 0);
}

void ChatEngine::startVoice() {
    if (_state.load() != State::Idle) return;
    if (audio.startRecord()) _state = State::Recording;
}

void ChatEngine::stopVoice() {
    if (_state.load() != State::Recording) return;
    if (audio.stopRecord() && audio.pcmSamples() > MIC_RATE / 2) {
        Job j{JobKind::Voice, "", false};
        xQueueSend(_jobs, &j, portMAX_DELAY);
    } else {
        _state = State::Idle;
    }
}

void ChatEngine::xzConnect() {
    if (cfg.provider != 2) return;
    _state = State::XzConnecting;
    Job j{JobKind::XzConnect, "", false};
    xQueueSend(_jobs, &j, portMAX_DELAY);
}

void ChatEngine::xzDisconnect() {
    xz.disconnect();
    _state = State::Idle;
}

void ChatEngine::xzPushToTalk(bool down) {
    if (cfg.provider != 2) return;
    _xzTalkReq = down;
    if (down) audio.startStreamMic();
    else audio.stopStreamMic();
}

bool ChatEngine::setProvider(uint8_t p) {
    if (p > 2) return false;
    cfg.provider = p;
    cfg.save();
    if (p != 2) xz.disconnect();
    else xzConnect();
    return true;
}

void ChatEngine::taskTramp(void* p) { ((ChatEngine*)p)->workerLoop(); }

// main-loop hook: pump captured mic frames into the xiaozhi ws while PTT held
void ChatEngine::tick() {
    if (cfg.provider != 2) return;
    if (_xzTalkReq != _xzPush) {
        _xzPush = _xzTalkReq;
        if (_xzPush) xz.startListening();
        else xz.stopListening();
    }
    if (_xzPush && xz.state() == XiaozhiClient::State::Listening) {
        static int16_t chunkBuf[960];
        while (audio.streamDrain(chunkBuf, 960) == 960) {
            xz.sendMicFrame(chunkBuf, 960);
        }
    }
}

void ChatEngine::workerLoop() {
    xz.onTranscript = [&](const std::string& t) {
        _lastUser = t;
        lockHist(); history.add("user", t); unlockHist();
    };
    xz.onAssistantText = [&](const std::string& t) {
        _lastReply = t;
        lockHist(); history.add("assistant", t); unlockHist();
    };
    xz.onEmotion = [&](const std::string& e) {
        if (e == "happy" || e == "laughing") _emotion = Emotion::Happy;
        else if (e == "sad" || e == "crying") _emotion = Emotion::Sad;
        else if (e == "angry") _emotion = Emotion::Angry;
        else if (e == "surprised" || e == "astonished") _emotion = Emotion::Surprised;
        else if (e == "sleepy") _emotion = Emotion::Sleepy;
        else if (e == "thinking" || e == "doubtful") _emotion = Emotion::Doubt;
        else _emotion = Emotion::Neutral;
    };
    xz.onSpeaking = [&](bool on) {
        if (on) audio.streamStart(xz.serverRate());
        else audio.streamEnd();
    };

    while (true) {
        if (cfg.provider == 2 && xz.state() != XiaozhiClient::State::Off) {
            if (_xzPush && xz.state() == XiaozhiClient::State::Listening) _state = State::XzListening;
            else if (xz.state() == XiaozhiClient::State::ServerSpeaking) _state = State::XzServerSpeaking;
            else if (xz.state() == XiaozhiClient::State::Ready) _state = State::Idle;
            else if (xz.state() == XiaozhiClient::State::Connecting) _state = State::XzConnecting;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        Job j;
        if (xQueueReceive(_jobs, &j, portMAX_DELAY) == pdTRUE) {
            _abort = false;
            if (j.kind == JobKind::XzConnect) {
                if (!xz.connect(cfg.xz_url, cfg.xz_token)) {
                    _lastError = "xz connect";
                    _state = State::Error;
                }
                continue;
            }
            if (j.kind == JobKind::Voice) processVoiceJob();
            else processTextJob(j.text, j.photo);
        }
    }
}

void ChatEngine::processTextJob(const std::string& userText, bool withPhoto) {
    if (userText.empty()) { _state = State::Idle; return; }
    _state = State::Thinking;
    leds.setMood(Mood::Thinking);
    LlmClient* llm = (cfg.provider == 1) ? geminiClient() : openAiClient();
    _lastUser = userText;
    lockHist(); history.add("user", userText); unlockHist();

    VisionImage img;
    uint8_t* jpeg = nullptr;
    size_t jpegLen = 0;
    if (withPhoto && camera.ready() && camera.getJpeg(&jpeg, &jpegLen)) {
        img.jpeg = jpeg;
        img.len = jpegLen;
    }

    std::string reply;
    _partial = "";
    bool ok = llm->chat(history, userText, img,
                        [&](const std::string& d) { _partial += d; }, reply);
    if (jpeg) free(jpeg);
    if (_abort) { _state = State::Idle; return; }
    if (!ok || reply.empty()) {
        _lastError = llm->name();
        _state = State::Error;
        leds.setMood(Mood::Error);
        return;
    }

    Emotion e = parseEmotionTag(reply);
    _emotion = e;
    _lastReply = reply;
    lockHist(); history.add("assistant", reply); unlockHist();

    size_t wavLen = 0;
    uint8_t* wav = llm->tts(reply, wavLen);
    if (wav) {
        _state = State::Speaking;
        leds.setMood(Mood::Speaking);
        audio.playWav(wav, wavLen);
        while (audio.playing() && !_abort) vTaskDelay(pdMS_TO_TICKS(50));
        free(wav);
    }
    _partial = "";
    _state = State::Idle;
    leds.setMood(Mood::Idle);
}

void ChatEngine::processVoiceJob() {
    _state = State::Transcribing;
    size_t wavLen = 0;
    uint8_t* wav = audio.takeWav(wavLen);
    if (!wav) { _state = State::Idle; return; }

    LlmClient* llm = (cfg.provider == 1) ? geminiClient() : openAiClient();
    std::string text;
    bool ok = llm->transcribe(wav, wavLen, text);
    free(wav);
    if (!ok || text.empty()) {
        _lastError = "stt";
        _state = State::Error;
        leds.setMood(Mood::Error);
        return;
    }
    _lastUser = text;
    if (_abort) { _state = State::Idle; return; }
    processTextJob(text, false);   // history entry added inside
}
