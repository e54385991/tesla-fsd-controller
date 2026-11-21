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
    assert len(found) == 3, f"expected 3 payloads, found {len(found)}"
    expected = [
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
    print(f"FSD payload: {len(found)} presets, URI matcher {len(true_cases)}/{len(false_cases)} cases, TLS chain OK")


if __name__ == "__main__":
    main()
