# TeslaDG mothership / api-prd parity audit

Compared against:

- `TeslaDG/app/dns_sink.py`
- `TeslaDG/app/svc/stack.py`
- `TeslaDG/app/start_hotspot.ps1`
- `TeslaDG/app/start_all.ps1`
- `TeslaDG/app/tools/arp_mitm.py`
- `TeslaDG/app/fsd_mock.py`
- `TeslaDG/app/web/server.py`
- `TeslaDG/app/certs2/`
- `TeslaDG/app/requirements.txt` (mitmproxy 12.3.1)

ESP32 implementation:

- `include/mod_dns.h` (DNS sink + redirect matcher)
- `src/fsd_mock.cpp` (443/4567 catch-all, reverse proxy, FSD reply)
- `src/client_config.cpp` (443 TLS + 4567 plaintext listeners)
- `include/fsd_tls_identity.h` (eMudhra chain/key/leaf)
- `include/mod_wifi_bridge.h` (default external blocklist)

## Hijack set

| Item | PC | ESP32 |
|---|---|---|
| Hotspot DNS redirect | `api-prd.vn.cloud.tesla.cn` | `api-prd` + both mothership hosts (combined mode) |
| Stack DNS redirect | `mothership.vn.teslamotors.com` | same, plus api-prd |
| ARP MITM redirect | `mothership.vn.teslamotors.com`, `mothership.vn.cloud.tesla.cn`, `api-prd.vn.cloud.tesla.cn` | same |
| ARP MITM prefix | any `mothership.*` | any `mothership.*` via `dnsFsdRedirectMatchesHost()` |
| A answer | local IP, TTL 60 | AP IP, TTL 60 |
| AAAA/other | empty answer | empty answer |

The ESP32 intentionally combines the PC hotspot and stack redirect sets so one
device handles both the 443 api-prd chain and the 4567 mothership chain.

## Port 443 reverse target

| Item | PC (`start_hotspot.ps1` / `start_all.ps1`) | ESP32 |
|---|---|---|
| Upstream | `https://api-prd.vn.cloud.tesla.cn` | fixed `api-prd.vn.cloud.tesla.cn` |
| `keep_host_header` | not set (Host rewritten) | Host header not forwarded; URL host is api-prd |
| Upstream TLS | `ssl_insecure=true` | cert verification skipped |

## Port 4567 reverse target

| Item | PC (`web/server.py` / `fsd_mock.py`) | ESP32 |
|---|---|---|
| Upstream | resolved `mothership.vn.teslamotors.com` A record, port 4567 | `http://mothership.vn.teslamotors.com:4567` |
| `keep_host_header` | true | car's Host header forwarded |
| Transport | plain HTTP | plain HTTP |

Note: `svc/stack.py` uses `reverse:http://192.0.2.1:4567` as a deliberate
blackhole for non-FSD paths. The ESP32 follows the `web/server.py` /
`fsd_mock.py` real-upstream behavior; if strict `stack.py` parity is required,
change `kMothershipUpstreamHost` handling to return 502 instead of forwarding.

## feature_configuration contract

| Item | PC `fsd_mock.py` | ESP32 |
|---|---|---|
| Path match | `PATH_RE = /feature_configuration/?($|[?#])` anywhere in path | `isFeatureConfigurationUri()` matches anywhere + optional slash/query/fragment |
| Methods | all unless `FSD_MOCK_METHOD` set | all |
| Hosts | all unless `FSD_MOCK_HOSTS` set | all |
| reply mode | 200, compact JSON, `Content-Type: application/json`, `X-Checksum: md5(body)`, `Cache-Control: no-store` | same |
| observe mode | log OBSERVE, forward upstream | same |
| disabled | `FSD_MOCK=0` forwards | enabled toggle forwards |

Logging differs in shape (PC logs FLOW for every request, then HIT/OBSERVE;
ESP32 logs HIT for FSD hits and FLOW for passthrough), but the vehicle-visible
response is the same.

## Certificates

Verified with hashes and OpenSSL against mitmproxy 12.3.1
`EMUDHRA_CHAIN_PEM` (the CA used by `--emudhra`; **not** the regular
`certs2/mitmproxy-ca.pem`):

| Material | Result |
|---|---|
| EM OV TLS CA - G3C | byte-identical (`cb6e1516…`) |
| emSign Root TLS CA - G3 | byte-identical (`30940257…`) |
| AAA Certificate Services cross-sign | byte-identical (`7485b51c…`) |
| EM OV TLS CA - G3C private key | byte-identical (`25265212…`) |
| Wire chain order | leaf → EM OV → emSign Root → AAA |
| Leaf public key | identical to CA public key (`dummy_cert()` behavior) |
| Leaf issuer | `EM OV TLS CA - G3C` |
| Leaf EKU / AKI | serverAuth / CA SKI |
| Leaf extensions | EKU + AKI + SAN only, like `dummy_cert()` |

The leaf is necessarily different from PC's runtime leaf:

- PC mitmproxy generates a short-lived (~197 day) leaf per SNI at runtime.
- ESP32 serves one static leaf for both `api-prd` and the mothership hosts.
- Static leaf validity is the `certs2` CA window (2026-11-07 → 2036-11-04)
  so the device does not expire without a rebuild.
- The static SAN set is a superset: api-prd Tesla names + both mothership
  hosts + `fsd.local`, `9.9.9.9`, `127.0.0.1`.
- CN is `api-prd.vn.cloud.tesla.cn`; mothership is covered by SAN (RFC 2818
  clients use SAN when present). PC's per-SNI leaf would use the SNI as CN.

`kFsdTlsCaCertificate` is a trust bundle for export (`emSign ECC Root CA - G3`
+ intermediates), not the wire chain. It is not expected to equal the wire
chain byte-for-byte.

## Default external blocklist

The first-boot blocklist includes:

```text
api-prd.vn.cloud.tesla.cn
mothership.vn.teslamotors.com
mothership.vn.cloud.tesla.cn
```

FSD hijack runs before the DNS allow/block decision, matching the PC chain.
With FSD disabled, the blacklist prevents direct external mothership/api-prd
access. Old v3/v4 default blocklists migrate automatically.

The allowlist and blocklist each have a one-click **Restore Default** action in
all three UIs. It calls `/api/wifi-bridge/set?dnsReset=allow|block|all`, which
reads `kTeslaMinAllowV5` / `kTeslaMinBlockV5` directly and re-enables DNS
filtering; the old preset buttons have been removed.

## Verification commands

```sh
python3 tests/run_dns_checks.py
python3 tests/run_fsd_payload_checks.py
python3 tests/run_legacy_checks.py
~/.platformio/penv/bin/pio run -e esp32s3-waveshare-wifi
```
