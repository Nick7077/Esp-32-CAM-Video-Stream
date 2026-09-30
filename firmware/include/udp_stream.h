#pragma once
#include <Arduino.h>

// Low-latency Wi-Fi video over UDP (port UDP_PORT in config.h).
//
// Why: the MJPEG stream on :81 runs over TCP. On a flaky Wi-Fi link a single lost
// packet makes TCP stall everything behind it, which shows up as freezes followed
// by bursts. Over UDP a lost packet only costs that one frame; the next frame
// arrives on time.
//
// Protocol (all little-endian):
//   host -> ESP32 :UDP_PORT   "SUB"  every ~0.5 s (subscribe + keepalive).
//                              The ESP32 streams to the sender's IP:port until
//                              it hasn't heard from it for UDP_CLIENT_TIMEOUT_MS.
//   ESP32 -> host             one datagram per JPEG chunk:
//       char[2]  magic "E3"
//       uint8    version (1)
//       uint8    session       random per boot, so the PC notices a reboot
//       uint32   frame_id      increments per frame
//       uint32   frame_len     total JPEG size
//       uint32   timestamp_ms  capture time since boot
//       uint16   chunk_index
//       uint16   chunk_count
//       ...      payload (<= UDP_CHUNK bytes)
// host/esp32cam.py UdpCamera implements the receiving side.

void udpStreamStart();

// JSON object with UDP stream stats (client, fps, dropped frames).
size_t udpStreamStatsJson(char *out, size_t len);
