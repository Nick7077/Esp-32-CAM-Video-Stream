#pragma once
#include <Arduino.h>

// Join WIFI_SSID (station mode); if that's blank or fails, start an access point.
void netBegin();
bool netIsAP();
String netIP();

// Full device status as JSON: network + camera. Used by /status and the USB `status` command.
size_t statusJson(char *out, size_t len);
