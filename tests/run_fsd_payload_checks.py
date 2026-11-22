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

    # The private key is the compromised EM OV TLS CA - G3C key from
    # mitmproxy 12.3.1 EMUDHRA_CHAIN_PEM, byte-for-byte.
    key_der = base64.b64decode("".join(
        key.split("-----BEGIN PRIVATE KEY-----")[1].split("-----END PRIVATE KEY-----")[0].split()
    ))
    assert hashlib.sha256(key_der).hexdigest() == (
        "25265212a4872a42372fb7ba3401f8eec91e0486d64e34cd2f68be5b9f15ef9d"
    ), "FSD private key no longer matches mitmproxy EMUDHRA_CHAIN_PEM"

    openssl = shutil.which("openssl")
    if not openssl:
        return
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        (base / "chain.pem").write_text(chain)
        (base / "ca.pem").write_text(ca)
        (base / "key.pem").write_text(key)
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
        assert "mothership.vn.teslamotors.com" in san
        assert "mothership.vn.cloud.tesla.cn" in san
        assert "9.9.9.9" in san
        subject = subprocess.run(
            [openssl, "x509", "-in", str(base / "leaf.pem"), "-noout", "-subject"],
            capture_output=True, text=True, check=True,
        ).stdout
        assert "CN=api-prd.vn.cloud.tesla.cn" in subject
        assert "O=Tesla Inc." in subject
        # mitmproxy dummy_cert() uses the CA public key for the leaf, and the
        # CA private key to sign it. The ESP32 leaf must do the same.
        leaf_pub = subprocess.run(
            [openssl, "x509", "-in", str(base / "leaf.pem"), "-noout", "-pubkey"],
            capture_output=True, text=True, check=True,
        ).stdout
        key_pub = subprocess.run(
            [openssl, "pkey", "-in", str(base / "key.pem"), "-pubout"],
            capture_output=True, text=True, check=True,
        ).stdout
        assert leaf_pub.strip() == key_pub.strip(), "leaf public key must equal the eMudhra CA key"


def check_response_contract() -> None:
    """TeslaDG/app/fsd_mock.py reply contract + RAM-only logging + HTTP parity."""
    source = (ROOT / "src/fsd_mock.cpp").read_text()
    mock_header = (ROOT / "include/mod_fsd_mock.h").read_text()
    for needle in (
        'httpd_resp_set_type(request, "application/json")',
        'httpd_resp_set_hdr(request, "X-Checksum", checksum)',
        'httpd_resp_set_hdr(request, "Cache-Control", "no-store")',
        'formatStatusLine(200, statusLine)',
        'const bool observe = mode() == Mode::Observe',
        'strlcpy(hit.kind, observe ? "OBSERVE" : "HIT"',
        'gHits[gHitWrite] = hit',
        'Handshake& handshake = gHandshakes[gHandshakeWrite]',
        'esp_http_client_open(client, static_cast<int>(bodyLength))',
        'esp_http_client_delete_header(client, "User-Agent")',
        'upstream_unavailable',
        'constexpr size_t kMaxForwardBodyBytes = 65536;',
        'constexpr size_t kUpstreamBufferTxBytes = 4096;',
        'HTTP_EVENT_ON_HEADER',
        'esp_http_client_fetch_headers(client)',
        'httpd_resp_send_chunk(request, buffer, read)',
        'ESP_ERR_HTTPD_RESULT_TRUNC',
        'isChunkedRequest(request)',
    ):
        assert needle in source, f"missing reply contract: {needle}"
    # esp_http_client_get_header() reads outgoing request headers, not response headers.
    code_lines = "\n".join(line for line in source.splitlines() if not line.lstrip().startswith("//"))
    assert 'esp_http_client_get_header(' not in code_lines, (
        "must capture upstream response headers via HTTP_EVENT_ON_HEADER, not esp_http_client_get_header()"
    )
    # Last-response preview must hold the full 654 B default payload without truncation.
    m = re.search(r"kLastResponseCapacity\s*=\s*(\d+)", mock_header)
    assert m and int(m.group(1)) >= 655, "kLastResponseCapacity too small for 654 B default payload"
    # Only removal of the old persisted logs is allowed; no append handles.
    assert 'open("/fsd_hits.jsonl"' not in source
    assert 'open("/fsd_handshake.log"' not in source
    assert source.count('SPIFFS.remove("/fsd_hits.jsonl")') == 1
    assert source.count('SPIFFS.remove("/fsd_handshake.log")') == 1
    # perform() consumes and discards the response body; must use streaming.
    assert 'esp_http_client_perform(client)' not in source


def _extract_block(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if end < len(source) and source[end] == ";":
        end += 1
    return source[start:end]


def check_upstream_header_capture() -> None:
    """Compile and run UpstreamResponseHeaders + formatStatusLine under ASan/UBSan."""
    cxx = shutil.which("c++")
    if not cxx:
        return
    source = (ROOT / "src/fsd_mock.cpp").read_text()
    extracted = "\n".join((
        _extract_block(source, "const char* formatStatusLine("),
        _extract_block(source, "struct UpstreamResponseHeaders {"),
    ))
    harness = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
constexpr size_t kMaxForwardedRespHeaders = 14;
constexpr size_t kMaxRespHeaderTotalBytes = 2048;
''' + extracted + r'''
int main() {
    char buf[32];
    assert(std::strcmp(formatStatusLine(200, buf), "200 OK") == 0);
    assert(std::strcmp(formatStatusLine(204, buf), "204 No Content") == 0);
    assert(std::strcmp(formatStatusLine(431, buf), "431") == 0);

    UpstreamResponseHeaders h;
    h.add("Content-Type", "application/json; charset=utf-8");
    h.add("X-Checksum", "bd262907ff9fb2709bb54f9d3e78d07d");
    h.add("Content-Encoding", "gzip");
    h.add("Set-Cookie", "a=1; Path=/");
    h.add("Set-Cookie", "b=2; Path=/");
    // Hop-by-hop & framing headers must be skipped.
    h.add("Transfer-Encoding", "chunked");
    h.add("Content-Length", "123");
    h.add("Connection", "keep-alive");
    h.add("Keep-Alive", "timeout=5");
    // CRLF header injection must be rejected.
    h.add("X-Evil", "ok\r\nInjected: 1");
    h.add("Bad\nName", "val");
    assert(h.contentType && std::strcmp(h.contentType, "application/json; charset=utf-8") == 0);
    assert(h.count == 4);
    assert(std::strcmp(h.entries[0].key, "X-Checksum") == 0);
    assert(std::strcmp(h.entries[1].key, "Content-Encoding") == 0);
    assert(std::strcmp(h.entries[2].key, "Set-Cookie") == 0);
    assert(std::strcmp(h.entries[3].value, "b=2; Path=/") == 0);

    // Count cap must hold cleanly without leaks.
    for (int i = 0; i < 30; ++i) {
        char k[16];
        std::snprintf(k, sizeof(k), "X-H-%d", i);
        h.add(k, "value");
    }
    assert(h.count == kMaxForwardedRespHeaders);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="fsd-hdr-") as tmp:
        cpp = Path(tmp) / "hdr.cpp"
        exe = Path(tmp) / "hdr"
        cpp.write_text(harness)
        subprocess.run(
            [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             str(cpp), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


def check_payload_compact_and_sum_parity() -> None:
    """Compile production compactJson/dedupeJsonObjects against ESP-IDF cJSON
    under ASan+UBSan and verify byte-for-byte & MD5 X-Checksum parity with
    PC TeslaDG (_compact + hashlib.md5) across add/delete/reorder/UTF-8/dupes."""
    cxx = shutil.which("c++")
    cc = shutil.which("cc")
    cjson_dir = Path.home() / ".platformio/packages/framework-espidf/components/json/cJSON"
    if not (cxx and cc and (cjson_dir / "cJSON.c").is_file()):
        return

    source = (ROOT / "src/fsd_mock.cpp").read_text()
    extracted = "\n".join((
        _extract_block(source, "size_t countFields("),
        _extract_block(source, "void dedupeJsonObjects("),
        _extract_block(source, "bool compactJson("),
    ))
    harness = r'''
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cJSON.h"
constexpr size_t kPayloadCapacity = 2049;
constexpr size_t kPayloadInputCapacity = 4096;
''' + extracted + r'''
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    assert(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    char* in = static_cast<char*>(std::malloc(static_cast<size_t>(sz) + 1));
    assert(in != nullptr);
    size_t n = std::fread(in, 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    in[n] = '\0';

    char out[kPayloadCapacity] = {};
    char err[64] = {};
    size_t outLen = 0, fields = 0;
    bool ok = compactJson(in, n, out, sizeof(out), outLen, fields, err, sizeof(err));
    std::free(in);
    if (!ok) {
        std::fprintf(stderr, "ERR:%s\n", err);
        return 1;
    }
    std::printf("%zu\n", fields);
    std::fwrite(out, 1, outLen, stdout);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="fsd-sum-") as tmp:
        base = Path(tmp)
        cjson_obj = base / "cJSON.o"
        cpp = base / "compact.cpp"
        exe = base / "compact"
        subprocess.run(
            [cc, "-O2", "-I", str(cjson_dir), "-c", str(cjson_dir / "cJSON.c"), "-o", str(cjson_obj)],
            check=True,
        )
        cpp.write_text(harness)
        subprocess.run(
            [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(cjson_dir), str(cpp), str(cjson_obj), "-o", str(exe)],
            check=True,
        )

        def run_case(raw_bytes: bytes) -> tuple[bool, int, bytes, str]:
            inp = base / "in.json"
            inp.write_bytes(raw_bytes)
            r = subprocess.run([str(exe), str(inp)], capture_output=True)
            if r.returncode != 0:
                return False, 0, b"", r.stderr.decode("utf-8", "replace").strip()
            nl = r.stdout.index(b"\n")
            return True, int(r.stdout[:nl]), r.stdout[nl + 1:], ""

        def pc_compact(obj: dict) -> bytes:
            # Exact TeslaDG/app/fsd_mock.py _compact()
            return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")

        base_obj = json.loads(payloads()[0])
        # 1) Adding fields (bool, string, null, int, UTF-8)
        added = dict(base_obj)
        added["custom_bool"] = True
        added["custom_str"] = "CHN-测试"
        added["custom_null"] = None
        added["custom_num"] = 42
        # 2) Deleting fields including enable_eu_fsd_behavior & autoshift_dr
        deleted = {k: v for k, v in base_obj.items()
                   if k not in ("enable_eu_fsd_behavior", "autoshift_dr", "tcpdump_enabled")}
        # 3) Deleting down to single field and empty object
        single = {"full_self_driving": True}
        empty_obj: dict = {}
        # 4) Key reordering (must preserve order and match PC MD5)
        reordered = {"autoshift_dr": True, "full_self_driving": True, "eu_fsd_allowed_country_codes": "CHN"}

        for label, obj in (
            ("default", base_obj),
            ("added", added),
            ("deleted", deleted),
            ("single", single),
            ("empty", empty_obj),
            ("reordered", reordered),
        ):
            pretty = json.dumps(obj, indent=2, ensure_ascii=False).encode("utf-8")
            ok, fields, out_bytes, err = run_case(pretty)
            expected_bytes = pc_compact(obj)
            assert ok, f"{label}: compactJson failed: {err}"
            assert fields == len(obj), f"{label}: fields={fields} != {len(obj)}"
            assert out_bytes == expected_bytes, f"{label}: bytes mismatch:\n  esp={out_bytes!r}\n  pc ={expected_bytes!r}"
            assert hashlib.md5(out_bytes).hexdigest() == hashlib.md5(expected_bytes).hexdigest(), (
                f"{label}: MD5 X-Checksum mismatch"
            )

        # 5) Duplicate keys must deduplicate identically to Python json.loads (keep first position, last value)
        dup_raw = b'{"a":true,"b":false,"a":false,"c":"x","a":"final"}'
        ok, fields, out_bytes, err = run_case(dup_raw)
        pc_dup = pc_compact(json.loads(dup_raw.decode("utf-8")))
        assert ok and fields == 3 and out_bytes == pc_dup, (ok, fields, out_bytes, pc_dup, err)
        assert hashlib.md5(out_bytes).hexdigest() == hashlib.md5(pc_dup).hexdigest()

        # 6) Pretty-printed JSON > 2048 B raw that compacts to <= 2048 B must succeed
        big_obj = {f"field_{i:02d}": (i % 2 == 0) for i in range(95)}
        big_pretty = json.dumps(big_obj, indent=4).encode("utf-8")
        assert len(big_pretty) > 2048 and len(pc_compact(big_obj)) < 2048
        ok, fields, out_bytes, err = run_case(big_pretty)
        assert ok and fields == 95 and out_bytes == pc_compact(big_obj), (ok, fields, err)

        # 7) Invalid payloads must be rejected
        for bad in (b"", b"[1,2]", b'{"a":true}junk', b'{"a":\x00}', b"not json"):
            ok, _, _, _ = run_case(bad)
            assert not ok, f"expected failure for {bad!r}"

    # UI editor contract: no forced re-injection or reordering; case-insensitive coerce
    ui = (ROOT / "include/web_ui_fsd.h").read_text()
    assert "if(!hasEu)" not in ui, "web_ui_fsd must not re-inject deleted enable_eu_fsd_behavior"
    assert "delete obj.autoshift_dr" not in ui, "web_ui_fsd must not silently reorder autoshift_dr"
    assert "toLowerCase()" in ui, "web_ui_fsd coerce() must be case-insensitive like PC fsd_panel.py"
    assert "默认 654B" in ui, "web_ui_fsd default preset button must show 654B"
    assert "默认 653B" not in ui, "stale 653B label found in web_ui_fsd.h"


def check_port_policy() -> None:
    """443 is FSD-only; 4567 mirrors the PC mothership HTTP listener."""
    main = (ROOT / "src/main.cpp").read_text()
    mod = (ROOT / "include/mod_client_config.h").read_text()
    mock_header = (ROOT / "include/mod_fsd_mock.h").read_text()
    bridge = (ROOT / "include/mod_wifi_bridge.h").read_text()
    server = (ROOT / "src/client_config.cpp").read_text()
    mock = (ROOT / "src/fsd_mock.cpp").read_text()
    assert "kManagementPort = 10010" in main
    assert "constexpr uint16_t kFsdPort = 443;" in mod
    assert "constexpr uint16_t kMothershipHttpPort = 4567;" in mock_header
    assert "kClientConfigPort" not in mod
    assert "kClientConfigPort" not in server
    assert "config.port_secure = kFsdPort;" in server
    # Client-config must be registered on the AsyncWebServer (10010), not 443.
    assert '"/client-config"' in main
    assert '"/api/client-config"' in main
    assert "client_config::apiGet" in main
    assert "client_config::apiChange" in main
    assert "max_open_sockets = 1" in server
    # PC parity: plaintext mothership listener + keep-host upstream routing.
    assert "config.server_port = fsd_mock::kMothershipHttpPort;" in server
    assert "startMothershipHttp()" in server
    assert "mothershipHttpStarted()" in server
    assert "const char* upstreamHost = secure ? kApiPrdHost : kMothershipUpstreamHost;" in mock
    assert 'if (secure && strcasecmp(name, "Host") == 0) continue;' in mock
    assert "mothership.vn.teslamotors.com" in mock
    assert "mothership.vn.cloud.tesla.cn" in mock
    # Default external blocklist carries both mothership hosts and migrates
    # the v4 api-prd-only default.
    assert "mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn" in bridge
    assert "kTeslaMinBlockV5" in bridge
    for path in ("include/web_ui_manage.h",):
        text = (ROOT / path).read_text()
        assert "mothership.vn.teslamotors.com" in text, f"{path}: missing mothership blocklist entry"
        assert "mothership.vn.cloud.tesla.cn" in text, f"{path}: missing mothership blocklist entry"
        assert "dnsReset" in text, f"{path}: missing one-click DNS default reset"
    # Default reset is served from the firmware constants, not duplicated JS.
    assert "dnsReset" in main
    assert "kTeslaMinAllowV5" in main
    assert "kTeslaMinBlockV5" in main
    # v5 default allowlist: consolidated Apple Music + Doubao domains.
    for domain in ("*.apple.com", "mzstatic.com", "baidu.com", "bdimg.com", "bcebos.com",
                   "qq.com", "tencentmusic.com", "163.com", "126.net", "netease.com",
                   "doubao.com", "doubaoapps.com", "doubaocdn.com", "butterflyai.cn",
                   "byteimg.com", "bytescm.com", "bytednsdoc.com", "snssdk.com"):
        assert domain in bridge, f"default allowlist missing {domain}"


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
        text = path.read_text()
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y" in text, (
            f"{path.name}: TLS 1.3 must stay enabled"
        )
        assert "CONFIG_HTTPD_MAX_REQ_HDR_LEN=8192" in text, (
            f"{path.name}: httpd header limit must stay >= 8192 to avoid HTTP 431 on JWT/browser headers"
        )
        assert "CONFIG_HTTPD_MAX_URI_LEN=1024" in text, (
            f"{path.name}: httpd URI limit must stay >= 1024"
        )

    platformio = (ROOT / "platformio.ini").read_text()
    assert platformio.count("platform = platformio/espressif32@8.0.0") >= 2
    assert 'board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.tls13.defaults"' in platformio

    server = (ROOT / "src/client_config.cpp").read_text()
    mock = (ROOT / "src/fsd_mock.cpp").read_text()
    assert "config.user_cb = tlsUserCallback;" in server
    assert "kFsdMaxReqHdrLen = 8192" in server
    assert "config.max_req_hdr_len = kFsdMaxReqHdrLen;" in server
    assert "config.httpd.max_req_hdr_len = kFsdMaxReqHdrLen;" in server
    assert "config.max_resp_headers = kFsdMaxRespHeaders;" in server
    assert "config.httpd.max_resp_headers = kFsdMaxRespHeaders;" in server
    # Anti-hang & self-healing contracts (Part 1 + Part 2)
    assert "config.keep_alive_enable = true;" in server
    assert "config.httpd.keep_alive_enable = true;" in server
    assert "config.httpd.lru_purge_enable = true;" in server
    assert "config.tls_handshake_timeout_ms = 4000;" in server
    assert "httpd_queue_work(" in server
    assert "kHeartbeatIntervalMs = 15000" in server
    assert "kAutoOffDelayMs = 120000" in (ROOT / "include/mod_client_config.h").read_text()
    assert "kMaxForwardDurationMs = 15000" in mock
    assert "kMaxForwardResponseBytes = 262144" in mock
    assert "kMaxFsdRequestBodyBytes = 65536" in mock
    assert "uint8_t enabled = 0;" in mock
    assert "uint8_t autoOffOnHit = 1;" in mock
    assert "client_config::scheduleAutoOff();" in mock
    assert "mbedtls_ssl_get_version" in mock

    # UI must expose autoOff toggle and distinguish active vs enabled/safeMode.
    for ui_path in ("include/web_ui_manage.h", "include/web_ui_fsd.h"):
        ui_text = (ROOT / ui_path).read_text()
        for needle in ("fsdAutoOff", "autoOffPending", "autoOffRemainingSec", "fsd443", "fsd4567", "safeMode", "lastError"):
            assert needle in ui_text, f"{ui_path}: missing {needle}"


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
        (654, "bd262907ff9fb2709bb54f9d3e78d07d", 22),
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
    check_upstream_header_capture()
    check_payload_compact_and_sum_parity()
    check_port_policy()
    print(f"FSD payload: {len(found)} presets, URI matcher {len(true_cases)}/{len(false_cases)} cases, "
          f"TLS chain + reply contract + 443/10010 policy OK")


if __name__ == "__main__":
    main()
