#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>

enum class Emotion : uint8_t { Neutral, Happy, Angry, Sad, Surprised, Sleepy, Doubt };

struct ChatMsg {
    std::string role;
    std::string text;
};

class ChatHistory {
public:
    std::vector<ChatMsg> msgs;
    void add(const char* role, const std::string& text) {
        msgs.push_back({role, text});
        while (msgs.size() > 12) msgs.erase(msgs.begin());
    }
    void clear() { msgs.clear(); }
};

Emotion parseEmotionTag(std::string& text);
std::string systemPrompt();

struct VisionImage {
    const uint8_t* jpeg = nullptr;
    size_t len = 0;
};

class LlmClient {
public:
    virtual ~LlmClient() {}
    virtual bool chat(ChatHistory& hist, const std::string& userMsg, const VisionImage& img,
                      std::function<void(const std::string&)> onDelta, std::string& reply) = 0;
    virtual bool transcribe(const uint8_t* wav, size_t len, std::string& text) = 0;
    virtual uint8_t* tts(const std::string& text, size_t& outLen) = 0;
    virtual const char* name() const = 0;
};

LlmClient* openAiClient();
LlmClient* geminiClient();
