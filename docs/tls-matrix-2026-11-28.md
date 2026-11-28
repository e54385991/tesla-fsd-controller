# TLS version matrix and `-0x0050` fix — 2026-11-28

## Environment

- Hardware: Waveshare ESP32-S3 (8 MB PSRAM), AP `fsd-esp32-push` / `9.9.9.9`
- Toolchain: pioarduino 61.04.00 / Arduino 4.0.0 / ESP-IDF 6.1.1 / mbedTLS 4.0
- FSD 443: TLS 1.2 + TLS 1.3, ECDSA P-384 leaf certificate
- Firmware: `esp32s3-waveshare-wifi` v1.4.35 with this fix

## Symptom

Serial showed:

```
E esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x0050
E esp_https_server: esp_tls_create_server_session failed, 0x0050
```

`-0x0050` is `MBEDTLS_ERR_NET_CONN_RESET`: the peer reset the TCP connection
during the TLS handshake. It is not a compiler or cipher-configuration error.
The 443 listener used `max_open_sockets = 1`; when the car opened `api-prd` and
`mothership` in parallel, the second ClientHello waited for the first
keep-alive session to close. The car gave up and sent RST, producing `-0x0050`.

## Fix

- `src/client_config.cpp`: 443 uses `tlsSlots = PSRAM ? 4 : 2` for
  `max_open_sockets`/`backlog_conn` (PSRAM-backed mbedTLS sessions are
  ~20-30 KB each). No LRU purge: active car sockets must not be evicted.
- `sdkconfig.tls13.defaults` + both S3 sdkconfigs:
  `CONFIG_LWIP_MAX_SOCKETS=24` (four 443 slots + 4567 + DNS + async proxy).
- `fsd-selftest` expanded: TLS 1.2, TLS 1.3, default negotiation, SNI
  (`api-prd`, `mothership`, `fsd.local`), three repeats per version,
  keep-alive/reconnect, TLS 1.2 and 1.3 proxy flows, and two concurrent
  sockets (the car's two connections). The concurrent probes skip client-side
  certificate verification because P-384 verification on the same ESP32
  competes with the server and turned the test into a CPU/heap benchmark; the
  sequential probes still verify the full chain.

## Results (after fix)

Device self-test (serial `fsd-selftest`):

```
[fsd-selftest] done pass=19/19 heap=61095 minHeap=13160
[wifi-diag] fsdHs ok=18 fail=0 tls=TLSv1.3 err=-
```

External matrix (macOS on the device AP):

| Case | Result |
|---|---|
| TLS 1.0 / 1.1 | rejected: `tlsv1 alert protocol version` |
| TLS 1.2 forced, chain verify | 200, `ssl_verify_result=0` |
| TLS 1.3 default, chain verify | 200, `ssl_verify_result=0` |
| SNI api-prd / mothership / fsd.local | handshake + chain OK for all three |
| ALPN h2 offer | falls back to HTTP/1.1 (server does not advertise ALPN) |
| Keep-alive | two FSD HEADs on one TLS session |
| 2-way default ×20 | 20/20, appconnect ~0.55-1.2 s after warm-up |
| 2-way TLS 1.2 ×10 | 10/10, appconnect ~0.5-1.0 s |
| 4-way default ×12 | 12/12, appconnect ~0.5-2.4 s |

Before the fix, the same 4-way TLS 1.2 burst completed only 5/12 requests and
7 timed out at 8 s. After the fix, `handshake.fail` did not increase during any
external concurrency run; the only two failures in the final diagnostics are
the intentional TLS 1.0/1.1 rejection probes (`-0x6E80`, protocol version).

## Notes

- The ESP32-side self-test handshake still takes ~2.4-2.7 s because the client
  verifies the P-384 chain on the same CPU. This is not the car-facing server
  cost; external single/parallel handshakes are ~0.5-2.4 s.
- If a specific car still resets with more than four parallel connections,
  raise `tlsSlots`/`CONFIG_LWIP_MAX_SOCKETS` or reduce `keep_alive_idle`.
  Rolling back to IDF 5.5.5 is not required for this fix.
- Test commands:

```sh
python3 tests/run_tls_matrix.py --host 9.9.9.9 --rounds 10 --concurrency 4
uv run --with pyserial tests/run_device_tls_selftest.py --port /dev/cu.usbmodem21201
```

The host matrix script was also validated end-to-end against a local TLS
server (`15/15` checks), so it can be used from another phone/PC without the
macOS local-network permission issue seen in this test shell.
