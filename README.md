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
| Scope | Live viewer + snapshot + dimmable flash LED (no SD, no motion) |
| Camera default | SVGA 800×600, JPEG quality 12, `fb_count=2`, `GRAB_LATEST` |
| WiFi | Hardcoded in git-ignored `include/secrets.h` |
| Addressing | Static IP in firmware + mDNS (`esp32cam.local`) |
| Partition | `min_spiffs.csv` (dual ~1.9 MB app slots, OTA-capable) |
| Updates | USB (first flash) then wireless OTA (password-protected) |
| Security | LAN-only, no auth — **never port-forward this** |
| Power | Dev via MB micro-USB; deploy on a dedicated **5V 2A** supply |

## First-time setup

1. Copy the secrets template and fill in your network:
   ```
   cp include/secrets.example.h include/secrets.h
   ```
   Edit `include/secrets.h` — set `WIFI_SSID`, `WIFI_PASS`, a `STATIC_IP`
   **outside your router's DHCP pool**, and an `OTA_PASSWORD`.

2. First flash over USB (ESP32-CAM-MB plugged in):
   ```
   pio run -e esp32cam --target upload
   pio device monitor
   ```
   The MB adapter handles boot mode + auto-reset — no GPIO0 jumper needed.

3. Watch the serial monitor (115200) for the sensor type and the line:
   ```
   [http] ready:  http://172.16.1.11/    stream on :81
   ```
   Open that URL (or `http://esp32cam.local/`) in a browser.

## Updating over the air (no cable)

Once the OTA-capable firmware is on the board (step 2 above), push future
updates wirelessly from any machine on the same WiFi:

```
pio run -e esp32cam_ota --target upload
```

- Target IP and `--auth` password live in `platformio.ini` under `[env:esp32cam_ota]`.
- The `--auth` value **must match** `OTA_PASSWORD` in `include/secrets.h`.
- Changing the OTA password requires one USB reflash (the board authenticates
  the incoming update against the password it's *currently* running).
- A partition-table change (e.g. going back to non-OTA) also needs USB.

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
- HTTP Basic Auth / Tailscale for safe remote access
