#include "camera.h"
#include "camera_pins.h"
#include "config.h"

struct FsName {
  const char *name;
  framesize_t fs;
};

// Names -> enum values, resolved at compile time so they stay correct across
// esp32-camera versions (the enum's numbering has changed over time).
static const FsName FRAME_SIZES[] = {
    {"96X96", FRAMESIZE_96X96}, {"QQVGA", FRAMESIZE_QQVGA}, {"128X128", FRAMESIZE_128X128},
    {"QCIF", FRAMESIZE_QCIF},   {"HQVGA", FRAMESIZE_HQVGA}, {"240X240", FRAMESIZE_240X240},
    {"QVGA", FRAMESIZE_QVGA},   {"CIF", FRAMESIZE_CIF},     {"HVGA", FRAMESIZE_HVGA},
    {"VGA", FRAMESIZE_VGA},     {"SVGA", FRAMESIZE_SVGA},   {"XGA", FRAMESIZE_XGA},
    {"HD", FRAMESIZE_HD},       {"SXGA", FRAMESIZE_SXGA},   {"UXGA", FRAMESIZE_UXGA},
    {"FHD", FRAMESIZE_FHD},     {"QXGA", FRAMESIZE_QXGA},
};

const char *framesizeName(framesize_t fs) {
  for (const auto &f : FRAME_SIZES)
    if (f.fs == fs) return f.name;
  return "?";
}

static bool parseFramesize(const char *val, framesize_t *out) {
  for (const auto &f : FRAME_SIZES) {
    if (strcasecmp(val, f.name) == 0) {
      *out = f.fs;
      return true;
    }
  }
  char *end;
  long n = strtol(val, &end, 10);
  if (end != val && *end == '\0' && n >= 0 && n < FRAMESIZE_INVALID) {
    *out = (framesize_t)n;
    return true;
  }
  return false;
}

bool cameraInit() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = XCLK_FREQ_HZ;
  c.pixel_format = PIXFORMAT_JPEG;  // the sensor does JPEG compression in hardware
  c.jpeg_quality = DEFAULT_JPEG_QUALITY;

  if (psramFound()) {
    // Frame buffers are sized at init, so allocate for the sensor's largest
    // resolution (the driver clamps QXGA to the sensor max). That lets you
    // switch to any resolution later without re-initialising.
    c.frame_size = FRAMESIZE_QXGA;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.fb_count = 3;                      // Wi-Fi + USB can each hold one while the DMA fills the third
    c.grab_mode = CAMERA_GRAB_LATEST;    // always hand out the newest frame (low latency)
  } else {
    // Shouldn't happen on N16R8 - check board_build.arduino.memory_type = qio_opi
    c.frame_size = FRAMESIZE_QVGA;
    c.fb_location = CAMERA_FB_IN_DRAM;
    c.fb_count = 1;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    log_e("esp_camera_init failed: 0x%x", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    // OV3660 modules come out upside-down and a bit washed out by default.
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, psramFound() ? DEFAULT_FRAMESIZE : FRAMESIZE_QVGA);
  return true;
}

bool cameraSet(const char *var, const char *val) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !var || !val) return false;

  if (!strcmp(var, "framesize")) {
    framesize_t fs;
    if (!parseFramesize(val, &fs)) return false;
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    if (info && fs > info->max_size) return false;
    if (!psramFound() && fs > FRAMESIZE_QVGA) return false;
    return s->set_framesize(s, fs) == 0;
  }

  char *end;
  int v = (int)strtol(val, &end, 10);
  if (end == val) return false;

  int res = -1;
  if (!strcmp(var, "quality")) res = s->set_quality(s, constrain(v, 4, 63));
  else if (!strcmp(var, "brightness")) res = s->set_brightness(s, v);
  else if (!strcmp(var, "contrast")) res = s->set_contrast(s, v);
  else if (!strcmp(var, "saturation")) res = s->set_saturation(s, v);
  else if (!strcmp(var, "sharpness")) res = s->set_sharpness(s, v);
  else if (!strcmp(var, "hmirror")) res = s->set_hmirror(s, v);
  else if (!strcmp(var, "vflip")) res = s->set_vflip(s, v);
  else if (!strcmp(var, "awb")) res = s->set_whitebal(s, v);
  else if (!strcmp(var, "awb_gain")) res = s->set_awb_gain(s, v);
  else if (!strcmp(var, "wb_mode")) res = s->set_wb_mode(s, v);
  else if (!strcmp(var, "aec")) res = s->set_exposure_ctrl(s, v);
  else if (!strcmp(var, "aec2")) res = s->set_aec2(s, v);
  else if (!strcmp(var, "ae_level")) res = s->set_ae_level(s, v);
  else if (!strcmp(var, "aec_value")) res = s->set_aec_value(s, v);
  else if (!strcmp(var, "agc")) res = s->set_gain_ctrl(s, v);
  else if (!strcmp(var, "agc_gain")) res = s->set_agc_gain(s, v);
  else if (!strcmp(var, "gainceiling")) res = s->set_gainceiling(s, (gainceiling_t)v);
  else if (!strcmp(var, "special_effect")) res = s->set_special_effect(s, v);
  else if (!strcmp(var, "dcw")) res = s->set_dcw(s, v);
  else if (!strcmp(var, "bpc")) res = s->set_bpc(s, v);
  else if (!strcmp(var, "wpc")) res = s->set_wpc(s, v);
  else if (!strcmp(var, "raw_gma")) res = s->set_raw_gma(s, v);
  else if (!strcmp(var, "lenc")) res = s->set_lenc(s, v);
  return res == 0;
}

size_t cameraStatusJson(char *out, size_t len) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return snprintf(out, len, "{\"ok\":false}");
  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  framesize_t fs = s->status.framesize;
  int n = snprintf(out, len,
                   "{\"ok\":true,\"sensor\":\"%s\",\"framesize\":\"%s\",\"width\":%u,\"height\":%u,"
                   "\"quality\":%u,\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
                   "\"hmirror\":%u,\"vflip\":%u,\"awb\":%u,\"aec\":%u,\"agc\":%u}",
                   info ? info->name : "unknown", framesizeName(fs), resolution[fs].width,
                   resolution[fs].height, s->status.quality, s->status.brightness,
                   s->status.contrast, s->status.saturation, s->status.hmirror, s->status.vflip,
                   s->status.awb, s->status.aec, s->status.agc);
  return n < 0 ? 0 : ((size_t)n >= len ? len - 1 : (size_t)n);
}
