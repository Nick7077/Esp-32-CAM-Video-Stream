#include "web_server.h"
#include <Arduino.h>
#include "esp_http_server.h"
#include "esp_camera.h"
#include "lwip/sockets.h"
#include "lwip/api.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
extern "C" {
#include "lwip/priv/sockets_priv.h"  // lwip_socket_dbg_get_socket(), see widenSendBuffer()
}
#include "camera.h"
#include "config.h"
#include "net.h"

#define PART_BOUNDARY "frameboundary7c2f"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_PART = "\r\n--" PART_BOUNDARY
                                  "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %ld.%06ld\r\n\r\n";

static httpd_handle_t httpServer = nullptr;
static httpd_handle_t streamServer = nullptr;

// Viewer page. The stream lives on port 81, so build its URL from the page's host.
// It reads the MJPEG stream with fetch() instead of an <img>, so it can tell when frames
// stop arriving: after STALL_MS with nothing new it drops the connection and reconnects
// (a plain <img> just sits on the last frame, or freezes for good if the stream closed).
static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3-CAM</title>
<style>
 body{margin:0;font-family:system-ui,sans-serif;background:#111;color:#ddd}
 header{display:flex;flex-wrap:wrap;gap:12px;align-items:center;padding:10px 14px;background:#1b1b1b}
 canvas{display:block;max-width:100%;height:auto;margin:0 auto;background:#000}
 #hud{position:fixed;left:8px;bottom:8px;font:12px ui-monospace,monospace;background:#000b;color:#8f8;padding:3px 7px;border-radius:4px}
 #hud.bad{color:#f88}
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
<canvas id="view" width="640" height="480"></canvas>
<div id="hud">connecting</div>
<script>
const $=id=>document.getElementById(id);
const streamUrl=`${location.protocol}//${location.hostname}:81/stream`;
const STALL_MS=1500, CONNECT_MS=5000;
const cv=$('view'), ctx=cv.getContext('2d'), hud=$('hud'), td=new TextDecoder();
let live={}, state='connecting', ctrl=null, last=performance.now(), frames=0, bytes=0, reconnects=0;

function getJSON(u,ms){const c=new AbortController(), t=setTimeout(()=>c.abort(),ms);
 return fetch(u,{signal:c.signal,cache:'no-store'}).then(r=>r.json()).finally(()=>clearTimeout(t));}
function refresh(full){return getJSON('/status',1500).then(s=>{
 const c=s.camera; live={q:c.live_quality,rssi:s.rssi};
 if(full){$('fs').value=c.framesize; $('q').value=c.quality;}
 $('st').textContent=`${c.sensor} ${c.width}x${c.height} q${c.quality} | ${s.mode} ${s.ip} ${s.rssi?s.rssi+' dBm':''} | stream: ${streamUrl}`;
}).catch(()=>{});}
const set=(v,x)=>fetch(`/control?var=${v}&val=${x}`).then(()=>refresh(true));
$('fs').onchange=e=>set('framesize',e.target.value);
$('q').onchange=e=>set('quality',e.target.value);
$('snap').onclick=()=>window.open('/capture?t='+Date.now());
refresh(true); setInterval(()=>refresh(false),2000);

// Draw only the newest frame; if decoding falls behind, skip the stale ones.
let pending=null, busy=false;
function show(jpg){
 pending=jpg; last=performance.now(); frames++; bytes+=jpg.length; state='streaming';
 if(!busy) draw();
}
async function draw(){
 busy=true;
 while(pending){
  const j=pending; pending=null;
  try{const b=await createImageBitmap(new Blob([j],{type:'image/jpeg'}));
   if(cv.width!==b.width||cv.height!==b.height){cv.width=b.width; cv.height=b.height;}
   ctx.drawImage(b,0,0); b.close();}catch(e){}
 }
 busy=false;
}

function headerEnd(b,n){for(let i=0;i+3<n;i++)if(b[i]===13&&b[i+1]===10&&b[i+2]===13&&b[i+3]===10)return i;return -1;}

async function stream(){
 for(;;){
  ctrl=new AbortController(); state='connecting'; last=performance.now();
  try{
   const r=await fetch(`${streamUrl}?t=${Date.now()}`,{signal:ctrl.signal,cache:'no-store'});
   const rd=r.body.getReader();
   let buf=new Uint8Array(1<<18), n=0;
   for(;;){
    const {value,done}=await rd.read(); if(done) break;
    if(n+value.length>buf.length){const nb=new Uint8Array(Math.max(buf.length*2,n+value.length)); nb.set(buf.subarray(0,n)); buf=nb;}
    buf.set(value,n); n+=value.length;
    for(;;){  // pull out every complete part:  --boundary / headers / blank line / JPEG
     const he=headerEnd(buf,n);
     if(he<0){ if(n>8192) throw new Error('lost sync'); break; }
     const m=/content-length:\s*(\d+)/i.exec(td.decode(buf.subarray(0,he))), start=he+4;
     if(!m){ buf.copyWithin(0,start,n); n-=start; continue; }
     const len=+m[1]; if(n<start+len) break;
     show(buf.slice(start,start+len));
     buf.copyWithin(0,start+len,n); n-=start+len;
    }
   }
  }catch(e){}
  ctrl=null; if(state!=='stalled') state='reconnecting';
  await new Promise(r=>setTimeout(r,300));
 }
}
setInterval(()=>{  // watchdog
 const limit=state==='streaming'?STALL_MS:CONNECT_MS;
 if(ctrl && performance.now()-last>limit){ state='stalled'; reconnects++; ctrl.abort(); }
},250);
setInterval(()=>{  // HUD, once a second
 const kb=frames?bytes/frames/1024:0;
 hud.textContent=state==='streaming'
  ? `${frames} fps  ${kb.toFixed(0)} kB/frame  q${live.q??'?'}  ${live.rssi?live.rssi+' dBm  ':''}reconnects ${reconnects}`
  : `${state}...  reconnects ${reconnects}`;
 hud.className=(state!=='streaming'||frames<5)?'bad':'';
 frames=bytes=0;
},1000);
stream();
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

// Busy scenes (lots of motion, or the camera itself moving) make JPEGs 2-3x bigger.
// Over TCP that can be more than the Wi-Fi link carries: sends back up, and a lost
// packet then costs a retransmission timeout, which shows up as a multi-second
// freeze. So time how long each frame takes to go out, compress harder while it's
// too slow, and ease back when there's headroom. It never goes better than the
// quality the user set.
// Arduino-ESP32 builds lwIP with a 5.7 KB TCP send buffer (CONFIG_LWIP_TCP_SND_BUF_DEFAULT),
// so only 4 segments are ever in flight. With so few, a lost segment rarely produces the
// 3 duplicate ACKs a fast retransmit needs, and TCP instead waits out a retransmission
// timeout (0.5-2 s): the short freezes. This raises the stream connection's send buffer
// to what lwIP's segment queue can hold (~16 segments, ~23 KB), so a loss is repaired in
// about one round trip. Doing it properly means a custom sdkconfig and rebuilding the whole
// framework (which breaks on paths with spaces on Windows), so it patches this one socket.
static void widenSendBuffer(int fd) {
  const tcpwnd_size_t target = (tcpwnd_size_t)((TCP_SND_QUEUELEN - 1) * TCP_MSS);
  if (target <= TCP_SND_BUF) return;
  LOCK_TCPIP_CORE();
  struct lwip_sock *sock = lwip_socket_dbg_get_socket(fd);
  if (sock && sock->conn && NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
    struct tcp_pcb *pcb = sock->conn->pcb.tcp;
    if (pcb && pcb->snd_buf <= TCP_SND_BUF) {
      pcb->snd_buf += target - TCP_SND_BUF;  // bytes already queued stay accounted for
      if (pcb->ssthresh < target) pcb->ssthresh = target;
    }
  }
  UNLOCK_TCPIP_CORE();
}

class StreamRateControl {
 public:
  void update(uint32_t sendMs) {
    uint32_t now = millis();
    avgMs_ = avgMs_ > 0 ? 0.7f * avgMs_ + 0.3f * sendMs : (float)sendMs;
    int q = cameraLiveQuality();
    int userQ = cameraUserQuality();
    int qMax = max(STREAM_MAX_AUTO_QUALITY, userQ);
    if (sendMs > 4 * STREAM_TARGET_FRAME_MS && q < qMax) {
      step(min(q + 6, qMax), now);  // hard stall: back off right away
      avgMs_ = STREAM_TARGET_FRAME_MS;
    } else if (now - lastChange_ < 300) {
      return;  // give the sensor a few frames to apply the last change
    } else if (avgMs_ > 1.3f * STREAM_TARGET_FRAME_MS && q < qMax) {
      step(min(q + 2, qMax), now);
    } else if (avgMs_ < 0.5f * STREAM_TARGET_FRAME_MS && q > userQ && now - lastChange_ > 1500) {
      step(q - 1, now);  // plenty of headroom: win quality back slowly
    }
  }

 private:
  void step(int q, uint32_t now) {
    cameraSetLiveQuality(q);
    lastChange_ = now;
  }
  float avgMs_ = 0;
  uint32_t lastChange_ = 0;
};

static esp_err_t streamHandler(httpd_req_t *req) {
  char part[160];
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  // Send every write immediately. With Nagle on, the small chunk/part headers wait
  // for the previous segment's ACK, which the PC may delay by up to 200 ms.
  int fd = httpd_req_to_sockfd(req);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  widenSendBuffer(fd);

  StreamRateControl rate;
  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      log_e("camera capture failed");
      res = ESP_FAIL;
      break;
    }
    uint32_t t0 = millis();
    int hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len,
                        (long)fb->timestamp.tv_sec, (long)fb->timestamp.tv_usec);
    res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res == ESP_OK) rate.update(millis() - t0);
  }
  cameraSetLiveQuality(cameraUserQuality());  // stream over: back to the user's setting (USB shares it)
  return res;  // client disconnected or stalled past STREAM_SEND_TIMEOUT_S
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
  cfg.send_wait_timeout = STREAM_SEND_TIMEOUT_S;  // fail fast on a stalled link (default 5 s)
  cfg.ctrl_port += 1;
  const httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr};
  if (httpd_start(&streamServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream);
  } else {
    log_e("failed to start stream server on port %d", STREAM_PORT);
  }
}
