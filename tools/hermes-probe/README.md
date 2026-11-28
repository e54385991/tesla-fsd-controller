# Hermes local stub / probe (lab only)

This directory contains a **standalone lab probe** for the vehicle side of
Tesla's Hermes signaling protocol. It is intentionally separate from the ESP32
firmware: the first goal is to answer two questions without flashing a new
production image.

1. Does the vehicle accept a locally presented certificate for a Hermes host
   (i.e. no pinning beyond the normal CA/hostname check, or no pinning at all)?
2. After a successful WebSocket + `STREAMING_CONFIG`, does the vehicle request
   `/feature_configuration`?

The probe **never opens an upstream connection**. It only listens. It does not
forward commands, and it does not send signed commands to the vehicle.

> **Lab only.** Use an owned vehicle and an isolated network. Do not use this
> against Tesla infrastructure. Do not use it to bypass a paid FSD entitlement.
> If the vehicle rejects the certificate, stop; do not attempt to bypass
> pinning. See the repository README for the full disclaimer.

---

## What it implements

| Stage | TLS | WebSocket 101 | `STREAMING_CONFIG` | ACK / ping |
|---|---|---|---|---|
| `tls` | yes | no | no | no |
| `ws` | yes | yes | no | no |
| `config` | yes | yes | one frame | no |
| `full` | yes | yes | one frame | yes |

The first three stages are diagnostic. `full` is the minimum "vehicle thinks it
is online" stub. Start with `tls`; only move forward if the handshake succeeds.

The WebSocket path is not hard-coded to `/v1/mobile`; the probe logs whatever
the vehicle sends. If the vehicle offers a `Sec-WebSocket-Protocol`, set
`--ws-subprotocol` to the value it expects; the probe logs the offered header
on the first run. The `STREAMING_CONFIG` topic defaults to `internal` and can
be overridden with `--config-topic`. The protobuf schema is in
`proto/hermes.proto`, based on the community research in
[timdorr/tesla-api#769](https://github.com/timdorr/tesla-api/discussions/769).
It is not official Tesla documentation.

---

## Quick start

### 1. Create a lab leaf certificate

The existing `include/fsd_tls_identity.h` contains the same eMudhra chain and
key material used by the ESP32 FSD endpoint. The helper below mints a
separate leaf with the SANs you specify:

```sh
python3 tools/hermes-probe/make_lab_cert.py \
  --san hermes-prd.vn.cloud.tesla.cn \
  --san hermes-stream-prd.vn.cloud.tesla.cn \
  --san signaling.vn.cloud.tesla.cn \
  --out tools/hermes-probe/certs
```

Output:

```text
tools/hermes-probe/certs/lab-leaf.pem
tools/hermes-probe/certs/lab-chain.pem
tools/hermes-probe/certs/lab-cert.pem   # leaf + intermediates
tools/hermes-probe/certs/lab-key.pem
tools/hermes-probe/certs/lab-trust.pem  # verification bundle
```

Generated files are git-ignored. Do not copy the private key to a vehicle.

### 2. Run a stage

```sh
# TLS only: trust / pinning probe
sudo python3 tools/hermes-probe/hermes_probe.py \
  --port 443 \
  --cert tools/hermes-probe/certs/lab-cert.pem \
  --key tools/hermes-probe/certs/lab-key.pem \
  --stage tls \
  --log hermes-tls.jsonl

# WebSocket + streaming config + ACK
sudo python3 tools/hermes-probe/hermes_probe.py \
  --port 443 \
  --cert tools/hermes-probe/certs/lab-cert.pem \
  --key tools/hermes-probe/certs/lab-key.pem \
  --stage full \
  --log hermes-full.jsonl
```

For a non-root test, use `--port 8443` and redirect the vehicle's 443 to 8443
on the gateway.

### 3. Point the vehicle at the probe

Use a lab AP/gateway you control. The vehicle must resolve the Hermes host to
the probe and must not have an alternate route to Tesla:

- Linux/Pi: `hostapd` + `dnsmasq` + firewall default DROP.
- Or keep the ESP32 as AP and add a **lab-only** DNS answer that maps the
  discovered Hermes hosts to the probe's LAN IP. Do not put this in a
  production allowlist.
- Capture `tcpdump` on the gateway. Verify the only 443 peer is the probe.

If the vehicle has LTE, it can bypass Wi-Fi filtering. For a true "no Tesla"
test, physically disable cellular (SIM/modem). A DNS allowlist alone is not
proof.

---

## What to look for

### `tls` stage

- `tls_ok` with the expected SNI, TLS version, cipher, and ALPN.
- `tls_error` with an alert such as `unknown_ca`, `bad_certificate`, or
  `certificate_unknown`.

The probe offers only `http/1.1` ALPN. If the vehicle offers only `h2`, the
`tls_ok` record will show `alpn: null` or the following HTTP parse will fail;
that is a useful finding, not a probe bug. In that case a companion with an
HTTP/2 stack is required. The probe also does not request a client certificate;
if the vehicle insists on mTLS, add that to the companion design after the
first capture.

Interpretation:

| Result | Meaning |
|---|---|
| `tls_ok` with the lab cert | The vehicle accepts the eMudhra chain and SAN; no obvious pinning |
| `unknown_ca` | The vehicle does not trust this chain for this host (CA scoping/revocation) |
| `bad_certificate` / `certificate_unknown` | Possible pinning or hostname/cert policy |
| No ClientHello | The vehicle is not trying; check DNS, LTE, sleep state, and cache |

A failure is a **stop signal**. Do not try to bypass pinning.

### `ws` / `config` / `full` stages

- `http_request`: path, Host, Upgrade, whether `X-Jwt` is present (the token is
  hashed, never logged in full).
- `ws_open`: the vehicle accepted the 101.
- `frame`: incoming binary frames, decoded field summary.
- `frame_sent`: the `STREAMING_CONFIG` and ACK frames.
- `ping_sent` / incoming pong.

Then watch the actual `/feature_configuration` endpoint (ESP32 `/fsd` board or
the local proxy). The key measurement is **time from `ws_open` to the first
configuration hit**, compared with the same run where Hermes is blocked.

If `config`/`full` does not trigger a configuration request, Hermes is not the
trigger. Do not keep expanding the stub blindly; go back to the baseline and
look at `mothership`, `connman`, or the vehicle's own timer.

---

## Tests

```sh
python3 -m unittest discover -s tools/hermes-probe/tests -v
python3 -m py_compile tools/hermes-probe/hermes_probe.py tools/hermes-probe/make_lab_cert.py
```

The tests do not open a network port and do not need a certificate.

---

## Integration later

The standalone probe is the gate. If it passes:

- **Recommended:** keep the ESP32 as AP/DNS/FSD mock and run this stub on a
  small always-on Linux companion (Pi Zero 2 W, old phone, mini PC). The ESP32
  then needs a per-host DNS answer for the Hermes names.
- **Not recommended initially:** implement TLS + WebSocket + protobuf on the
  ESP32-S3. The current 443 listener uses 4 TLS slots with PSRAM (2 without),
  enough for a long-lived WebSocket plus configuration requests, but the
  stress-test heap margin still needs per-board validation.
- **Fallback if pinning:** keep Hermes blocked and use a short, explicit
  one-shot window only when configuration must be refreshed. Do not run a
  permanent allowlist.
