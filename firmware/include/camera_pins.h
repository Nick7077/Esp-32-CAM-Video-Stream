#pragma once
// Camera wiring for the dual-USB-C "ESP32-S3-CAM" (N16R8) boards.
// Same pinout as Espressif's ESP32-S3-EYE / Freenove ESP32-S3-WROOM CAM,
// i.e. CAMERA_MODEL_ESP32S3_EYE in Espressif's CameraWebServer example.
// If camera init fails with 0x105/0x106, check the ribbon cable first, then these.

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM   4   // SCCB (I2C) data
#define SIOC_GPIO_NUM   5   // SCCB (I2C) clock

#define Y9_GPIO_NUM    16   // D7
#define Y8_GPIO_NUM    17   // D6
#define Y7_GPIO_NUM    18   // D5
#define Y6_GPIO_NUM    12   // D4
#define Y5_GPIO_NUM    10   // D3
#define Y4_GPIO_NUM     8   // D2
#define Y3_GPIO_NUM     9   // D1
#define Y2_GPIO_NUM    11   // D0

#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM  13

// Pins already used by the camera: 4-13, 15-18.
// Pins used by the octal PSRAM/flash on N16R8 modules: 26-37 (don't touch).
// Free-ish GPIOs for motors/sensors later: 1, 2, 14, 21, 38-42, 45-47 (check your board's silkscreen).
