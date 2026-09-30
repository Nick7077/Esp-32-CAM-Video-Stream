#include "net.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "camera.h"
#include "udp_stream.h"
#include "config.h"

static bool apMode = false;

static bool joinStation() {
  if (strlen(WIFI_SSID) == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    return false;
  }
  WiFi.setAutoReconnect(true);
  return true;
}

void netBegin() {
  if (joinStation()) {
    apMode = false;
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
  }
  // Modem sleep adds 100+ ms of latency to every frame; turn it off for streaming.
  WiFi.setSleep(false);
}

bool netIsAP() { return apMode; }

String netIP() { return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }

size_t statusJson(char *out, size_t len) {
  char cam[384], udp[160];
  cameraStatusJson(cam, sizeof(cam));
  udpStreamStatsJson(udp, sizeof(udp));
  int n = snprintf(out, len,
                   "{\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"host\":\"%s.local\","
                   "\"uptime_s\":%lu,\"free_heap\":%u,\"free_psram\":%u,\"camera\":%s,\"udp\":%s}",
                   apMode ? "AP" : "STA", netIP().c_str(), apMode ? AP_SSID : WIFI_SSID,
                   apMode ? 0 : (int)WiFi.RSSI(), HOSTNAME, (unsigned long)(millis() / 1000),
                   (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), cam, udp);
  return n < 0 ? 0 : ((size_t)n >= len ? len - 1 : (size_t)n);
}
