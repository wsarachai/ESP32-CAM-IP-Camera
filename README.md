# ESP32-CAM IP Camera (v1)

A LAN-only IP camera on the AI-Thinker **ESP32-CAM**, flashed via the
**ESP32-CAM-MB** (CH340G) adapter. Live MJPEG stream + on-demand snapshot,
viewable in any browser.

## Design (v1)

| Decision | Choice |
|---|---|
| Toolchain | PlatformIO + Arduino (`board = esp32cam`) |
| Consumption | Browser MJPEG stream + web control UI |
| Base code | Stock `CameraWebServer`, **face detection stripped out** |
| Scope | Live viewer + snapshot only (no SD, no LED, no motion) |
| Camera default | SVGA 800×600, JPEG quality 12, `fb_count=2`, `GRAB_LATEST` |
| WiFi | Hardcoded in git-ignored `include/secrets.h` |
| Addressing | Static IP in firmware + mDNS (`esp32cam.local`) |
| Partition | `huge_app.csv` (3 MB app, no OTA) |
| Updates | USB reflash |
| Security | LAN-only, no auth — **never port-forward this** |
| Power | Dev via MB micro-USB; deploy on a dedicated **5V 2A** supply |

## First-time setup

1. Copy the secrets template and fill in your network:
   ```
   cp include/secrets.example.h include/secrets.h
   ```
   Edit `include/secrets.h` — set `WIFI_SSID`, `WIFI_PASS`, and a `STATIC_IP`
   **outside your router's DHCP pool**.

2. Build & flash (ESP32-CAM-MB plugged into USB):
   ```
   pio run --target upload
   pio device monitor
   ```
   The MB adapter handles boot mode + auto-reset — no GPIO0 jumper needed.

3. Watch the serial monitor (115200) for the sensor type and the line:
   ```
   [http] ready:  http://192.168.1.50/    stream on :81
   ```
   Open that URL (or `http://esp32cam.local/`) in a browser.

## Endpoints

| URL | Purpose |
|---|---|
| `http://<ip>/` | Control UI (port 80) |
| `http://<ip>:81/stream` | MJPEG stream |
| `http://<ip>/capture` | Single JPEG snapshot |
| `http://<ip>/control?var=<name>&val=<n>` | Live sensor tweak (incl. `led_intensity` 0–255) |
| `http://<ip>/status` | Current sensor settings (JSON) |

## Troubleshooting

- **Reboots / brownouts / garbage frames** → power first. Use a real 5V 2A
  supply and a short, thick cable. Weak USB ports sag under the camera's
  current spikes.
- **`camera init failed`** → reseat the ribbon cable; confirm power.
- **Can't reach `esp32cam.local`** → mDNS is flaky on some Android/Windows
  setups; use the static IP directly.
- **Stream stutters** → drop resolution to VGA in the UI, or move closer to
  the AP (check the RSSI printed at boot).

## Roadmap (deliberately out of v1)

- SD-card snapshot/recording (note: SD shares GPIO4 with the flash LED, so
  the two can't be used together without rework)
- OTA updates (`ArduinoOTA`, needs a partition change)
- HTTP Basic Auth / Tailscale for safe remote access
