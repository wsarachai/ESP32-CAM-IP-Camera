// Stripped CameraWebServer for the ESP32-CAM IP Camera.
//
// Kept: control web UI (port 80), MJPEG stream (port 81), snapshot, live camera
//       controls (resolution, quality, brightness, mirror/flip, etc.).
// Removed: all face detection / recognition (esp-face) — saves flash & RAM and
//          keeps the stream fast.

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"

// ---- MJPEG multipart stream framing --------------------------------------
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

static httpd_handle_t camera_httpd = NULL;  // port 80: UI + control + capture
static httpd_handle_t stream_httpd = NULL;  // port 81: MJPEG stream

// Onboard white flash LED (AI-Thinker board). Driven via LEDC PWM so it can be
// dimmed; uses channel 2 to stay clear of the camera's XCLK on channel 0.
#define LED_GPIO_NUM      4
#define LED_LEDC_CHANNEL  2
static int led_intensity = 0;  // 0 (off) .. 255 (full)

static void setupLedFlash() {
  ledcSetup(LED_LEDC_CHANNEL, 5000, 8);   // 5 kHz, 8-bit duty (0..255)
  ledcAttachPin(LED_GPIO_NUM, LED_LEDC_CHANNEL);
  ledcWrite(LED_LEDC_CHANNEL, led_intensity);  // start off
}

// ---- Web UI --------------------------------------------------------------
// Minimal but functional control panel. The stream <img> points at :81.
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM</title>
<style>
  body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}
  header{padding:10px 14px;background:#1b1b1b;font-weight:600}
  .wrap{display:flex;flex-wrap:wrap;gap:14px;padding:14px}
  .view{flex:1 1 480px;min-width:320px}
  img{width:100%;border-radius:8px;background:#000;display:block}
  .panel{flex:0 0 260px;background:#1b1b1b;border-radius:8px;padding:12px}
  .row{display:flex;align-items:center;justify-content:space-between;margin:10px 0}
  label{font-size:14px}
  input[type=range]{width:130px}
  select,button{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:6px;padding:6px 10px}
  button{cursor:pointer}
  button.primary{background:#2d6cdf;border-color:#2d6cdf}
  .btns{display:flex;gap:8px;margin-top:8px}
</style>
</head>
<body>
<header>ESP32-CAM &middot; live</header>
<div class="wrap">
  <div class="view">
    <img id="stream" src="">
    <div class="btns">
      <button class="primary" id="toggle">Start stream</button>
      <button id="snap">Save snapshot</button>
    </div>
  </div>
  <div class="panel">
    <div class="row">
      <label for="framesize">Resolution</label>
      <select id="framesize">
        <option value="13">UXGA 1600x1200</option>
        <option value="11">HD 1280x720</option>
        <option value="9">XGA 1024x768</option>
        <option value="8" selected>SVGA 800x600</option>
        <option value="6">VGA 640x480</option>
        <option value="5">CIF 400x296</option>
        <option value="4">QVGA 320x240</option>
      </select>
    </div>
    <div class="row"><label>Quality</label><input type="range" id="quality" min="4" max="63" value="12"></div>
    <div class="row"><label>Brightness</label><input type="range" id="brightness" min="-2" max="2" value="0"></div>
    <div class="row"><label>Contrast</label><input type="range" id="contrast" min="-2" max="2" value="0"></div>
    <div class="row"><label>Saturation</label><input type="range" id="saturation" min="-2" max="2" value="0"></div>
    <div class="row"><label>Flash LED</label><input type="range" id="led_intensity" min="0" max="255" value="0"></div>
    <div class="row"><label for="hmirror">H-Mirror</label><input type="checkbox" id="hmirror"></div>
    <div class="row"><label for="vflip">V-Flip</label><input type="checkbox" id="vflip"></div>
  </div>
</div>
<script>
  var base = document.location.origin;
  var streamUrl = base + ':81/stream';
  var img = document.getElementById('stream');
  var toggle = document.getElementById('toggle');
  var streaming = false;

  function startStream(){ img.src = streamUrl; streaming = true; toggle.textContent = 'Stop stream'; }
  function stopStream(){ window.stop(); img.src = ''; streaming = false; toggle.textContent = 'Start stream'; }
  toggle.onclick = function(){ streaming ? stopStream() : startStream(); };

  document.getElementById('snap').onclick = function(){
    var a = document.createElement('a');
    a.href = base + '/capture?_=' + Date.now();
    a.download = 'esp32cam_' + Date.now() + '.jpg';
    document.body.appendChild(a); a.click(); a.remove();
  };

  function setControl(v, val){ fetch(base + '/control?var=' + v + '&val=' + val); }
  function bindRange(id){ var e = document.getElementById(id); e.onchange = function(){ setControl(id, e.value); }; }
  ['quality','brightness','contrast','saturation','framesize','led_intensity'].forEach(bindRange);
  document.getElementById('hmirror').onchange = function(){ setControl('hmirror', this.checked?1:0); };
  document.getElementById('vflip').onchange   = function(){ setControl('vflip',   this.checked?1:0); };

  // Load current sensor state so the controls reflect reality.
  fetch(base + '/status').then(r=>r.json()).then(function(s){
    for (var k in s){ var e = document.getElementById(k); if(!e) continue;
      if (e.type === 'checkbox') e.checked = !!s[k]; else e.value = s[k]; }
  }).catch(()=>{});

  startStream();
</script>
</body>
</html>)HTML";

// ---- Handlers ------------------------------------------------------------

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "identity");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// A structurally valid JPEG starts with SOI (FF D8) and ends with EOI (FF D9).
// Truncated/byte-dropped frames fail this — cheap way to reject obvious garbage.
static bool looks_like_jpeg(camera_fb_t *fb) {
  return fb && fb->format == PIXFORMAT_JPEG && fb->len > 100 &&
         fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
         fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
}

// Grab a frame that passes the JPEG sanity check, retrying a few times.
// Favors returning a clean still over returning quickly (AI use-case).
// Non-static: also used by the periodic snapshot push in main.cpp.
camera_fb_t *grab_validated_frame(int max_tries) {
  camera_fb_t *fb = NULL;
  for (int i = 0; i < max_tries; i++) {
    fb = esp_camera_fb_get();
    if (looks_like_jpeg(fb)) return fb;
    if (fb) esp_camera_fb_return(fb);  // drop bad frame, try again
  }
  return esp_camera_fb_get();  // last resort: whatever we can get
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = grab_validated_frame(5);
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = ESP_OK;
  if (fb->format == PIXFORMAT_JPEG) {
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    // Non-JPEG buffer (shouldn't happen with our config) — convert on the fly.
    uint8_t *jpg = NULL; size_t jpg_len = 0;
    bool ok = frame2jpg(fb, 80, &jpg, &jpg_len);
    if (ok) { res = httpd_resp_send(req, (const char *)jpg, jpg_len); free(jpg); }
    else    { httpd_resp_send_500(req); res = ESP_FAIL; }
  }
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

  char part_buf[128];  // must fit the full header incl. 10-digit epoch + usec
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }

    // Drop obviously corrupt (truncated) frames so the live view doesn't flash garbage.
    if (fb->format == PIXFORMAT_JPEG && !looks_like_jpeg(fb)) {
      esp_camera_fb_return(fb);
      continue;
    }

    uint8_t *jpg = fb->buf; size_t jpg_len = fb->len; bool converted = false;
    if (fb->format != PIXFORMAT_JPEG) {
      converted = frame2jpg(fb, 80, &jpg, &jpg_len);
      if (!converted) { esp_camera_fb_return(fb); res = ESP_FAIL; break; }
    }

    struct timeval ts; gettimeofday(&ts, NULL);
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART,
                           jpg_len, (int)ts.tv_sec, (int)ts.tv_usec);

    if ((res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY))) == ESP_OK)
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)jpg, jpg_len);

    if (converted) free(jpg);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;  // client disconnected
  }
  return res;
}

// /control?var=<name>&val=<int> — live sensor tweaks from the UI.
static esp_err_t control_handler(httpd_req_t *req) {
  char query[128]; char var[32]; char valstr[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", valstr, sizeof(valstr)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
    return ESP_FAIL;
  }
  int val = atoi(valstr);
  sensor_t *s = esp_camera_sensor_get();
  int r = 0;
  if      (!strcmp(var, "framesize"))  r = s->set_framesize(s, (framesize_t)val);
  else if (!strcmp(var, "quality"))    r = s->set_quality(s, val);
  else if (!strcmp(var, "brightness")) r = s->set_brightness(s, val);
  else if (!strcmp(var, "contrast"))   r = s->set_contrast(s, val);
  else if (!strcmp(var, "saturation")) r = s->set_saturation(s, val);
  else if (!strcmp(var, "hmirror"))    r = s->set_hmirror(s, val);
  else if (!strcmp(var, "vflip"))      r = s->set_vflip(s, val);
  else if (!strcmp(var, "led_intensity")) {
    led_intensity = val < 0 ? 0 : (val > 255 ? 255 : val);
    ledcWrite(LED_LEDC_CHANNEL, led_intensity);
  }
  else { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown var"); return ESP_FAIL; }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0) == ESP_OK && r == 0 ? ESP_OK : ESP_OK;
}

// /status — current sensor state as JSON so the UI can sync its controls.
static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  char json[256];
  snprintf(json, sizeof(json),
           "{\"framesize\":%u,\"quality\":%u,\"brightness\":%d,\"contrast\":%d,"
           "\"saturation\":%d,\"led_intensity\":%d,\"hmirror\":%u,\"vflip\":%u}",
           s->status.framesize, s->status.quality, s->status.brightness,
           s->status.contrast, s->status.saturation, led_intensity,
           s->status.hmirror, s->status.vflip);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

void startCameraServer() {
  setupLedFlash();  // init the flash LED (starts off)

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri   = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,   .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t control_uri = { .uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = NULL };
  httpd_uri_t status_uri  = { .uri = "/status",  .method = HTTP_GET, .handler = status_handler,  .user_ctx = NULL };
  httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
  }

  // Stream lives on its own port so a long-running MJPEG connection never
  // blocks the control UI.
  config.server_port += 1;   // 81
  config.ctrl_port   += 1;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}
