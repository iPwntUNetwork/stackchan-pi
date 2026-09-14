#include "llm_client.h"
#include "../config_store.h"
#include "../net/http_util.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>

// ---------------- Google Gemini (streamGenerateContent + STT + TTS) ----------------

static const char* TAG = "gemini";

namespace {

std::string baseUrl() { return "https://generativelanguage.googleapis.com/v1beta"; }
std::string keyParam() { return std::string("?key=") + cfg.gemini_key; }

class GeminiClient : public LlmClient {
public:
    const char* name() const override { return "gemini"; }

    bool chat(ChatHistory& hist, const std::string& userMsg, const VisionImage& img,
              std::function<void(const std::string&)> onDelta, std::string& reply) override {
        cJSON* doc = cJSON_CreateObject();
        cJSON* sys = cJSON_AddObjectToObject(doc, "systemInstruction");
        cJSON* sysParts = cJSON_AddArrayToObject(sys, "parts");
        cJSON* sysP0 = cJSON_CreateObject();
        cJSON_AddStringToObject(sysP0, "text", systemPrompt().c_str());
        cJSON_AddItemToArray(sysParts, sysP0);

        cJSON* contents = cJSON_AddArrayToObject(doc, "contents");
        auto addText = [&](const char* role, const std::string& t) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "role", role);
            cJSON* parts = cJSON_AddArrayToObject(o, "parts");
            cJSON* p0 = cJSON_CreateObject();
            cJSON_AddStringToObject(p0, "text", t.c_str());
            cJSON_AddItemToArray(parts, p0);
            cJSON_AddItemToArray(contents, o);
        };
        for (auto& m : hist.msgs) addText(m.role == "assistant" ? "model" : "user", m.text);

        cJSON* u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON* parts = cJSON_AddArrayToObject(u, "parts");
        cJSON* p0 = cJSON_CreateObject();
        cJSON_AddStringToObject(p0, "text", userMsg.c_str());
        cJSON_AddItemToArray(parts, p0);
        if (img.jpeg) {
            size_t b64len = 0;
            mbedtls_base64_encode(nullptr, 0, &b64len, img.jpeg, img.len);
            char* b64 = (char*)malloc(b64len);
            if (b64) {
                size_t olen = 0;
                mbedtls_base64_encode((uint8_t*)b64, b64len, &olen, img.jpeg, img.len);
                cJSON* pd = cJSON_CreateObject();
                cJSON* inl = cJSON_AddObjectToObject(pd, "inline_data");
                cJSON_AddStringToObject(inl, "mime_type", "image/jpeg");
                cJSON_AddStringToObject(inl, "data", b64);
                cJSON_AddItemToArray(parts, pd);
                free(b64);
            }
        }
        cJSON_AddItemToArray(contents, u);

        char* bodyStr = cJSON_PrintUnformatted(doc);
        std::string body = bodyStr ? bodyStr : "{}";
        cJSON_free(bodyStr);
        cJSON_Delete(doc);

        std::string url = baseUrl() + "/models/" + cfg.gemini_model +
                          ":streamGenerateContent?alt=sse" + keyParam();
        http::Headers headers;
        headers.push_back({"Content-Type", "application/json"});
        int code = http::postStream(url.c_str(), headers, body, [&](const std::string& line) {
            if (line.rfind("data: ", 0) != 0) return true;
            cJSON* jd = cJSON_Parse(line.c_str() + 6);
            if (!jd) return true;
            cJSON* cands = cJSON_GetObjectItem(jd, "candidates");
            cJSON* c = cands ? cands->child : nullptr;
            while (c) {
                cJSON* cc = cJSON_GetObjectItem(cJSON_GetObjectItem(c, "content"), "parts");
                cJSON* p = cc ? cc->child : nullptr;
                while (p) {
                    cJSON* t = cJSON_GetObjectItem(p, "text");
                    if (t && cJSON_IsString(t) && t->valuestring[0]) {
                        reply += t->valuestring;
                        onDelta(t->valuestring);
                    }
                    p = p->next;
                }
                c = c->next;
            }
            cJSON_Delete(jd);
            return true;
        });
        if (code != 200) {
            ESP_LOGW(TAG, "chat http %d", code);
            return false;
        }
        return !reply.empty();
    }

    bool transcribe(const uint8_t* wav, size_t len, std::string& text) override {
        size_t b64len = 0;
        mbedtls_base64_encode(nullptr, 0, &b64len, wav, len);
        char* b64 = (char*)heap_caps_malloc(b64len, MALLOC_CAP_SPIRAM);
        if (!b64) return false;
        size_t olen = 0;
        mbedtls_base64_encode((uint8_t*)b64, b64len, &olen, wav, len);
        cJSON* doc = cJSON_CreateObject();
        cJSON* contents = cJSON_AddArrayToObject(doc, "contents");
        cJSON* c0 = cJSON_CreateObject();
        cJSON* parts = cJSON_AddArrayToObject(c0, "parts");
        cJSON* pt = cJSON_CreateObject();
        cJSON_AddStringToObject(pt, "text", "Transcribe this speech. Reply with only the transcription.");
        cJSON_AddItemToArray(parts, pt);
        cJSON* pa = cJSON_CreateObject();
        cJSON* inl = cJSON_AddObjectToObject(pa, "inline_data");
        cJSON_AddStringToObject(inl, "mime_type", "audio/wav");
        cJSON_AddStringToObject(inl, "data", b64);
        cJSON_AddItemToArray(parts, pa);
        cJSON_AddItemToArray(contents, c0);
        char* bodyStr = cJSON_PrintUnformatted(doc);
        std::string body = bodyStr ? bodyStr : "{}";
        cJSON_free(bodyStr);
        cJSON_Delete(doc);
        free(b64);

        std::string response;
        std::string url = baseUrl() + "/models/" + cfg.gemini_model + ":generateContent" + keyParam();
        http::Headers headers;
        headers.push_back({"Content-Type", "application/json"});
        int code = http::postJson(url.c_str(), headers, body, response, 45000, 400000);
        if (code != 200) {
            ESP_LOGW(TAG, "stt http %d", code);
            return false;
        }
        cJSON* jd = cJSON_Parse(response.c_str());
        if (!jd) return false;
        cJSON* cands = cJSON_GetObjectItem(jd, "candidates");
        text = "";
        cJSON* c = cands ? cJSON_GetArrayItem(cands, 0) : nullptr;
        if (c) {
            cJSON* cc = cJSON_GetObjectItem(cJSON_GetObjectItem(c, "content"), "parts");
            cJSON* p = cc ? cJSON_GetArrayItem(cc, 0) : nullptr;
            cJSON* t = p ? cJSON_GetObjectItem(p, "text") : nullptr;
            if (t && cJSON_IsString(t)) text = t->valuestring;
        }
        cJSON_Delete(jd);
        return !text.empty();
    }

    uint8_t* tts(const std::string& input, size_t& outLen) override {
        outLen = 0;
        if (!cfg.gemini_tts || !cfg.gemini_tts_model[0]) return nullptr;
        cJSON* doc = cJSON_CreateObject();
        cJSON* contents = cJSON_AddArrayToObject(doc, "contents");
        cJSON* c0 = cJSON_CreateObject();
        cJSON* parts = cJSON_AddArrayToObject(c0, "parts");
        cJSON* p0 = cJSON_CreateObject();
        cJSON_AddStringToObject(p0, "text", input.c_str());
        cJSON_AddItemToArray(parts, p0);
        cJSON_AddItemToArray(contents, c0);
        cJSON* gc = cJSON_AddObjectToObject(doc, "generationConfig");
        cJSON* modal = cJSON_AddArrayToObject(gc, "responseModalities");
        cJSON_AddItemToArray(modal, cJSON_CreateString("AUDIO"));
        cJSON* sp = cJSON_AddObjectToObject(gc, "speechConfig");
        cJSON* vc = cJSON_AddObjectToObject(sp, "voiceConfig");
        cJSON* pv = cJSON_AddObjectToObject(vc, "prebuiltVoiceConfig");
        cJSON_AddStringToObject(pv, "voiceName", cfg.gemini_voice);
        char* bodyStr = cJSON_PrintUnformatted(doc);
        std::string body = bodyStr ? bodyStr : "{}";
        cJSON_free(bodyStr);
        cJSON_Delete(doc);

        std::string url = baseUrl() + "/models/" + cfg.gemini_tts_model +
                          ":generateContent" + keyParam();
        http::Headers headers;
        headers.push_back({"Content-Type", "application/json"});
        std::string response;
        int code = http::postJson(url.c_str(), headers, body, response, 60000, 3000000);
        if (code != 200) {
            ESP_LOGW(TAG, "tts http %d", code);
            return nullptr;
        }
        // raw-scan for inline audio base64 (response can exceed 1 MB)
        size_t pos = response.find("\"data\": \"");
        if (pos == std::string::npos) pos = response.find("\"data\":\"");
        if (pos == std::string::npos) return nullptr;
        size_t start = response.find('"', pos + 7) + 1;
        size_t end = response.find('"', start);
        if (end == std::string::npos || end - start > 3000000) return nullptr;
        std::string b64 = response.substr(start, end - start);

        size_t need = 0;
        mbedtls_base64_decode(nullptr, 0, &need, (const uint8_t*)b64.data(), b64.size());
        uint8_t* wav = (uint8_t*)heap_caps_malloc(need + 44, MALLOC_CAP_SPIRAM);
        if (!wav) return nullptr;
        size_t olen = 0;
        mbedtls_base64_decode(wav + 44, need, &olen, (const uint8_t*)b64.data(), b64.size());
        // wrap raw L16 24 kHz mono in a WAV container
        uint32_t dataSize = olen, fileSize = 36 + olen;
        memcpy(wav, "RIFF", 4); memcpy(wav + 4, &fileSize, 4); memcpy(wav + 8, "WAVE", 4);
        memcpy(wav + 12, "fmt ", 4);
        uint32_t fmtLen = 16; memcpy(wav + 16, &fmtLen, 4);
        uint16_t af = 1, ch = 1; memcpy(wav + 20, &af, 2); memcpy(wav + 22, &ch, 2);
        uint32_t sr = 24000; memcpy(wav + 24, &sr, 4);
        uint32_t br = 48000; memcpy(wav + 28, &br, 4);
        uint16_t ba = 2, bits = 16; memcpy(wav + 32, &ba, 2); memcpy(wav + 34, &bits, 2);
        memcpy(wav + 36, "data", 4); memcpy(wav + 40, &dataSize, 4);
        outLen = 44 + olen;
        return wav;
    }
};

}  // namespace

LlmClient* geminiClient() {
    static GeminiClient c;
    return &c;
}
