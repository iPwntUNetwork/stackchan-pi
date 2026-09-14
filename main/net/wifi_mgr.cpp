#include "wifi_mgr.h"
#include <esp_timer.h>
#include "config_store.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <cstring>
#include <cctype>

WifiMgr wifi;
static const char* TAG = "wifi";
static EventGroupHandle_t s_ev = nullptr;
static constexpr int CONNECTED_BIT = BIT0;
static bool s_netifReady = false;

static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_DISCONNECTED:
                xEventGroupClearBits(s_ev, CONNECTED_BIT);
                if (wifi.state() == WifiState::Connected)
                    ESP_LOGW(TAG, "lost connection");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, CONNECTED_BIT);
    }
}

void WifiMgr::begin() {
    if (!s_ev) {
        s_ev = xEventGroupCreate();
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&wc);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_netifReady = true;
    }
    if (cfg.wifi_ssid[0]) {
        wifi_config_t wc = {};
        strlcpy((char*)wc.sta.ssid, cfg.wifi_ssid, sizeof(wc.sta.ssid));
        strlcpy((char*)wc.sta.password, cfg.wifi_pass, sizeof(wc.sta.password));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_start();
        esp_wifi_connect();
        _state = WifiState::Connecting;
        _connectStart = (uint32_t)(esp_timer_get_time() / 1000LL);
        ESP_LOGI(TAG, "connecting to %s ...", cfg.wifi_ssid);
    } else {
        startPortal();
    }
}

void WifiMgr::startPortal() {
    _apSsid = std::string(HOSTNAME_BASE) + "-setup";
    wifi_config_t wc = {};
    strlcpy((char*)wc.ap.ssid, _apSsid.c_str(), sizeof(wc.ap.ssid));
    strlcpy((char*)wc.ap.password, "stackchan", sizeof(wc.ap.password));
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (!s_netifReady) esp_wifi_start();
    startCaptiveDns();
    _portal = true;
    _state = WifiState::ApPortal;
    ESP_LOGI(TAG, "AP portal: ssid=%s pass=stackchan", _apSsid.c_str());
}

void WifiMgr::applyCredentials(const std::string& ssid, const std::string& pass) {
    strlcpy(cfg.wifi_ssid, ssid.c_str(), sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, pass.c_str(), sizeof(cfg.wifi_pass));
    cfg.save();
    if (_portal) { stopCaptiveDns(); _portal = false; }
    esp_wifi_disconnect();
    wifi_config_t wc = {};
    strlcpy((char*)wc.sta.ssid, cfg.wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char*)wc.sta.password, cfg.wifi_pass, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_connect();
    _state = WifiState::Connecting;
    _connectStart = (uint32_t)(esp_timer_get_time() / 1000LL);
}

void WifiMgr::tick() {
    if (_portal) return;
    if (_state == WifiState::Connecting) {
        if (connected()) {
            _state = WifiState::Connected;
            // start SNTP with configured TZ
            setenv("TZ", cfg.tz, 1);
            tzset();
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sc);
            ESP_LOGI(TAG, "connected");
        } else if ((uint32_t)(esp_timer_get_time() / 1000LL) - _connectStart > 15000) {
            ESP_LOGW(TAG, "connect failed -> AP portal");
            esp_wifi_disconnect();
            esp_wifi_stop();
            startPortal();
        }
    } else if (_state == WifiState::Connected) {
        if (!connected()) {
            _state = WifiState::Connecting;
            _connectStart = (uint32_t)(esp_timer_get_time() / 1000LL);
            esp_wifi_connect();
        }
    }
}

bool WifiMgr::connected() {
    return s_ev && (xEventGroupGetBits(s_ev) & CONNECTED_BIT);
}

std::string WifiMgr::ip() {
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    char buf[20];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
    return buf;
}

int WifiMgr::rssi() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return -100;
}
