#pragma once
#include "esp_camera.h"

// ---- Wi-Fi -----------------------------------------------------------------
// Put your network in include/secrets.h (copy secrets.h.example). It is optional:
// without it, or if joining fails, the board starts its own access point instead.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

#define HOSTNAME "esp32cam"            // -> http://esp32cam.local (station mode)
#define AP_SSID  "ESP32S3-CAM"         // fallback access point (IP 192.168.4.1)
#define AP_PASS  "esp32cam"            // >= 8 chars
#define WIFI_CONNECT_TIMEOUT_MS 15000

// ---- Camera defaults (changeable at runtime over HTTP or USB) --------------
#define DEFAULT_FRAMESIZE    FRAMESIZE_VGA   // 640x480: good baseline for vision models
#define DEFAULT_JPEG_QUALITY 12              // 0-63, lower = better image, bigger frames
#define XCLK_FREQ_HZ         20000000

// ---- Ports ------------------------------------------------------------------
#define HTTP_PORT   80   // web UI, /capture, /status, /control
#define STREAM_PORT 81   // /stream (MJPEG) on its own server so it doesn't block the UI

// ---- UDP stream (low-latency Wi-Fi video, see udp_stream.h) -------------------
#define UDP_PORT 5005
#define UDP_CHUNK 1440                  // payload bytes per datagram (fits one Wi-Fi frame)
#define UDP_CLIENT_TIMEOUT_MS 3000      // stop sending if the PC stops saying "SUB"

// ---- Browser / MJPEG stream (port 81) -----------------------------------------
// Busy scenes and camera motion make JPEGs 2-3x bigger. The stream times how long each
// frame takes to send and compresses harder while Wi-Fi can't keep up (streamHandler in
// web_server.cpp). It never goes better than the quality you set, and resets when the
// stream closes.
#define STREAM_TARGET_FRAME_MS  70   // aim: each frame fully sent within this (~14 fps floor)
#define STREAM_MAX_AUTO_QUALITY 40   // most compression auto mode will use (higher = smaller)
#define STREAM_SEND_TIMEOUT_S   2    // drop a stalled connection after this; the page reconnects
