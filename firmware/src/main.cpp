// ESP32-S3-CAM baseline streamer
//  - Wi-Fi: MJPEG at http://<ip>:81/stream  (+ web UI on port 80)
//  - USB:   framed JPEGs over the native USB-C port (see usb_link.h)
// Both work at the same time. host/viewer.py can read either.

#include <Arduino.h>
#include "camera.h"
#include "config.h"
#include "net.h"
#include "udp_stream.h"
#include "usb_link.h"
#include "web_server.h"

static bool cameraOk = false;

void setup() {
  usbBegin();
  delay(1500);  // give the PC a moment to open the port so you see the boot messages

  cameraOk = cameraInit();
  usbPrintf("%s\n", cameraOk ? "camera ok" : "err: camera init failed - check the ribbon cable / camera_pins.h");

  netBegin();
  webServerStart();
  udpStreamStart();
  usbStartTask();

  String ip = netIP();
  if (netIsAP())
    usbPrintf("wifi: no network joined -> started access point '%s' (password '%s')\n", AP_SSID, AP_PASS);
  usbPrintf("ready  UI: http://%s/   stream: http://%s:81/stream   udp: %s:%d   (type 'status' any time)\n",
            ip.c_str(), ip.c_str(), ip.c_str(), UDP_PORT);
}

void loop() {
  // Free for your own code (motor control, sensors, ...). Streaming runs in
  // other tasks, so keep this non-blocking-ish and use usbPrintf() for output.
  //
  // To add your own USB commands (e.g. "drive 100 100"), set usbCustomCommand
  // in setup() (the handler runs in the USB task on core 0):
  //   usbCustomCommand = [](const char *line) {
  //     int l, r;
  //     if (sscanf(line, "drive %d %d", &l, &r) == 2) { /* set motors */ return true; }
  //     return false;
  //   };
  delay(10);
}
