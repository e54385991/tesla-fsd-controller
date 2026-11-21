#!/usr/bin/env python3
"""Verify the ESP32 FSD payload constants and URI matcher against TeslaDG.

Run: python3 tests/run_fsd_payload_checks.py
"""
from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/fsd_payload.h").read_text()
PATTERN = re.compile(r'R"JSON\((.*?)\)JSON"', re.S)
TLS_HEADER = (ROOT / "include/fsd_tls_identity.h").read_text()
PEM_PATTERN = re.compile(r'R"PEM\((.*?)\)PEM"', re.S)


def check_tls_identity() -> None:
    blocks = PEM_PATTERN.findall(TLS_HEADER)
    assert len(blocks) == 3, f"expected 3 PEM blocks, found {len(blocks)}"
    chain, key, ca = blocks
    assert chain.count("BEGIN CERTIFICATE") == 4, "chain must contain leaf + eMudhra intermediates"
    assert "BEGIN RSA PRIVATE KEY" in key or "BEGIN PRIVATE KEY" in key
    assert "BEGIN CERTIFICATE" in ca and "END CERTIFICATE" in ca
    assert "api-prd.vn.cloud.tesla.cn" in TLS_HEADER
    assert "9.9.9.9" in TLS_HEADER

    # The three non-leaf certs must stay byte-identical to mitmproxy 12.3.1
    # EMUDHRA_CHAIN_PEM (certs.py), in the same order.
    certs = re.findall(
        r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", chain, re.S
    )
    assert len(certs) == 4
    import base64
    import hashlib
    fingerprints = [
        hashlib.sha256(base64.b64decode("".join(cert.split()))).hexdigest()
        for cert in certs[1:]
    ]
    assert fingerprints == [
        "cb6e15165ac802e9b3f993f16c25d41f4e81c21ebde71ae1b012ac00bf9c7c3f",
        "3094025792039a68af6e553686410bdab7c0d4a36b9084cba64202e35d6ebbdd",
        "7485b51c0ed192878fa1d2f948fb529ae1842cc5086c4b67f7be5b2039de9489",
    ], "eMudhra chain no longer matches mitmproxy --emudhra"

    openssl = shutil.which("openssl")
    if not openssl:
        return
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        (base / "chain.pem").write_text(chain)
        (base / "ca.pem").write_text(ca)
        leaf = chain.split("-----END CERTIFICATE-----")[0] + "-----END CERTIFICATE-----\n"
        (base / "leaf.pem").write_text(leaf)
        verified = subprocess.run(
            [openssl, "verify", "-CAfile", str(base / "ca.pem"), str(base / "leaf.pem")],
            capture_output=True, text=True,
        )
        assert verified.returncode == 0, verified.stderr
        san = subprocess.run(
            [openssl, "x509", "-in", str(base / "leaf.pem"), "-noout", "-ext", "subjectAltName"],
            capture_output=True, text=True, check=True,
        ).stdout
        assert "api-prd.vn.cloud.tesla.cn" in san
        assert "9.9.9.9" in san
        subject = subprocess.run(
            [openssl, "x509", "-in", str(base / "leaf.pem"), "-noout", "-subject"],
            capture_output=True, text=True, check=True,
        ).stdout
        assert "CN=api-prd.vn.cloud.tesla.cn" in subject
        assert "O=Tesla Inc." in subject


def check_response_contract() -> None:
    """TeslaDG/app/fsd_mock.py reply contract + RAM-only logging."""
    source = (ROOT / "src/fsd_mock.cpp").read_text()
    for needle in (
        'httpd_resp_set_type(request, "application/json")',
        'httpd_resp_set_hdr(request, "X-Checksum", checksum)',
        'httpd_resp_set_hdr(request, "Cache-Control", "no-store")',
        'setStatusLine(request, 200)',
        'const bool observe = mode() == Mode::Observe',
        'strlcpy(hit.kind, observe ? "OBSERVE" : "HIT"',
        'gHits[gHitWrite] = hit',
        'Handshake& handshake = gHandshakes[gHandshakeWrite]',
        'esp_http_client_open(client, bodyLength)',
        'upstream_unavailable',
        'constexpr size_t kForwardBodyBytes = 4096;',
        'esp_http_client_fetch_headers(client)',
        'httpd_resp_send_chunk(request, buffer, read)',
    ):
        assert needle in source, f"missing reply contract: {needle}"
    # Only removal of the old persisted logs is allowed; no append handles.
    assert 'open("/fsd_hits.jsonl"' not in source
    assert 'open("/fsd_handshake.log"' not in source
    assert source.count('SPIFFS.remove("/fsd_hits.jsonl")') == 1
    assert source.count('SPIFFS.remove("/fsd_handshake.log")') == 1
    # perform() consumes and discards the response body; must use streaming.
    assert 'esp_http_client_perform(client)' not in source


def check_port_policy() -> None:
    """443 is FSD-only; management + client-config share port 10010."""
    main = (ROOT / "src/main.cpp").read_text()
    mod = (ROOT / "include/mod_client_config.h").read_text()
    server = (ROOT / "src/client_config.cpp").read_text()
    assert "kManagementPort = 10010" in main
    assert "constexpr uint16_t kFsdPort = 443;" in mod
    assert "kClientConfigPort" not in mod
    assert "kClientConfigPort" not in server
    assert "config.port_secure = kFsdPort;" in server
    # Client-config must be registered on the AsyncWebServer (10010), not 443.
    assert '"/client-config"' in main
    assert '"/api/client-config"' in main
    assert "client_config::apiGet" in main
    assert "client_config::apiChange" in main
    assert "max_open_sockets = 1" in server


def check_tls_policy() -> None:
    """Waveshare-S3 Wi-Fi runs mbedTLS 3.6 (TLS 1.2 + 1.3); IDF 4.4 envs are 1.2-only."""
    tls12_only = [
        ROOT / "sdkconfig.defaults",
        ROOT / "sdkconfig.esp32-wifi",
        ROOT / "sdkconfig.esp32-wifi-debug",
    ]
    tls13 = [
        ROOT / "sdkconfig.tls13.defaults",
        ROOT / "sdkconfig.esp32s3-waveshare-wifi",
        ROOT / "sdkconfig.esp32s3-waveshare-wifi-debug",
    ]
    for path in tls12_only + tls13:
        text = path.read_text()
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y" in text, f"{path.name}: TLS 1.2 must stay enabled"
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1=y" not in text, f"{path.name}: TLS 1.0 must stay disabled"
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_1=y" not in text, f"{path.name}: TLS 1.1 must stay disabled"
    for path in tls12_only:
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y" not in path.read_text(), (
            f"{path.name}: IDF 4.4 has no TLS 1.3 option"
        )
    for path in tls13:
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y" in path.read_text(), (
            f"{path.name}: TLS 1.3 must stay enabled"
        )

    platformio = (ROOT / "platformio.ini").read_text()
    assert platformio.count("platform = platformio/espressif32@8.0.0") >= 2
    assert 'board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.tls13.defaults"' in platformio

    server = (ROOT / "src/client_config.cpp").read_text()
    mock = (ROOT / "src/fsd_mock.cpp").read_text()
    assert "config.user_cb = tlsUserCallback;" in server
    assert "mbedtls_ssl_get_version" in mock


def payloads() -> list[str]:
    return PATTERN.findall(HEADER)


def matches(uri: str) -> bool:
    """Mirror fsd_mock::isFeatureConfigurationUri (TeslaDG PATH_RE)."""
    marker = "/feature_configuration"
    start = uri.find(marker)
    if start < 0:
        return False
    end = start + len(marker)
    if uri[end:] == "" or uri[end] in "?#":
        return True
    if uri[end] == "/":
        end += 1
        return uri[end:] == "" or uri[end] in "?#"
    return False


def main() -> None:
    found = payloads()
    assert len(found) == 4, f"expected 4 payloads, found {len(found)}"
    expected = [
        (653, "d216ba9d001cfff321dfb6e596f61d64", 22),
        (602, "241cb01b81a2b9c8b9f389a471bcbc94", 20),
        (476, "b61035d9dea38e737fbf8195ccb39f2c", 16),
        (282, "c212a75ca858750ac302093c138991f2", 9),
    ]
    for text, (size, digest, fields) in zip(found, expected):
        raw = text.encode("utf-8")
        assert len(raw) == size, (len(raw), size)
        assert hashlib.md5(raw).hexdigest() == digest
        assert len(json.loads(raw)) == fields
        assert b"\n" not in raw and b": " not in raw
        assert f'"{digest}"' in HEADER

    default_json = json.loads(found[0])
    assert default_json.get("enable_eu_fsd_behavior") is False
    assert default_json.get("autoshift_dr") is True
    assert list(default_json)[-1] == "autoshift_dr"

    true_cases = [
        "/feature_configuration",
        "/feature_configuration/",
        "/feature_configuration?vin=1",
        "/feature_configuration/#x",
        "/vehicles/5YJ3E1EA7KF000000/feature_configuration",
        "/a/b/feature_configuration?x=1",
    ]
    false_cases = [
        "/feature_configurations",
        "/feature_configuration/extra",
        "/xfeature_configuration",
        "/vehicles/5YJ3E1EA7KF000000/feature_configurationx",
        "/",
    ]
    for uri in true_cases:
        assert matches(uri), uri
    for uri in false_cases:
        assert not matches(uri), uri
    check_tls_identity()
    check_tls_policy()
    check_response_contract()
    check_port_policy()
    print(f"FSD payload: {len(found)} presets, URI matcher {len(true_cases)}/{len(false_cases)} cases, "
          f"TLS chain + reply contract + 443/10010 policy OK")


if __name__ == "__main__":
    main()
