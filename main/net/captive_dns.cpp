#include <lwip/sockets.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

// Minimal captive-portal DNS: answer every A query with the AP IP.
static const char* TAG = "dns";
static int s_sock = -1;

static void dnsTask(void*) {
    uint8_t buf[512];
    while (s_sock >= 0) {
        struct sockaddr_in from{};
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;             // not a query
        // one question assumed; craft response
        uint8_t resp[512];
        int r = 0;
        memcpy(resp, buf, n);
        r = n;
        resp[2] |= 0x80;                          // response
        resp[3] |= 0x81;                          // recursion + rcode ok... use 0x80
        resp[3] = 0x80;
        resp[6] = 0; resp[7] = 1;                 // answers = 1
        // append answer: name ptr, type A, class IN, ttl, rdlen 4, ip
        resp[r++] = 0xC0; resp[r++] = 0x0C;       // pointer to qname
        resp[r++] = 0; resp[r++] = 1;             // type A
        resp[r++] = 0; resp[r++] = 1;             // class IN
        resp[r++] = 0; resp[r++] = 0; resp[r++] = 0; resp[r++] = 60;  // ttl
        resp[r++] = 0; resp[r++] = 4;             // rdlen
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip);
        memcpy(resp + r, &ip.ip.addr, 4); r += 4;
        sendto(s_sock, resp, r, 0, (struct sockaddr*)&from, flen);
    }
    vTaskDelete(nullptr);
}

void startCaptiveDns() {
    if (s_sock >= 0) return;
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) return;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s_sock);
        s_sock = -1;
        return;
    }
    xTaskCreatePinnedToCore(dnsTask, "dns", 4096, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "captive DNS on :53");
}

void stopCaptiveDns() {
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
}
