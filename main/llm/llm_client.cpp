#include "llm_client.h"
#include "config_store.h"

Emotion parseEmotionTag(std::string& text) {
    int idx = -1;
    for (int i = (int)text.size() - 2; i >= 0; i--) {
        if (text[i] == '[') { idx = i; break; }
        if (text[i] == ']') break;
    }
    if (idx >= 0 && !text.empty() && text.back() == ']') {
        std::string tag = text.substr(idx + 1, text.size() - idx - 2);
        for (auto& c : tag) c = (char)tolower((unsigned char)c);
        text = text.substr(0, idx);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
        if (tag == "happy" || tag == "joy" || tag == "laugh") return Emotion::Happy;
        if (tag == "angry") return Emotion::Angry;
        if (tag == "sad" || tag == "cry") return Emotion::Sad;
        if (tag == "surprised" || tag == "shocked") return Emotion::Surprised;
        if (tag == "sleepy" || tag == "sleep") return Emotion::Sleepy;
        if (tag == "doubt" || tag == "thinking") return Emotion::Doubt;
        if (tag.size() < 12) return Emotion::Neutral;
    }
    return Emotion::Neutral;
}

std::string systemPrompt() {
    std::string p(cfg.persona);
    p += "\nEnd EVERY reply with an emotion tag in square brackets, one of: [happy] [angry] [sad] [surprised] [sleepy] [doubt] [neutral]. Example: 'Ooh, I see a cat! [happy]'";
    return p;
}
