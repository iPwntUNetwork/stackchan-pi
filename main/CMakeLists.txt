set(_priv_reqs
    M5Unified
    M5GFX
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    esp_psram
    driver
    esp_https_ota
    app_update
    json
    lwip
    esp_http_client
    esp_websocket_client
    esp_audio_codec
    esp32-camera
    mdns
    led_strip
    esp_http_server
    esp_timer
)

idf_component_register(
    SRCS
        "app_main.cpp"
        "config_store.cpp"
        "hal/servo_bus.cpp"
        "hal/py32_expander.cpp"
        "hal/motion.cpp"
        "hal/head_touch.cpp"
        "hal/camera_feed.cpp"
        "hal/audio_io.cpp"
        "hal/leds.cpp"
        "hal/ir_rmt.cpp"
        "hal/nfc_pn532.cpp"
        "hal/opus_codec.cpp"
        "net/wifi_mgr.cpp"
        "net/captive_dns.cpp"
        "net/http_util.cpp"
        "net/pi_link.cpp"
        "net/web_app.cpp"
        "llm/llm_client.cpp"
        "llm/openai_client.cpp"
        "llm/gemini_client.cpp"
        "llm/xiaozhi_client.cpp"
        "llm/chat_engine.cpp"
        "ui/ui.cpp"
        "ui/face.cpp"
        "ui/keyboard.cpp"
        "ui/screens.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "webui.html"
    PRIV_REQUIRES ${_priv_reqs}
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function)
