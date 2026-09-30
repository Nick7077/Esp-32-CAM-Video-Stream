#include "web_server.h"
#include <Arduino.h>
#include "esp_http_server.h"
#include "esp_camera.h"
#include "camera.h"
#include "config.h"
#include "net.h"

#define PART_BOUNDARY "frameboundary7c2f"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_PART = "\r\n--" PART_BOUNDARY
                                  "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %ld.%06ld\r\n\r\n";

static httpd_handle_t httpServer = nullptr;
static httpd_handle_t streamServer = nullptr;

// Minimal viewer page. The stream lives on port 81, so build its URL from the page's host.
static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3-CAM</title>
<style>
 body{margin:0;font-family:system-ui,sans-serif;background:#111;color:#ddd}
 header{display:flex;flex-wrap:wrap;gap:12px;align-items:center;padding:10px 14px;background:#1b1b1b}
 img{display:block;max-width:100%;margin:0 auto;background:#000}
 select,button,input{font:inherit}
 #st{font-size:12px;opacity:.7}
</style></head><body>
<header>
 <b>ESP32-S3-CAM</b>
 <label>Size <select id="fs">
  <option>QVGA</option><option selected>VGA</option><option>SVGA</option><option>XGA</option>
  <option>HD</option><option>SXGA</option><option>UXGA</option><option>FHD</option><option>QXGA</option>
 </select></label>
 <label>Quality <input id="q" type="range" min="4" max="40" value="12"></label>
 <button id="snap">Snapshot</button>
 <span id="st"></span>
</header>
<img id="view" alt="stream">
<script>
const $=id=>document.getElementById(id);
const streamUrl=`${location.protocol}//${location.hostname}:81/stream`;
$('view').src=streamUrl;
const set=(v,x)=>fetch(`/control?var=${v}&val=${x}`).then(refresh);
$('fs').onchange=e=>set('framesize',e.target.value);
$('q').onchange=e=>set('quality',e.target.value);
$('snap').onclick=()=>window.open('/capture?t='+Date.now());
function refresh(){fetch('/status').then(r=>r.json()).then(s=>{
 const c=s.camera; $('fs').value=c.framesize; $('q').value=c.quality;
 $('st').textContent=`${c.sensor} ${c.width}x${c.height} q${c.quality} | ${s.mode} ${s.ip} ${s.rssi?s.rssi+' dBm':''} | stream: ${streamUrl}`;
});}
refresh();
</script></body></html>)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t captureHandler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  char json[1024];
  size_t n = statusJson(json, sizeof(json));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, n);
}

static esp_err_t controlHandler(httpd_req_t *req) {
  char query[96], var[32], val[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use /control?var=NAME&val=VALUE");
    return ESP_FAIL;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (!cameraSet(var, val)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown setting or bad value");
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "ok");
}

static esp_err_t streamHandler(httpd_req_t *req) {
  char part[160];
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      log_e("camera capture failed");
      res = ESP_FAIL;
      break;
    }
    int hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len,
                        (long)fb->timestamp.tv_sec, (long)fb->timestamp.tv_usec);
    res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
  }
  return res;  // client disconnected
}

void webServerStart() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = HTTP_PORT;
  cfg.max_uri_handlers = 8;
  cfg.stack_size = 8192;
  cfg.lru_purge_enable = true;  // drop dead sockets if a client vanishes mid-stream

  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr},
      {.uri = "/capture", .method = HTTP_GET, .handler = captureHandler, .user_ctx = nullptr},
      {.uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr},
      {.uri = "/control", .method = HTTP_GET, .handler = controlHandler, .user_ctx = nullptr},
  };
  if (httpd_start(&httpServer, &cfg) == ESP_OK) {
    for (const auto &r : routes) httpd_register_uri_handler(httpServer, &r);
  } else {
    log_e("failed to start HTTP server on port %d", HTTP_PORT);
  }

  // Separate server so a long-running /stream doesn't block the UI/API.
  cfg.server_port = STREAM_PORT;
  cfg.ctrl_port += 1;
  const httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr};
  if (httpd_start(&streamServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream);
  } else {
    log_e("failed to start stream server on port %d", STREAM_PORT);
  }
}
