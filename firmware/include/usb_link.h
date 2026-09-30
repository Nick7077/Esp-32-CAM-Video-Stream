#pragma once
#include <Arduino.h>

// USB link over the native USB-C port (`Serial`, USB-Serial/JTAG, ~12 Mbit/s).
//
// Host -> ESP32: newline-terminated text commands
//   stream on | stream off | snap | status | set <var> <val> | help
//
// ESP32 -> host: a mix of
//   * text lines (replies / logs), and
//   * binary JPEG frames:  [A5 5A C3 3C][uint32 len LE][uint32 timestamp_ms LE][len bytes JPEG]
// host/esp32cam.py demuxes the two.

void usbBegin();        // call first thing in setup()
void usbStartTask();    // starts the command/stream task (after camera + Wi-Fi are up)

// printf that is safe to use while frames are streaming (never lands inside a frame).
// Use this instead of Serial.print anywhere in your own code.
void usbPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Optional hook for your own commands (e.g. robot motion): return true if handled.
// Anything the built-in parser doesn't recognise is passed here.
extern bool (*usbCustomCommand)(const char *line);
