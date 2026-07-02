// ESP32-CAM IP Camera — v1
// AI-Thinker board, PlatformIO + Arduino. Live MJPEG stream + snapshot, LAN-only.
//
// Boot flow: init camera (SVGA / q12 / fb_count 2 / GRAB_LATEST)
//            -> connect WiFi with a static IP -> start mDNS -> start HTTP servers.

#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include "secrets.h"
#include "camera_pins.h"

// Defined in app_httpd.cpp — starts the control server (port 80) and stream
// server (port 81).
void startCameraServer();

static void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  // 10 MHz (vs the usual 20) gives the DMA capture path maximum margin against
  // dropped bytes, which show up as random colored horizontal lines. This is an
  // AI feed — FPS is irrelevant, clean frames are everything.
  config.xclk_freq_hz = 10000000;
  config.frame_size   = FRAMESIZE_UXGA;         // 1600x1200 — max detail for AI
  config.pixel_format = PIXFORMAT_JPEG;         // streaming
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY; // only complete, in-order frames
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;                  // lower = better quality / bigger; 12 is the safe floor at UXGA
  config.fb_count     = 2;                   // double buffer (needs PSRAM)

  if (!psramFound()) {
    // No PSRAM: fall back to a config that fits internal RAM so we still boot.
    Serial.println("[cam] WARNING: PSRAM not found — falling back to VGA/single buffer");
    config.frame_size  = FRAMESIZE_VGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count    = 1;
    config.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed (0x%x) — check ribbon cable & power, then reset\n", err);
    // Nothing works without the sensor; reboot to retry rather than hang half-alive.
    delay(3000);
    ESP.restart();
  }

  // Sensor is up. Report which one and apply a couple of sensible defaults.
  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("[cam] sensor PID: 0x%x (%s)\n", s->id.PID,
                s->id.PID == OV3660_PID ? "OV3660" :
                s->id.PID == OV2640_PID ? "OV2640" : "unknown");

  if (s->id.PID == OV3660_PID) {
    // OV3660 ships slightly washed out & upside-down on this board — correct it.
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
}

static void connectWiFi() {
  IPAddress ip(STATIC_IP);
  IPAddress gw(GATEWAY_IP);
  IPAddress mask(SUBNET_MASK);
  IPAddress dns1(PRIMARY_DNS);
  IPAddress dns2(SECONDARY_DNS);

  WiFi.mode(WIFI_STA);
  if (!WiFi.config(ip, gw, mask, dns1, dns2)) {
    Serial.println("[wifi] static IP config failed — will use DHCP instead");
  }

  Serial.printf("[wifi] connecting to \"%s\"", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // keep the stream responsive

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {  // 20s: give up and reboot to retry cleanly
      Serial.println("\n[wifi] timeout — restarting");
      ESP.restart();
    }
  }
  // Reduce radio TX power to shrink the current spikes that stack on top of the
  // camera's capture spikes (a cause of frame corruption on power-marginal boards).
  // Safe here — signal is strong. Raise if the link ever drops.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  Serial.printf("\n[wifi] connected, IP: %s  RSSI: %d dBm  (TX power reduced)\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static void setupOTA() {
  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("\n[ota] update starting"); });
  ArduinoOTA.onEnd([]()   { Serial.println("\n[ota] done — rebooting"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[ota] %u%%\r", (p * 100) / t);
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
  ArduinoOTA.begin();
  Serial.println("[ota] ready for wireless updates");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println("\n\n=== ESP32-CAM IP Camera (v1) ===");

  initCamera();
  connectWiFi();

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mdns] http://%s.local/\n", MDNS_HOSTNAME);
  } else {
    Serial.println("[mdns] failed to start (browse by IP instead)");
  }

  setupOTA();

  startCameraServer();
  Serial.printf("[http] ready:  http://%s/    stream on :81\n",
                WiFi.localIP().toString().c_str());
}

void loop() {
  ArduinoOTA.handle();  // must be serviced often for wireless updates to work

  // Throttled link check: reconnect if WiFi drops (static IP re-applies on reconnect).
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 5000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[wifi] link lost — reconnecting");
      WiFi.reconnect();
    }
  }
  delay(10);
}
