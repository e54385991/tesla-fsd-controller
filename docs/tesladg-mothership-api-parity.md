# TeslaDG mothership / api-prd parity audit

Source of truth: the Windows `TeslaDG` package (audited from a local checkout
next to this repository; last re-audited 2026-11-28).

Compared against:

- `TeslaDG/app/dns_sink.py`
- `TeslaDG/app/svc/stack.py`
- `TeslaDG/app/start_hotspot.ps1`
- `TeslaDG/app/start_all.ps1`
- `TeslaDG/app/tools/arp_mitm.py`
- `TeslaDG/app/fsd_mock.py`
- `TeslaDG/app/web/fsd_panel.py`
- `TeslaDG/app/web/server.py`
- `TeslaDG/app/certs2/`
- `TeslaDG/app/requirements.txt` (mitmproxy 12.3.1)

ESP32 implementation:

- `include/mod_dns.h` (DNS sink + redirect matcher)
- `src/fsd_mock.cpp` (443/4567 catch-all, reverse proxy, FSD reply, error pages)
- `src/client_config.cpp` (443 TLS + 4567 plaintext listeners)
- `include/fsd_tls_identity.h` (eMudhra chain/key/leaf)
- `include/fsd_payload.h` (payload presets)
- `include/mod_wifi_bridge.h` (default external blocklist)

The vehicle-visible contract (DNS answers, `/feature_configuration` reply,
mitmproxy-style error pages, TLS chain) is intended to match TeslaDG. The
resource limits and the ESP32-only fast-fail paths are listed explicitly under
"Intentional ESP32 differences" so they are not mistaken for drift.

## Hijack set

| Item | PC | ESP32 |
|---|---|---|
| Hotspot DNS redirect | `api-prd.vn.cloud.tesla.cn` | `api-prd` + both mothership hosts (combined mode) |
| Stack DNS redirect | `mothership.vn.teslamotors.com` | same, plus api-prd |
| ARP MITM redirect | `mothership.vn.teslamotors.com`, `mothership.vn.cloud.tesla.cn`, `api-prd.vn.cloud.tesla.cn` | same |
| ARP MITM prefix | any `mothership.*` | any `mothership.*` via `dnsFsdRedirectMatchesHost()` |
| A answer | local IP, TTL 60 | AP IP, TTL 60 |
| AAAA/other | empty answer | empty answer |
| FSD redirect vs allow/block | redirect first | redirect first (`processNextRequest`) |

The ESP32 intentionally combines the PC hotspot and stack redirect sets so one
device handles both the 443 api-prd chain and the 4567 mothership chain.

PC DNS has no allow/block policy; every non-redirect query is forwarded to
`223.5.5.5`/`1.1.1.1`. The ESP32 adds the allowlist/blocklist and IP policy.
Those are separate features, not part of the FSD reply contract. Blocked A
queries return `0.0.0.0` on the ESP32; the PC has no equivalent. When the AP
upstream policy is blocked the ESP32 returns `REFUSED`; the PC would forward or
time out.

## Port 443 reverse target

| Item | PC (`start_hotspot.ps1` / `start_all.ps1`) | ESP32 |
|---|---|---|
| Upstream | `https://api-prd.vn.cloud.tesla.cn` | fixed `api-prd.vn.cloud.tesla.cn` |
| `keep_host_header` | not set (Host rewritten) | Host header not forwarded; URL host is api-prd |
| Upstream TLS | `ssl_insecure=true` | cert verification skipped (`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`) |
| Upstream method/URI | mitmproxy forwards | forwarded unchanged |
| Upstream request headers | all non-hop-by-hop headers | fixed allowlist of Tesla/browser headers (see below) |
| Upstream response headers | all headers | non-hop-by-hop headers, max 22 / 4 KB + `Content-Type` |

## Port 4567 reverse target

| Item | PC main chain (`svc/stack.py`) | ESP32 |
|---|---|---|
| Upstream | `http://192.0.2.1:4567` (TEST-NET, unreachable) | fixed blackhole `192.0.2.1:4567` |
| `keep_host_header` | true (only relevant if upstream existed) | car Host is logged, not forwarded |
| Transport | plain HTTP | plain HTTP |
| Non-FSD path | mitmproxy `502 Bad Gateway` HTML | identical `502 Bad Gateway` HTML |
| FSD path | local 200 reply | local 200 reply |

`TeslaDG/app/web/server.py` (the old 8899 dashboard) can start a 4567 reverse
to a resolved real mothership IP. That is not the main router/hotspot chain.
The ESP32 deliberately follows `svc/stack.py`: a non-FSD mothership request can
never reach Tesla through this listener, so the cloud cannot rewrite the
vehicle's FSD configuration. The 502 message is:

```text
[Errno 113] No route to host
```

mitmproxy's `<p>` contains only the socket error string (`err = str(OSError)`),
not a `Cannot connect to ...` prefix. The ESP32 follows that shape. The exact
errno text is OS-specific on the PC side (Python/Windows may print
`WinError ...`); the ESP32 keeps stable Linux-style messages so tooling can
assert the contract.

## feature_configuration contract

| Item | PC `fsd_mock.py` | ESP32 |
|---|---|---|
| Path match | `PATH_RE = /feature_configuration/?($|[?#])` anywhere in path | `isFeatureConfigurationUri()` matches anywhere + optional slash/query/fragment |
| Methods | all unless `FSD_MOCK_METHOD` set | all |
| Hosts | all unless `FSD_MOCK_HOSTS` set | all |
| reply mode | 200, compact JSON, `Content-Type: application/json`, `X-Checksum: md5(body)`, `Cache-Control: no-store` | same |
| observe mode | log OBSERVE, forward upstream | same |
| disabled | `FSD_MOCK=0` forwards | enabled toggle forwards |
| `HEAD` | headers only, no body | `Content-Length: <bytes>` + 0 body bytes |
| body over limit | mitmproxy accepts up to `body_size_limit` | 413 JSON above 64 KB |
| auto-disable | none; stays in reply mode | optional 2-minute auto-off, default **off** |

Once enabled, reply mode matches TeslaDG by default: the mock keeps answering
until the user disables it. The "下发成功后延迟 2 分钟自动关闭" checkbox remains
available but is not enabled on a fresh device. (The ESP32 still starts with the
whole FSD listener off; that is the RAM trade-off described below.)

The ESP32 default payload is 654 B (the Windows 20-field payload with
`disable_cabin_camera:false` + `enable_eu_fsd_behavior:false` +
`autoshift_dr:true`). TeslaDG's `payload.json` is 602 B. Use the "PC 602B"
preset when byte-identical PC output is required.

## Error feedback parity

For upstream failures, mitmproxy 12.3.1 builds the response in
`proxy/layers/http/_base.py::format_error` and
`proxy/layers/http/_http1.py::make_error_response`:

```http
HTTP/1.1 502 Bad Gateway
Server: mitmproxy 12.3.1
Connection: close
Content-Type: text/html
Content-Length: <n>

<html>
<head>
    <title>502 Bad Gateway</title>
</head>
<body>
    <h1>502 Bad Gateway</h1>
    <p>MESSAGE</p>
</body>
</html>
```

The ESP32 `sendProxyError()` / `formatProxyErrorBody()` produce exactly this
byte layout (including `html.escape()`-compatible escaping and the mitmproxy
reason phrases, e.g. `504 Gateway Time-out`), with these messages:

| Condition | Status | Message |
|---|---|---|
| 4567 non-FSD | 502 | `[Errno 113] No route to host` |
| 443 upstream not ready | 502 | `[Errno 101] Network is unreachable` |
| 443 repeated upstream failure | 502 | `[Errno 113] No route to host` |
| AP internet policy blocked | 503 | `ap_upstream_blocked` |
| Low heap guard | 503 | `low_heap` |
| Worker queue/async unavailable | 503 | `proxy_busy` / `async_unavailable` |
| Listener shutting down | 503 | `proxy_stopping` |
| Upstream `esp_http_client` error | 502 | `<esp_err_to_name>` |

The AP-blocked, low-heap, worker-queue and `esp_http_client` rows are
ESP32-only guards; TeslaDG has no exact equivalent. The JSON fast fails below
are also ESP32-only:

| Condition | Status | Body |
|---|---|---|
| `/feature_configuration` body > 64 KB | 413 | `{"error":"request_too_large"}` |
| request body read timeout/EOF | 400 | `{"error":"request_read_failed"}` |
| allocation failure | 500 | `{"error":"no_memory"}` |

On the PC, the text inside `<p>` comes from the OS socket exception and is not
stable across Windows/Linux; the ESP32 keeps a deterministic message for
automated parity checks. Header/status/HTML-shape parity is the contract;
free-form errno text is not.

TLS handshake failures are logged in `handshake.log` on both sides. The PC
records `handshake_failed` with a Python `TlsError` repr; the ESP32 records the
mbedTLS line (e.g. `mbedtls_ssl_handshake returned -0x7780`) and, on IDF 6+,
appends `handshake -0x7780 alert=<n>(<name>)` from `mbedtls_ssl_get_fatal_alert`.
Both UIs show the last error and the success/failure counts.

## HTTP parser, headers & body forwarding

| Item | PC (`mitmproxy 12.3.1`) | ESP32-S3 |
|---|---|---|
| Request header total limit | 16 KB (`h11`) | 8 KB (`CONFIG_HTTPD_MAX_REQ_HDR_LEN=8192` + `max_req_hdr_len`, dynamic per-request realloc/free, 0 idle RAM) |
| URI length limit | 16 KB shared | 1024 B (`CONFIG_HTTPD_MAX_URI_LEN=1024`) |
| Upstream TX header buffer | unbounded | 4 KB (`kUpstreamBufferTxBytes=4096`) |
| Upstream `User-Agent` | passes client header only | ESP-IDF default `ESP32 HTTP Client/1.0` stripped; forwards client header only |
| Forwarded request headers | all non-hop-by-hop | allowlist: `Host` dropped on 443; `Authorization`, `Content-Type`, `Accept*`, `Cookie`, `Origin`, `Referer`, cache/range/`If-*`, and the Tesla `X-Tesla-*` / `X-Request-Id` / `X-Correlation-Id` / `X-Requested-With` set |
| Upstream response headers | all headers forwarded | all non-hop-by-hop headers captured via `HTTP_EVENT_ON_HEADER` (up to 22 headers / 4 KB, `max_resp_headers=28`) + `Content-Type` |
| Request body forwarding | streamed | streamed in 512 B chunks up to 64 KB (`kMaxForwardBodyBytes=65536`) with no 4 KB heap copy |
| Upstream response body | streamed | streamed, capped at 256 KB / 15 s per request |
| `HEAD` / `204` / `304` framing | headers only, no body | `HEAD` on FSD reply sends `Content-Length: <bytes>` + 0 body bytes; upstream `HEAD`/`204`/`304`/empty sends clean `Content-Length: 0` |
| Incoming `Transfer-Encoding: chunked` | de-chunked | replies normally and sets `Connection: close` so unconsumed chunk bytes cannot desync keep-alive |
| Concurrency | multi-connection | 443 uses 4 TLS sockets with PSRAM (`backlog_conn=4`, 2 without PSRAM) so parallel api-prd/mothership handshakes do not queue into `-0x0050`; 4567 uses 3 sockets |
| HTTP version | HTTP/1.1 or HTTP/2 (ALPN) | HTTP/1.1 only (no ALPN advertisement) |
| Connection strategy | `lazy` (PC launcher) | connect on first request; no idle upstream connection |

## Certificates

Verified with hashes and OpenSSL against mitmproxy 12.3.1
`EMUDHRA_CHAIN_PEM` (the CA used by `--emudhra`; **not** the regular
`certs2/mitmproxy-ca.pem`, which is a separate self-signed mitmproxy CA):

| Material | Result |
|---|---|
| EM OV TLS CA - G3C | byte-identical (`cb6e1516…`) |
| emSign Root TLS CA - G3 | byte-identical (`30940257…`) |
| AAA Certificate Services cross-sign | byte-identical (`7485b51c…`) |
| EM OV TLS CA - G3C private key | byte-identical (`25265212…`) |
| Wire chain order | leaf → EM OV → emSign Root TLS → AAA cross-sign |
| Leaf public key | identical to CA public key (`dummy_cert()` behavior) |
| Leaf issuer | `EM OV TLS CA - G3C` |
| Leaf subject | `CN=api-prd.vn.cloud.tesla.cn, O=Tesla Inc.` |
| Leaf validity | 2026-11-07 22:54:45 UTC → 2036-11-04 22:54:45 UTC |
| Leaf EKU / AKI | serverAuth / CA SKI (`34:EA:EA:…:1B:6C`) |
| Leaf extensions | EKU + AKI + SAN only, like `dummy_cert()`; no BasicConstraints/SKI |
| Static leaf DER SHA-256 | `9519436b5bf62c562037d4b57cbbcd1ccbec43409a829609bb92f71adf7a1437` |
| Trust/export bundle | 3 certs: emSign ECC Root CA - G3, EM OV TLS CA - G3C, emSign Root TLS CA - G3 |

The leaf is necessarily different from PC's runtime leaf:

- PC mitmproxy generates a short-lived (~199 day, backdated 2 days) leaf per
  SNI at runtime. With `upstream_cert=true` it also copies the upstream
  certificate's CN/SAN/organization, so the PC api-prd leaf rotates with
  Tesla's real certificate.
- ESP32 serves one static leaf for both `api-prd` and the mothership hosts.
- Static leaf validity uses the `certs2` CA date window only so the device does
  not expire without a rebuild. The issuer is still `EM OV TLS CA - G3C`; the
  `certs2` self-signed CA is not in the wire chain.
- The static SAN set is a superset: api-prd upstream Tesla names + both
  mothership hosts + `fsd.local`, `9.9.9.9`, `127.0.0.1`.
- CN is `api-prd.vn.cloud.tesla.cn`; mothership is covered by SAN (RFC 2818
  clients use SAN when present). PC's per-SNI leaf would use the SNI as CN.
- The AAA cross-sign expires 2028-12-31. Clients that trust emSign (the normal
  case) build the longer-lived emSign path; clients that only trust AAA will
  need the shorter path. This is identical to the PC wire chain.

`kFsdTlsCaCertificate` is a trust bundle for the serial `client-config` export
and the local loopback self-test. It is not the wire chain and is not expected
to equal it byte-for-byte. Do not install `certs2/mitmproxy-ca-cert.*` for the
ESP32: that is the self-signed mitmproxy CA and is not in the `--emudhra` wire
chain. The serial export contains the three eMudhra trust certificates instead.

## Logging schema

`handshake.log` / `/api/fsd-mock/handshakes` use the TeslaDG fields:

```json
{"ts":"...","event":"handshake_ok","client":"...","sni":"...","alpn":"...",
 "tls":"...","cipher":"...","confdir":"certs2","error":"",
 "first_request":"GET /..."}
```

`confdir` is `certs2` on both sides, matching `fsd_mock.py` even when
`--emudhra` supplies the wire CA.

`hits.jsonl` / `/api/fsd-mock/hits` use the TeslaDG fields plus ESP32 response
details (`status`, `ctype`, `resp_head`). `HIT`/`OBSERVE`/`FLOW` kinds and the
`ts`, `method`, `url`, `client`, `http_version`, `host_header`, `user_agent`,
`vin`, `req_bytes`, `req_md5`, `req_ctype`, `req_head`, `mode`, `bytes`, `md5`
fields match.

Differences:

- PC logs `FLOW` for every request when `FSD_MOCK_LOG_ALL=1`, then `HIT` or
  `OBSERVE`. ESP32 logs `FLOW` only for passthrough; FSD hits are `HIT` /
  `OBSERVE` only.
- PC writes request bodies to `logs/requests/*.bin`; ESP32 keeps a 64-byte head
  and the MD5 only (RAM/flash lifetime).
- PC writes `handshake_ok` on the first request (so `first_request` is known);
  ESP32 writes it at TLS session creation and fills `first_request` when the
  first request arrives. The final JSON fields are the same.
- ESP32 fills `handshake_ok.sni` from the first request's `Host` header because
  `esp_https_server` does not expose the ClientHello SNI after the handshake;
  PC logs the real SNI. For Tesla traffic the two values are the same.
- PC logs `clienthello` and, with `FSD_MOCK_VERBOSE=1`, `near_miss`; ESP32 has no
  ClientHello hook with `esp_https_server` and does not implement near-miss.
- ESP32 does not advertise ALPN, so `handshake_ok.alpn` is empty and
  `http_version` is `HTTP/1.1`; PC may log `h2`/`http/1.1` depending on the client.
- PC keeps a 256-byte request head, full User-Agent and full pretty URL; ESP32
  caps `req_head` at 64 B, `user_agent` at 32 B and `url` at 160 B to keep the
  RAM ring small. The MD5 still covers the full request body.
- PC keeps 999 recent car requests; ESP32 keeps an 8-entry RAM ring (hits) and
  6-entry RAM ring (handshakes). The persisted config/timeline rings hold 5.

## Intentional ESP32 differences

These are hardware/safety limits, not accidental drift:

| Item | TeslaDG | ESP32 | Reason |
|---|---|---|---|
| FSD mock default | on when the launcher runs | off | ~28 KB internal RAM for Wi-Fi/NAPT |
| Auto-off after hit | none | optional, default off | UI convenience; parity default is off |
| Default payload | 602 B | 654 B | CN defaults; 602 B preset available |
| Payload size | 8192 B | 2048 B compact (4096 B input) | SPIFFS/RAM |
| 443 sockets | multi-connection | 1 + 1 backlog | TLS context RAM |
| Request body | unlimited/streamed | 64 KB cap | heap guard |
| Response body | unlimited/streamed | 256 KB / 15 s cap | heap guard |
| Request header size | 16 KB | 8 KB | httpd config |
| URI size | 16 KB | 1024 B | httpd config |
| Upstream response headers | all | 22 / 4 KB | heap guard |
| Request bodies on disk | `logs/requests/*.bin` | no | flash lifetime |
| Log retention | files / 999 rows | RAM rings | flash lifetime |
| DNS policy | redirect + forward | redirect + allow/block + IP policy | ESP32 feature |
| HTTP version | h1/h2 | h1 | httpd |
| Upstream error text | OS exception | deterministic ESP32 text | reproducibility |

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
reads `kTeslaMinAllowV9` / `kTeslaMinBlockV7` directly and re-enables DNS
filtering; the old preset buttons have been removed.

## Verification commands

```sh
python3 tests/run_dns_checks.py
python3 tests/run_fsd_payload_checks.py   # URI, payload MD5, cert chain/leaf, error pages
python3 tests/run_wifi_bridge_checks.py
~/.platformio/penv/bin/pio run -e esp32s3-waveshare-wifi
```
