# Tesla FSD Controller — ESP32 / ESP32-S3 Wi-Fi Bridge Edition

<p align="right">English | <a href="README.md">中文</a></p>

## Overview

This firmware turns an ESP32 or ESP32-S3 into a dedicated **Wi-Fi Bridge + DNS Filter** (plus an on-demand **FSD `/feature_configuration` mock server** on ESP32-S3):

1. **Wi-Fi Bridge (AP + STA + NAPT)**: Connects to a phone hotspot or router (STA) and creates a dedicated AP (`fsd-esp32`, `9.9.9.9/24`) for the vehicle.
2. **DNS Allowlist / Blocklist + LRU Cache**: Advertises `9.9.9.9` via DHCP DNS, enforces allowlist/blocklist policy with a 32-entry LRU cache, and optionally drops blocked IPs at the lwIP forwarding hook.
3. **On-Demand FSD Delivery (ESP32-S3)**: Disabled by default to maximize heap headroom for Wi-Fi/NAPT; toggle ON from the web UI (`/`) or `/fsd` panel to start the 443 HTTPS (TLS 1.2/1.3) and 4567 HTTP listeners without rebooting.

> Legacy CAN bus injection, TWAI drivers, vehicle telemetry dashboards, and CAN-only build environments have been completely removed from this repository to save Flash/RAM and dedicate Core 1 to lwIP + DNS.

---

## Firmware Builds

Each environment produces two files in the repository root:

- **`<env>_v1.4.35.bin`** — merged binary (bootloader + partitions + app) for first-time USB flashing at `0x0`.
- **`<env>_v1.4.35_ota.bin`** — app-only binary for Web OTA upload or GitHub Releases pull updates.

| Environment | Target | Merged Binary | Notes |
|---|---|---|---|
| `esp32-wifi` | Standard ESP32 | `esp32_wifi_v1.4.35.bin` | Pure Wi-Fi bridge + DNS filter, lean web UI (port `80`), no 443 FSD mock |
| **`esp32s3-waveshare-wifi`** | **Waveshare ESP32-S3** | **`esp32s3_waveshare_wifi_v1.4.35.bin`** | **Primary build: Wi-Fi bridge + DNS filter + on-demand FSD mock (TLS 1.3), management on port `10010`** |

---

## Quick Start

1. Flash `esp32s3_waveshare_wifi_v1.4.35.bin` at `0x0` via [ESP Web Flasher](https://esp.huhn.me/) or `esptool`.
2. Connect to Wi-Fi **`fsd-esp32`** (default password **`88888888`**).
3. Open **`http://9.9.9.9:10010/`** (or `http://fsd.local:10010/`; port `80` on `esp32-wifi`).
4. Add your phone hotspot / router under **Upstream Wi-Fi**, verify the status turns **Connected**, then connect the vehicle to `fsd-esp32`.

---

## Building from Source

```sh
# Primary ESP32-S3 Wi-Fi Bridge + FSD Mock
pio run -e esp32s3-waveshare-wifi

# Standard ESP32 Wi-Fi Bridge
pio run -e esp32-wifi

# Host regression tests
python3 tests/run_dns_checks.py
python3 tests/run_wifi_bridge_checks.py
python3 tests/run_fsd_payload_checks.py
```

For full documentation (default DNS allowlist/blocklist rules, FSD presets, serial recovery commands, and troubleshooting), see [README.md](README.md).

---

## License

GPLv3
