#pragma once
#include <atomic>
#include <cstdint>
#include "../llm/llm_client.h"

// Stack-chan avatar face (gaze/saccade, blink, emotion mouth, breathing).
class Face {
public:
    void setEmotion(Emotion e);
    Emotion emotion() const { return _emotion.load(); }
    void setGaze(float dx, float dy);
    void render(int cx = 160, int cy = 96);
    void tickSpeech();
private:
    std::atomic<Emotion> _emotion{Emotion::Neutral};
    float _gdx = 0, _gdy = 0;
    uint32_t _nextBlink = 0, _blinkUntil = 0, _nextSaccade = 0;
    float _blink = 1.0f;
    float _pdx = 0, _pdy = 0;
    bool _mouthOpen = false;
    uint32_t _nextFlap = 0;
    uint32_t _lastFrame = 0;
};

extern Face face;
