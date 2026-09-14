#include "llm_client.h"
#include "../config_store.h"
#include "../net/http_util.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <cstring>
#include <cstdlib>
#include <esp_log.h>

// ---------------- OpenAI (chat + whisper STT + TTS) ----------------

static const char* TAG = "openai";

namespace {

std::string authHeader() { return std::string("Bearer ") + cfg.openai_key; }

class OpenAiClient : public LlmClient {
public:
    const char* name() const override { return "openai"; }

    bool chat(ChatHistory& hist, const std::string& userMsg, const VisionImage& img,
              std::function<void(const std::string&)> onDelta, std::string& reply) override {
        cJSON* doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "model", cfg.openai_model);
        cJSON_AddBoolToObject(doc, "stream", true);
        cJSON_AddNumberToObject(doc, "max_tokens", 300);
        cJSON* msgs = cJSON_AddArrayToObject(doc, "messages");

        auto addTextMsg = [&](const char* role, const char* content) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "role", role);
            cJSON_AddStringToObject(o, "content", content);
            cJSON_AddItemToArray(msgs, o);
        };
        addTextMsg("system", systemPrompt().c_str());
        for (auto& m : hist.msgs) addTextMsg(m.role.c_str(), m.text.c_str());

        cJSON* um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        if (img.jpeg) {
            cJSON* parts = cJSON_AddArrayToObject(um, "content");
            cJSON* it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "type", "text");
            cJSON_AddStringToObject(it, "text", userMsg.c_str());
            cJSON_AddItemToArray(parts, it);
            size_t b64len = 0;
            mbedtls_base64_encode(nullptr, 0, &b64len, img.jpeg, img.len);
            char* b64 = (char*)malloc(b64len);
            if (b64) {
                size_t olen = 0;
                mbedtls_base64_encode((uint8_t*)b64, b64len, &olen, img.jpeg, img.len);
                cJSON* im = cJSON_CreateObject();
                cJSON_AddStringToObject(im, "type", "image_url");
                cJSON* url = cJSON_CreateObject();
                std::string dataUri = std::string("data:image/jpeg;base64,") + b64;
                cJSON_AddStringToObject(url, "url", dataUri.c_str());
                cJSON_AddItemToObject(im, "image_url", url);
                cJSON_AddItemToArray(parts, im);
                free(b64);
            }
        } else {
            cJSON_AddStringToObject(um, "content", userMsg.c_str());
        }
        cJSON_AddItemToArray(msgs, um);

        char* bodyStr = cJSON_PrintUnformatted(doc);
        std::string body = bodyStr ? bodyStr : "{}";
        cJSON_free(bodyStr);
        cJSON_Delete(doc);

        http::Headers headers;
        headers.push_back({"Authorization", authHeader()});
        std::string url = std::string(cfg.openai_base) + "/chat/completions";
        int code = http::postStream(url.c_str(), headers, body, [&](const std::string& line) {
            if (line.rfind("data: ", 0) != 0) return true;
            std::string payload = line.substr(6);
            // trim
            while (!payload.empty() && (payload.back() == ' ' || payload.back() == '\r'))
                payload.pop_back();
            if (payload == "[DONE]") return false;
            cJSON* jd = cJSON_Parse(payload.c_str());
            if (!jd) return true;
            cJSON* ch = cJSON_GetArrayItem(cJSON_GetObjectItem(jd, "choices"), 0);
            if (ch) {
                cJSON* delta = cJSON_GetObjectItem(cJSON_GetObjectItem(ch, "delta"), "content");
                if (delta && cJSON_IsString(delta) && delta->valuestring[0]) {
                    reply += delta->valuestring;
                    onDelta(delta->valuestring);
                }
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
        std::string fields = "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
                             std::string(cfg.openai_stt) + "\r\n";
        fields += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";
        std::string response;
        int code = http::postMultipart(
            (std::string(cfg.openai_base) + "/audio/transcriptions").c_str(),
            {{"Authorization", authHeader()}}, "stackchan-fw", fields,
            "file", "audio.wav", "audio/wav", wav, len, response);
        if (code != 200) {
            ESP_LOGW(TAG, "stt http %d", code);
            return false;
        }
        cJSON* jd = cJSON_Parse(response.c_str());
        if (!jd) return false;
        cJSON* t = cJSON_GetObjectItem(jd, "text");
        text = t && cJSON_IsString(t) ? t->valuestring : "";
        cJSON_Delete(jd);
        return !text.empty();
    }

    uint8_t* tts(const std::string& input, size_t& outLen) override {
        cJSON* doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "model", cfg.openai_tts);
        cJSON_AddStringToObject(doc, "voice", cfg.openai_voice);
        cJSON_AddStringToObject(doc, "input", input.c_str());
        cJSON_AddStringToObject(doc, "response_format", "wav");
        char* s = cJSON_PrintUnformatted(doc);
        std::string body = s ? s : "{}";
        cJSON_free(s);
        cJSON_Delete(doc);
        http::Headers headers;
        headers.push_back({"Authorization", authHeader()});
        uint8_t* out = nullptr;
        int code = http::postBinary((std::string(cfg.openai_base) + "/audio/speech").c_str(),
                                    headers, body, &out, &outLen);
        if (code != 200 || !out) {
            ESP_LOGW(TAG, "tts http %d", code);
            if (out) free(out);
            return nullptr;
        }
        return out;
    }
};

}  // namespace

LlmClient* openAiClient() {
    static OpenAiClient c;
    return &c;
}
