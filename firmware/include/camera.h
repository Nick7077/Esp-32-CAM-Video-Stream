#pragma once
#include <Arduino.h>
#include "esp_camera.h"

// Initialise the OV3660 (also works with OV2640/OV5640 on the same connector).
bool cameraInit();

// Change a sensor setting by name. Same names as Espressif's CameraWebServer, e.g.
//   framesize  QVGA|VGA|SVGA|XGA|HD|SXGA|UXGA|FHD|QXGA  (or the enum number)
//   quality    4..63 (lower = better)
//   brightness / contrast / saturation   -2..2
//   hmirror / vflip / awb / aec / agc / aec2 / dcw / bpc / wpc / lenc   0|1
//   aec_value 0..1200, agc_gain 0..30, gainceiling 0..6, special_effect 0..6, wb_mode 0..4
// Returns false for unknown names or values the sensor rejects.
bool cameraSet(const char *var, const char *val);

// Writes a JSON object describing the camera settings. Returns length written.
size_t cameraStatusJson(char *out, size_t len);

const char *framesizeName(framesize_t fs);
