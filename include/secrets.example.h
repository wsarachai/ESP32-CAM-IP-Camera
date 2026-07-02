#pragma once
// -----------------------------------------------------------------------------
// TEMPLATE — copy this file to `secrets.h` (same folder) and fill in real values.
// `secrets.h` is git-ignored so your credentials never get committed.
//
//     cp include/secrets.example.h include/secrets.h
//
// -----------------------------------------------------------------------------

// --- WiFi ---
#define WIFI_SSID "YourNetworkName"
#define WIFI_PASS "YourNetworkPassword"

// --- Static IP ---
// Pick an address OUTSIDE your router's DHCP pool so nothing else grabs it.
// Example for a 192.168.1.0/24 network whose router is 192.168.1.1:
#define STATIC_IP 192, 168, 1, 50
#define GATEWAY_IP 192, 168, 1, 1
#define SUBNET_MASK 255, 255, 255, 0
#define PRIMARY_DNS 192, 168, 1, 1
#define SECONDARY_DNS 8, 8, 8, 8

// --- mDNS hostname --- reachable as http://<hostname>.local/ on supporting OSes.
#define MDNS_HOSTNAME "esp32cam"

// --- OTA --- password required to push firmware over WiFi.
// MUST match `upload_flags = --auth=...` for the esp32cam_ota env in platformio.ini.
#define OTA_PASSWORD "change-me-ota-pass"

// --- Periodic snapshot push --- POST a JPEG to PUSH_URL every PUSH_INTERVAL_MS.
// Set PUSH_ENABLED to 0 to disable. Coexists with the on-demand /capture endpoint.
#define PUSH_ENABLED     0
#define PUSH_URL         "http://192.168.1.20:3000/ingest"
#define PUSH_INTERVAL_MS 10000
