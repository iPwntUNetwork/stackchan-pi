#pragma once
#include <string>
#include <cstdint>

enum class WifiState : uint8_t { Offline, Connecting, Connected, ApPortal };

// WiFi manager: STA with stored creds, AP + captive-DNS fallback, SNTP.
class WifiMgr {
public:
    void begin();      // call once (after nvs init)
    void tick();
    bool connected();
    std::string ip();
    int rssi();
    WifiState state() const { return _state; }
    void applyCredentials(const std::string& ssid, const std::string& pass);
    std::string apSsid() const { return _apSsid; }
private:
    void startPortal();
    WifiState _state = WifiState::Offline;
    std::string _apSsid;
    uint32_t _connectStart = 0;
    bool _portal = false;
};

extern WifiMgr wifi;
void startCaptiveDns();     // UDP 53 -> AP IP (captive_dns.cpp)
void stopCaptiveDns();
