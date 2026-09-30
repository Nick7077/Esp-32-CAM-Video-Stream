#include "udp_stream.h"
#include <errno.h>
#include "esp_camera.h"
#include "lwip/sockets.h"
#include "config.h"

struct __attribute__((packed)) UdpHeader {
  char magic[2];
  uint8_t version;
  uint8_t session;
  uint32_t frameId;
  uint32_t frameLen;
  uint32_t timestampMs;
  uint16_t chunkIndex;
  uint16_t chunkCount;
};
static_assert(sizeof(UdpHeader) == 20, "header must be 20 bytes");

static int sock = -1;
static sockaddr_in client = {};
static uint32_t lastHeardMs = 0;
static bool haveClient = false;
static uint8_t bootSession = 0;

// stats
static uint32_t framesSent = 0, framesDropped = 0;
static float fps = 0;

static bool clientActive() { return haveClient && millis() - lastHeardMs < UDP_CLIENT_TIMEOUT_MS; }

static void pollControl() {
  char buf[32];
  sockaddr_in from = {};
  socklen_t fromLen = sizeof(from);
  for (;;) {
    int n = recvfrom(sock, buf, sizeof(buf) - 1, MSG_DONTWAIT, (sockaddr *)&from, &fromLen);
    if (n <= 0) return;
    if (n >= 3 && memcmp(buf, "SUB", 3) == 0) {
      bool isNew = !clientActive() || from.sin_addr.s_addr != client.sin_addr.s_addr ||
                   from.sin_port != client.sin_port;
      client = from;
      haveClient = true;
      lastHeardMs = millis();
      if (isNew) log_i("UDP client %s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    }
  }
}

// Send one datagram. When the Wi-Fi TX queue is full lwIP returns ENOMEM; back off
// briefly and retry instead of dropping the packet (that's what keeps frames whole).
static bool sendPacket(const uint8_t *data, size_t len) {
  for (int attempt = 0; attempt < 40; attempt++) {
    int r = sendto(sock, data, len, 0, (const sockaddr *)&client, sizeof(client));
    if (r == (int)len) return true;
    if (errno == ENOMEM || errno == EAGAIN || errno == ENOBUFS) {
      vTaskDelay(1);
      continue;
    }
    return false;
  }
  return false;
}

static void udpTask(void *) {
  static uint8_t pkt[sizeof(UdpHeader) + UDP_CHUNK];
  uint32_t frameId = 0;
  uint32_t lastFrameMs = 0;

  for (;;) {
    pollControl();
    if (!clientActive()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    UdpHeader h = {};
    h.magic[0] = 'E';
    h.magic[1] = '3';
    h.version = 1;
    h.session = bootSession;
    h.frameId = frameId++;
    h.frameLen = fb->len;
    h.timestampMs = (uint32_t)(fb->timestamp.tv_sec * 1000 + fb->timestamp.tv_usec / 1000);
    h.chunkCount = (fb->len + UDP_CHUNK - 1) / UDP_CHUNK;

    bool ok = true;
    for (uint16_t i = 0; i < h.chunkCount && ok; i++) {
      size_t off = (size_t)i * UDP_CHUNK;
      size_t n = min((size_t)UDP_CHUNK, fb->len - off);
      h.chunkIndex = i;
      memcpy(pkt, &h, sizeof(h));
      memcpy(pkt + sizeof(h), fb->buf + off, n);
      ok = sendPacket(pkt, sizeof(h) + n);
    }
    esp_camera_fb_return(fb);

    uint32_t now = millis();
    if (ok) {
      framesSent++;
      if (lastFrameMs) {
        uint32_t dt = now - lastFrameMs;
        float inst = 1000.0f / (dt ? dt : 1);
        fps = fps ? 0.9f * fps + 0.1f * inst : inst;
      }
      lastFrameMs = now;
    } else {
      framesDropped++;
    }
    vTaskDelay(1);  // let loop() and other tasks on this core breathe
  }
}

void udpStreamStart() {
  bootSession = (uint8_t)esp_random();
  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    log_e("UDP socket failed");
    return;
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(UDP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
    log_e("UDP bind failed");
    close(sock);
    sock = -1;
    return;
  }
  // Core 1 keeps it off the core that runs the Wi-Fi driver.
  xTaskCreatePinnedToCore(udpTask, "udp_stream", 6144, nullptr, 2, nullptr, 1);
}

size_t udpStreamStatsJson(char *out, size_t len) {
  bool active = clientActive();
  int n = snprintf(out, len, "{\"port\":%d,\"client\":\"%s:%u\",\"active\":%s,\"fps\":%.1f,\"sent\":%lu,\"dropped\":%lu}",
                   UDP_PORT, haveClient ? inet_ntoa(client.sin_addr) : "", haveClient ? ntohs(client.sin_port) : 0,
                   active ? "true" : "false", active ? fps : 0.0f, (unsigned long)framesSent,
                   (unsigned long)framesDropped);
  return n < 0 ? 0 : ((size_t)n >= len ? len - 1 : (size_t)n);
}
