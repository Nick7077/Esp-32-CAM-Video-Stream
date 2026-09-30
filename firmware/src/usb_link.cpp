#include "usb_link.h"
#include <stdarg.h>
#include "esp_camera.h"
#include "camera.h"
#include "net.h"

static const uint8_t FRAME_MAGIC[4] = {0xA5, 0x5A, 0xC3, 0x3C};

static SemaphoreHandle_t txMutex = nullptr;   // one writer at a time on the USB link
static volatile bool streaming = false;
bool (*usbCustomCommand)(const char *line) = nullptr;

void usbBegin() {
  Serial.setTxBufferSize(16 * 1024);  // must precede begin(); bigger ring = higher throughput
  Serial.setRxBufferSize(512);
  Serial.begin(115200);               // baud is ignored on native USB
  Serial.setTxTimeoutMs(50);          // per-call; writeAll() retries up to ~1 s of no progress
  // Also mirror text (not video) to the UART port (CH340 / 'COM' USB-C) so the
  // Serial Monitor shows the IP whichever port you're plugged into.
  Serial0.begin(115200);
  txMutex = xSemaphoreCreateMutex();
}

void usbPrintf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (txMutex) xSemaphoreTake(txMutex, portMAX_DELAY);
  size_t len = min((size_t)n, sizeof(buf) - 1);
  Serial.write((const uint8_t *)buf, len);
  Serial0.write((const uint8_t *)buf, len);
  if (txMutex) xSemaphoreGive(txMutex);
}

// Writes every byte, riding out short stalls (host briefly busy decoding etc.).
// Serial.write() on the native USB port returns a *partial* count after the TX
// timeout; a single unchecked call used to truncate frames and desync the stream.
static bool writeAll(const uint8_t *p, size_t n, uint32_t stallMs = 1000) {
  uint32_t lastProgress = millis();
  while (n) {
    size_t w = Serial.write(p, n);
    if (w) {
      p += w;
      n -= w;
      lastProgress = millis();
    } else if (!HWCDC::isConnected() || millis() - lastProgress > stallMs) {
      return false;
    } else {
      vTaskDelay(1);
    }
  }
  return true;
}

// Sends one frame. Returns false if the host isn't keeping up / went away.
static bool sendFrame() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    usbPrintf("err: camera capture failed\n");
    return true;  // camera hiccup, not a link problem
  }
  uint8_t hdr[12];
  uint32_t len = fb->len;
  uint32_t ts = (uint32_t)(fb->timestamp.tv_sec * 1000 + fb->timestamp.tv_usec / 1000);
  memcpy(hdr, FRAME_MAGIC, 4);
  memcpy(hdr + 4, &len, 4);  // ESP32 is little-endian
  memcpy(hdr + 8, &ts, 4);

  xSemaphoreTake(txMutex, portMAX_DELAY);
  bool ok = writeAll(hdr, sizeof(hdr)) && writeAll(fb->buf, fb->len);
  xSemaphoreGive(txMutex);
  esp_camera_fb_return(fb);
  return ok && HWCDC::isConnected();
}

static void handleCommand(char *line) {
  // trim
  while (*line == ' ' || *line == '\t') line++;
  size_t n = strlen(line);
  while (n && (line[n - 1] == ' ' || line[n - 1] == '\r' || line[n - 1] == '\t')) line[--n] = 0;
  if (!n) return;

  if (!strcmp(line, "stream on") || !strcmp(line, "start")) {
    streaming = true;
    usbPrintf("ok stream on\n");
  } else if (!strcmp(line, "stream off") || !strcmp(line, "stop")) {
    streaming = false;
    usbPrintf("ok stream off\n");
  } else if (!strcmp(line, "snap")) {
    sendFrame();
  } else if (!strcmp(line, "status")) {
    char json[1024];
    statusJson(json, sizeof(json));
    usbPrintf("%s\n", json);
  } else if (!strncmp(line, "set ", 4)) {
    char var[32], val[32];
    if (sscanf(line + 4, "%31s %31s", var, val) == 2 && cameraSet(var, val))
      usbPrintf("ok set %s %s\n", var, val);
    else
      usbPrintf("err: bad setting '%s'\n", line + 4);
  } else if (!strcmp(line, "help")) {
    usbPrintf("commands: stream on | stream off | snap | status | set <var> <val> | help\n"
              "  e.g. set framesize SVGA   set quality 10   set hmirror 1\n");
  } else if (!(usbCustomCommand && usbCustomCommand(line))) {
    usbPrintf("err: unknown command '%s' (try help)\n", line);
  }
}

struct LineReader {
  char line[96];
  size_t pos = 0;
  void poll(Stream &port) {
    while (port.available()) {
      int c = port.read();
      if (c == '\n' || c == '\r') {
        line[pos] = 0;
        if (pos) handleCommand(line);
        pos = 0;
      } else if (pos < sizeof(line) - 1) {
        line[pos++] = (char)c;
      }
    }
  }
};

static void usbTask(void *) {
  static LineReader usbIn, uartIn;
  for (;;) {
    usbIn.poll(Serial);    // native USB port (commands + video)
    uartIn.poll(Serial0);  // UART / CH340 port (commands only, e.g. typing `status`)
    if (streaming) {
      if (!HWCDC::isConnected()) {
        vTaskDelay(pdMS_TO_TICKS(50));   // no host: don't burn camera frames
      } else if (!sendFrame()) {
        // Stalled or truncated frame. Stay in streaming mode (the host resyncs on the
        // next frame magic) instead of stopping for good, just back off briefly.
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

void usbStartTask() {
  // Core 0 keeps loop() (core 1) free for your own robotics code.
  xTaskCreatePinnedToCore(usbTask, "usb_link", 8192, nullptr, 2, nullptr, 0);
}
