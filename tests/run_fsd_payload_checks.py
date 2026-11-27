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
        '503, "ap_upstream_blocked"',
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
    # Last-response preview must hold the full 2020 B CN preset without truncation.
    m = re.search(r"kLastResponseCapacity\s*=\s*(\d+)", mock_header)
    assert m and int(m.group(1)) >= 2021, "kLastResponseCapacity too small for 2020 B CN payload"
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
    in_string = False
    in_char = False
    escape = False
    while depth:
        ch = source[end]
        if escape:
            escape = False
        elif in_string:
            if ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
        elif in_char:
            if ch == "\\":
                escape = True
            elif ch == "'":
                in_char = False
        else:
            if ch == '"':
                in_string = True
            elif ch == "'":
                in_char = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
        end += 1
    if end < len(source) and source[end] == ";":
        end += 1
    return source[start:end]


def _sanitizer_flags(cxx: str) -> list[str]:
    with tempfile.TemporaryDirectory(prefix="san-probe-") as tmp:
        probe = Path(tmp) / "p.cpp"
        out = Path(tmp) / "p"
        probe.write_text("int main(){return 0;}\n")
        r = subprocess.run([cxx, "-fsanitize=address,undefined", str(probe), "-o", str(out)],
                           capture_output=True)
        return ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if r.returncode == 0 else []


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
constexpr size_t kMaxForwardedRespHeaders = 22;
constexpr size_t kMaxRespHeaderTotalBytes = 4096;
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
    san = _sanitizer_flags(cxx)
    with tempfile.TemporaryDirectory(prefix="fsd-hdr-") as tmp:
        cpp = Path(tmp) / "hdr.cpp"
        exe = Path(tmp) / "hdr"
        cpp.write_text(harness)
        subprocess.run(
            [cxx, "-std=gnu++26", "-Wall", "-Wextra", "-Werror",
             *san,
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
    san = _sanitizer_flags(cxx)
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
            [cxx, "-std=gnu++26", "-Wall", "-Wextra", "-Werror",
             *san,
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
    assert "PC 602B" in ui, "web_ui_fsd PC preset button must show 602B"
    assert "默认 653B" not in ui, "stale 653B label found in web_ui_fsd.h"
    assert 'data-preset="2031"' in ui, "web_ui_fsd must expose the 62-field CN preset"
    assert "CN 全量 2031B" in ui, "web_ui_fsd CN preset button label drifted"
    assert 'data-preset="1466"' in ui, "web_ui_fsd must keep the 46-field CN preset"
    assert "CN 旧版 1466B" in ui, "web_ui_fsd CN 1466 button label drifted"
    assert "eu_fsd_allowed_country_codes" in ui, "web_ui_fsd field docs missing CN preset keys"
    mock = (ROOT / "src/fsd_mock.cpp").read_text()
    assert 'strcasecmp(name, "2031")' in mock, "loadPreset must accept the 2031 preset"
    assert 'strcasecmp(name, "cn46")' in mock, "loadPreset must keep the 1466 preset"


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
    # 443 ClientHello/accept policy must stay identical to aa0a69f8b.
    assert "config.httpd.max_open_sockets = 1;" in server
    assert "config.httpd.backlog_conn = 1;" in server
    assert "config.httpd.lru_purge_enable = false;" in server
    assert "config.httpd.keep_alive_interval = 5;" in server
    assert "config.httpd.keep_alive_count = 3;" in server
    assert "config.max_open_sockets = 3;" in server
    # PC parity: plaintext mothership listener + keep-host upstream routing.
    assert "config.server_port = fsd_mock::kMothershipHttpPort;" in server
    assert "startMothershipHttp()" in server
    assert "mothershipHttpStarted()" in server
    assert "const char* upstreamHost = kApiPrdHost;" in mock
    assert "kMothershipBlockedUpstream" in mock
    assert "192.0.2.1:4567" in mock
    assert 'if (strcasecmp(name, "Host") == 0) continue;' in mock
    assert "sendProxyError(" in mock
    assert '"Server", "mitmproxy 12.3.1"' in mock
    assert "mothership.vn.teslamotors.com" in mock
    assert "mothership.vn.cloud.tesla.cn" in mock
    # Default external blocklist carries both mothership hosts and migrates
    # the v4 api-prd-only default.
    assert "mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn" in bridge
    assert "assistant-api.prd.cnn1.vn.cloud.tesla.cn" in bridge
    assert "kTeslaMinBlockV6" in bridge
    for path in ("include/web_ui_manage.h",):
        text = (ROOT / path).read_text()
        assert "mothership.vn.teslamotors.com" in text, f"{path}: missing mothership blocklist entry"
        assert "mothership.vn.cloud.tesla.cn" in text, f"{path}: missing mothership blocklist entry"
        assert "dnsReset" in text, f"{path}: missing one-click DNS default reset"
    # Default reset is served from the firmware constants, not duplicated JS.
    assert "dnsReset" in main
    assert "kTeslaMinAllowV8" in main
    assert "kTeslaMinBlockV7" in main
    # hermes is optional: the default v8 stays "no vehicle control"; the UI
    # checkbox appends it to the end of the current allowlist at runtime.
    v8 = re.search(r'kTeslaMinAllowV8\s*=\s*"(.*?)";', bridge, re.S)
    assert v8
    v8_items = v8.group(1).split()
    assert "hermes-prd.vn.cloud.tesla.cn" not in v8_items
    # v8: connman at the head, www.tesla.cn absent, QQ explicit entries added.
    assert v8_items[0] == "connman.vn.cloud.tesla.cn"
    assert "www.tesla.cn" not in v8_items
    assert len(v8_items) == 47
    for domain in ("qplaycloud.y.qq.com", "y.gtimg.cn", "y.qq.com",
                   "stat6.y.qq.com", "music-file.y.qq.com", "qpic.y.qq.com",
                   "isure6.stream.qqmusic.qq.com",
                   "isure6-stream-qqmusic.a.bdycdn.cn"):
        assert domain in v8_items, f"v8 allowlist missing {domain}"
    # Legacy v7 must stay for migration detection.
    assert 'kTeslaMinAllowV7' in bridge
    b7 = re.search(r'kTeslaMinBlockV7\s*=\s*"(.*?)";', bridge, re.S)
    assert b7
    b7_items = b7.group(1).split()
    assert len(b7_items) == 12
    for domain in ("assistant-api.prd.cnn1.vn.cloud.tesla.cn",
                   "telemetry-prd.vn.cloud.tesla.cn",
                   "hermes-stream-prd.vn.cloud.tesla.cn",
                   "hermes-prd.vn.cloud.tesla.cn",
                   "apigateway-x2-trigger.tesla.cn",
                   "vehicle-files.prd.cnn1.vn.cloud.tesla.cn",
                   "vehicle-files.prd.cn1.vn.cloud.tesla.cn",
                   "upgrade.vn.cloud.tesla.cn",
                   "amazonaws.com.cn",
                   "api-prd.vn.cloud.tesla.cn"):
        assert domain in b7_items, f"v7 blocklist missing {domain}"
    # hermes must be blocklisted by default but never allowlisted.
    assert "hermes-prd.vn.cloud.tesla.cn" in b7_items
    assert "hermes-prd.vn.cloud.tesla.cn" not in v8_items
    # Last 5 下发记录 must survive reboot (NVS "cfglog", not a RAM ring).
    mock_src = (ROOT / "src/fsd_mock.cpp").read_text()
    mock_hdr = (ROOT / "include/mod_fsd_mock.h").read_text()
    assert "kConfigHistoryCapacity = 5" in mock_hdr
    assert '"cfglog"' in mock_src and "persistConfigLog()" in mock_src
    assert mock_src.count("persistConfigLog()") >= 2, "record path must persist"
    assert 'getBytesLength("cfglog")' in mock_src, "boot path must restore the ring"
    # Last 5 下发时间线（配置变更 → 车机取配置）must also survive reboot (NVS "timeline").
    assert "kTimelineCapacity = 5" in mock_hdr
    assert '"timeline"' in mock_src and "persistTimelineLogLocked()" in mock_src
    assert mock_src.count("persistTimelineEntry(") >= 3, "config and hit paths must persist timeline"
    assert 'getBytesLength("timeline")' in mock_src, "boot path must restore the timeline ring"
    assert '"/api/fsd-mock/timeline"' in main
    fsd_ui = (ROOT / "include/web_ui_fsd.h").read_text()
    assert "下发时间线（配置变更 → 车机取配置 · 最近 5 条 · 重启保持）" in fsd_ui
    assert '"/api/fsd-mock/timeline"' in fsd_ui or "'/api/fsd-mock/timeline'" in fsd_ui
    manage_ui = (ROOT / "include/web_ui_manage.h").read_text()
    assert "hermesControl" in manage_ui
    assert "hermes-prd.vn.cloud.tesla.cn" in manage_ui
    # Ticking the hermes checkbox must auto-save (no manual DNS save needed)
    # and must survive a missing PIN by retrying after auth.
    toggle = re.search(r"async function toggleHermes\(\).*?\n\}", manage_ui, re.S)
    assert toggle and "await saveDnsRules()" in toggle.group(0), "hermes toggle must save immediately"
    assert "pendingAfterPin=saveDnsRules" in manage_ui, "DNS save must retry after PIN auth"
    # Unchecking must strip hermes from ANY position: tokenise like the firmware
    # (space/newline/comma/semicolon), compare case-insensitively, dedupe.
    assert "function dnsTokens" in manage_ui and r"split(/[\s,;]+/)" in manage_ui
    assert "function isHermesDomain" in manage_ui and "toLowerCase()" in manage_ui
    assert "function stripHermes" in manage_ui
    assert "hermes.checked=listHasHermes(d.dnsAllow)" in manage_ui
    toggle_src = re.search(r"async function toggleHermes\(\).*?\n\}", manage_ui, re.S)
    assert toggle_src and "applyHermes(false)" in toggle_src.group(0)
    apply_src = re.search(r"function applyHermes\(want\).*?\n\}", manage_ui, re.S)
    assert apply_src and "stripHermes(ta.value)" in apply_src.group(0)
    assert "parts.push(HERMES_DOMAIN)" in apply_src.group(0)
    # Enabling must temporarily drop hermes from the blacklist; disabling adds
    # it back (default protection restored).
    assert "function applyHermesBlock" in manage_ui
    blk_src = re.search(r"function applyHermesBlock\(want\).*?\n\}", manage_ui, re.S)
    assert blk_src and "stripHermes(tb.value)" in blk_src.group(0)
    assert "if(!want) parts.push(HERMES_DOMAIN)" in blk_src.group(0)
    assert "applyHermesBlock(want)" in apply_src.group(0)
    # Enabling remote control requires a 10-second countdown confirmation that
    # explains the online-config need, CAN/FSD history, and ban risk.
    assert "hermesConfirm" in manage_ui and "hermesConfirmBtn" in manage_ui
    assert 'var left=10;' in manage_ui, "hermes confirmation must start at 10s"
    assert 'btn.disabled=true' in manage_ui, "confirm button must stay disabled during countdown"
    for phrase in ("封禁 FSD", "认为在线",
                   "Model 3/Y", "Model S/X", "S/X 请勿开启"):
        assert phrase in manage_ui, f"hermes confirmation missing warning: {phrase}"
    # v5 default allowlist: consolidated Apple Music + Doubao domains.
    for domain in ("*.apple.com", "mzstatic.com", "baidu.com", "bdimg.com", "bcebos.com",
                   "qq.com", "tencentmusic.com", "163.com", "126.net", "netease.com",
                   "doubao.com", "doubaoapps.com", "doubaocdn.com", "butterflyai.cn",
                   "byteimg.com", "bytescm.com", "bytednsdoc.com", "snssdk.com"):
        assert domain in bridge, f"default allowlist missing {domain}"


def check_tls_policy() -> None:
    """All Wi-Fi envs run espressif32@8.0.0 / mbedTLS 3.6 (TLS 1.2 + 1.3) with IPv6 disabled."""
    all_cfgs = [
        ROOT / "sdkconfig.defaults",
        ROOT / "sdkconfig.esp32-wifi",
        ROOT / "sdkconfig.esp32-wifi-debug",
        ROOT / "sdkconfig.tls13.defaults",
        ROOT / "sdkconfig.esp32s3-waveshare-wifi",
        ROOT / "sdkconfig.esp32s3-waveshare-wifi-debug",
    ]
    s3_cfgs = [
        ROOT / "sdkconfig.tls13.defaults",
        ROOT / "sdkconfig.esp32s3-waveshare-wifi",
        ROOT / "sdkconfig.esp32s3-waveshare-wifi-debug",
    ]
    for path in all_cfgs:
        text = path.read_text()
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y" in text, f"{path.name}: TLS 1.2 must stay enabled"
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y" in text, f"{path.name}: TLS 1.3 must stay enabled"
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1=y" not in text, f"{path.name}: TLS 1.0 must stay disabled"
        assert "CONFIG_MBEDTLS_SSL_PROTO_TLS1_1=y" not in text, f"{path.name}: TLS 1.1 must stay disabled"
        assert "# CONFIG_LWIP_IPV6 is not set" in text and "CONFIG_LWIP_IPV6=y" not in text, (
            f"{path.name}: IPv6 must stay disabled"
        )
        assert "CONFIG_LWIP_IPV6_FORWARD=y" not in text, f"{path.name}: IPv6 forwarding must stay disabled"
    for path in s3_cfgs:
        text = path.read_text()
        assert "CONFIG_HTTPD_MAX_REQ_HDR_LEN=8192" in text, (
            f"{path.name}: httpd header limit must stay >= 8192 to avoid HTTP 431 on JWT/browser headers"
        )
        assert "CONFIG_HTTPD_MAX_URI_LEN=1024" in text, (
            f"{path.name}: httpd URI limit must stay >= 1024"
        )
        assert "# CONFIG_MBEDTLS_DYNAMIC_BUFFER is not set" in text, (
            f"{path.name}: DYNAMIC_BUFFER must stay disabled (IDF 6.1.1 esp_ssl_srv TLS 1.2 fallback deadlock)"
        )
        assert "CONFIG_SPIRAM=y" in text and "CONFIG_SPIRAM_IGNORE_NOTFOUND=y" in text, (
            f"{path.name}: ESP32-S3 PSRAM with safe non-PSRAM fallback must be enabled"
        )
        assert "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y" in text, (
            f"{path.name}: mbedTLS must use default alloc so large TLS buffers spill to PSRAM"
        )
        assert "CONFIG_LWIP_MAX_SOCKETS=20" in text, (
            f"{path.name}: lwIP socket pool must accommodate multi-socket 443/4567 + async proxy"
        )
        assert "# CONFIG_ESP_HTTPS_SERVER_EVENTS is not set" in text, (
            f"{path.name}: unused HTTPS server event posts should stay disabled"
        )

    platformio = (ROOT / "platformio.ini").read_text()
    assert platformio.count("https://github.com/pioarduino/platform-espressif32/releases/download/61.04.00/platform-espressif32.zip") >= 4
    assert 'board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.tls13.defaults"' in platformio
    for ui_path in ("include/web_ui_manage.h", "include/web_ui_fsd.h"):
        ui_text = (ROOT / ui_path).read_text()
        assert "fetch('/api/auth'" in ui_text, f"{ui_path}: PIN login must POST to /api/auth"
        assert "fetch('/api/pin'" not in ui_text, f"{ui_path}: PIN login must not hit /api/pin"

    server = (ROOT / "src/client_config.cpp").read_text()
    mock = (ROOT / "src/fsd_mock.cpp").read_text()
    assert "config.user_cb = tlsUserCallback;" in server
    assert "HTTPD_SSL_USER_CB_SESS_CREATE" in server
    assert "HTTPD_SSL_USER_CB_SESS_ERROR" in server
    assert "mbedtls_ssl_get_fatal_alert" in mock
    assert "char event[20]" in (ROOT / "include/mod_fsd_mock.h").read_text()
    assert "sameAlertBurst" in mock
    assert "httpd_req_async_handler_begin" in mock
    assert "httpd_req_async_handler_complete" in mock
    assert "fsd_mock::startProxyWorker();" in server
    assert "fsd_mock::stopProxyWorker();" in server
    assert "mbedtls_platform_set_calloc_free" in server
    assert "heap_caps_calloc_prefer" in server
    assert "kFsdMaxReqHdrLen = 8192" in server
    assert "config.max_req_hdr_len = kFsdMaxReqHdrLen;" in server
    assert "config.httpd.max_req_hdr_len = kFsdMaxReqHdrLen;" in server
    assert "config.max_resp_headers = kFsdMaxRespHeaders;" in server
    assert "config.httpd.max_resp_headers = kFsdMaxRespHeaders;" in server
    # Anti-hang & non-blocking concurrency contracts
    assert "config.keep_alive_enable = true;" in server
    assert "config.httpd.keep_alive_enable = true;" in server
    assert "config.httpd.lru_purge_enable = true;" not in server
    assert "dnsPaused" not in server, "busy httpd must never pause DNS hijack"
    assert "config.tls_handshake_timeout_ms = 4000;" in server
    # OOM/panic guard: never open a second TLS context when heap is thin, and
    # fast-fail repeated upstream failures instead of handshake storms.
    assert "ESP.getFreeHeap() < 45000" in mock
    assert "heap_caps_get_largest_free_block" in mock
    assert "upstreamCoolingDown(" in mock
    assert "upstreamRecordFailure(" in mock
    assert "httpd_queue_work(" in server
    assert "kHeartbeatIntervalMs = 15000" in server
    assert "kAutoOffDelayMs = 120000" in (ROOT / "include/mod_client_config.h").read_text()
    # The 2-minute auto-off must not race a 45-second watchdog reboot.
    assert "kMissedRestart" not in server
    assert "wifiBridgeTriggerRestart" not in server
    assert "ESP.restart" not in server
    assert "wifiBridgeTriggerRestart" not in (ROOT / "src/main.cpp").read_text()
    assert "kMaxForwardDurationMs = 15000" in mock
    assert "kMaxForwardResponseBytes = 262144" in mock
    assert "kMaxFsdRequestBodyBytes = 65536" in mock
    assert "uint8_t enabled = 0;" in mock
    assert "uint8_t autoOffOnHit = 1;" in mock
    assert "client_config::scheduleAutoOff();" in mock
    assert "mbedtls_ssl_get_version" in mock

    # UI must expose autoOff toggle and distinguish active vs enabled.
    for ui_path in ("include/web_ui_manage.h", "include/web_ui_fsd.h"):
        ui_text = (ROOT / ui_path).read_text()
        for needle in ("fsdAutoOff", "autoOffPending", "autoOffRemainingSec", "fsd443", "fsd4567", "lastError"):
            assert needle in ui_text, f"{ui_path}: missing {needle}"

    # Safe mode is removed completely: no status flag, no badge, no boot skip.
    for path in ("include/mod_client_config.h", "include/web_ui_fsd.h",
                 "include/web_ui_manage.h", "src/client_config.cpp",
                 "src/fsd_mock.cpp", "src/main.cpp"):
        text = (ROOT / path).read_text()
        assert "safeMode" not in text, f"{path}: safe mode must stay removed"
        assert "安全模式" not in text, f"{path}: safe mode badge must stay removed"
        assert "safe_mode" not in text, f"{path}: safe mode error tag must stay removed"

    # Parity checks for all local/error/passthrough HTTP returns and PSRAM/self-test contracts.
    for needle in (
        'httpd_resp_send(request, nullptr, static_cast<ssize_t>(bytes))',
        'const bool triggerAutoOff = autoOff && request->method != HTTP_HEAD;',
        r'413, "{\"error\":\"request_too_large\"}"',
        r'400, "{\"error\":\"request_read_failed\"}"',
        r'500, "{\"error\":\"no_memory\"}"',
        'httpd_resp_set_hdr(request, "Server", "mitmproxy 12.3.1");',
        'sendProxyError(request, hit, responseBody, 503, "low_heap");',
        'sendProxyError(request, hit, responseBody, 502,',
        'Cannot connect to %s: [Errno 113] No route to host',
        'const bool sameAlertBurst = !newFailure &&',
        'if (detail[0] && strstr(entry.error, "-0x7780") != nullptr',
    ):
        assert needle in mock, f"src/fsd_mock.cpp missing parity contract: {needle}"

    client_cfg = (ROOT / "src/client_config.cpp").read_text()
    for needle in (
        'done pass=%d/6',
        'http4567-fsd-head',
        'http4567-flow-502',
        'tls-443-flow',
    ):
        assert needle in client_cfg, f"src/client_config.cpp missing selftest probe: {needle}"

    tls13_defaults = (ROOT / "sdkconfig.tls13.defaults").read_text()
    assert "CONFIG_SPIRAM_IGNORE_NOTFOUND=y" in tls13_defaults


def check_timeline_persistence() -> None:
    """Compile and run the production timeline ring, dedup, loopback filter,
    NVS blob restore/validation, cfglog migration, and JSONL export under ASan/UBSan."""
    cxx = shutil.which("c++")
    if not cxx:
        return
    hdr = (ROOT / "include/mod_fsd_mock.h").read_text()
    src = (ROOT / "src/fsd_mock.cpp").read_text()
    extracted_types = "\n".join((
        _extract_block(hdr, "struct Hit {"),
        _extract_block(hdr, "struct ConfigChange {"),
        _extract_block(hdr, "struct TimelineEntry {"),
        _extract_block(src, "struct TimelineLogBlob {"),
    ))
    extracted_funcs = "\n".join((
        _extract_block(src, "void jsonEscape("),
        _extract_block(src, "bool validTimelineKind("),
        _extract_block(src, "void sanitizeTimelineEntry("),
        _extract_block(src, "bool restoreTimelineBlob("),
        _extract_block(src, "bool pushTimelineEntry("),
        _extract_block(src, "bool shouldPersistHitToTimeline("),
        _extract_block(src, "void seedTimelineFromConfigHistory("),
        _extract_block(src, "bool readTimelineEntry("),
        _extract_block(src, "void appendTimelineJsonl("),
    ))
    harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Print {
    std::string buf;
    void print(const char* s) { if (s) buf += s; }
    void print(unsigned long v) { buf += std::to_string(v); }
    void print(unsigned v) { buf += std::to_string(v); }
    void print(char c) { buf.push_back(c); }
    void write(uint8_t c) { buf.push_back(static_cast<char>(c)); }
};

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) (void)(x)
#define portEXIT_CRITICAL(x) (void)(x)

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !(defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)))
static size_t strlcpy(char* dst, const char* src, size_t size) {
    if (!src) src = "";
    const size_t len = std::strlen(src);
    if (size > 0) {
        const size_t copy = len < size - 1 ? len : size - 1;
        std::memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#endif

constexpr size_t kConfigHistoryCapacity = 5;
constexpr size_t kTimelineCapacity = 5;
constexpr uint32_t kTimelineLogMagic = 0x544D4C31;

''' + extracted_types + r'''

ConfigChange gConfigHistory[kConfigHistoryCapacity];
size_t gConfigCount = 0;
size_t gConfigWrite = 0;
portMUX_TYPE gConfigMux = portMUX_INITIALIZER_UNLOCKED;

TimelineEntry gTimeline[kTimelineCapacity];
size_t gTimelineCount = 0;
size_t gTimelineWrite = 0;
portMUX_TYPE gTimelineMux = portMUX_INITIALIZER_UNLOCKED;

''' + extracted_funcs + r'''

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

static TimelineEntry makeCfg(const char* ts, const char* src, const char* md5, uint32_t bytes, uint16_t fields) {
    TimelineEntry e = {};
    strlcpy(e.kind, "CFG", sizeof(e.kind));
    strlcpy(e.ts, ts, sizeof(e.ts));
    strlcpy(e.source, src, sizeof(e.source));
    strlcpy(e.md5, md5, sizeof(e.md5));
    e.bytes = bytes;
    e.fields = fields;
    return e;
}

static TimelineEntry makeHit(const char* kind, const char* ts, const char* method, const char* vin,
                             const char* md5, uint32_t bytes, const char* mode, uint16_t status) {
    TimelineEntry e = {};
    strlcpy(e.kind, kind, sizeof(e.kind));
    strlcpy(e.ts, ts, sizeof(e.ts));
    strlcpy(e.method, method, sizeof(e.method));
    strlcpy(e.vin, vin, sizeof(e.vin));
    strlcpy(e.reqHead, "{\"ver\":1}", sizeof(e.reqHead));
    strlcpy(e.md5, md5, sizeof(e.md5));
    strlcpy(e.mode, mode, sizeof(e.mode));
    strlcpy(e.httpVersion, "HTTP/1.1", sizeof(e.httpVersion));
    e.reqBytes = 9;
    e.bytes = bytes;
    e.status = status;
    return e;
}

int main() {
    static_assert(sizeof(TimelineLogBlob) == 8 + kTimelineCapacity * sizeof(TimelineEntry),
                  "TimelineLogBlob layout must match expected NVS size");

    // 1. Loopback and FLOW filtering.
    Hit flow = {};
    strlcpy(flow.kind, "FLOW", sizeof(flow.kind));
    strlcpy(flow.client, "9.9.9.2", sizeof(flow.client));
    assert(!shouldPersistHitToTimeline(flow));

    Hit loopback = {};
    strlcpy(loopback.kind, "HIT", sizeof(loopback.kind));
    strlcpy(loopback.client, "127.0.0.1", sizeof(loopback.client));
    assert(!shouldPersistHitToTimeline(loopback));
    strlcpy(loopback.client, "::1", sizeof(loopback.client));
    assert(!shouldPersistHitToTimeline(loopback));

    Hit carHit = {};
    strlcpy(carHit.kind, "HIT", sizeof(carHit.kind));
    strlcpy(carHit.client, "9.9.9.2", sizeof(carHit.client));
    assert(shouldPersistHitToTimeline(carHit));
    strlcpy(carHit.kind, "OBSERVE", sizeof(carHit.kind));
    assert(shouldPersistHitToTimeline(carHit));

    // 2. Coalesce consecutive CFG with identical md5, keep non-consecutive CFG after HIT,
    //    preserve true insertion order across mixed up:XX and ISO timestamps, and cap at 5.
    assert(pushTimelineEntry(makeCfg("up:10", "config", "aaa111", 654, 22)));
    assert(pushTimelineEntry(makeCfg("up:20", "payload", "aaa111", 654, 22)));  // coalesced
    assert(gTimelineCount == 1);
    assert(pushTimelineEntry(makeHit("HIT", "up:30", "GET", "LRW3E7FS0NC000001", "aaa111", 654, "reply", 200)));
    assert(pushTimelineEntry(makeCfg("2026-11-28T04:00:00+0800", "preset", "aaa111", 654, 22)));  // after HIT -> kept
    assert(pushTimelineEntry(makeHit("OBSERVE", "up:15", "POST", "LRW3E7FS0NC000001", "bbb222", 2031, "observe", 200)));
    assert(pushTimelineEntry(makeCfg("2026-11-28T04:01:00+0800", "config", "ccc333", 2031, 62)));
    assert(pushTimelineEntry(makeHit("HIT", "2026-11-28T04:02:00+0800", "POST", "LRW3E7FS0NC000001", "ccc333", 2031, "reply", 200)));
    assert(pushTimelineEntry(makeHit("HIT", "up:9", "GET", "LRW3E7FS0NC000002", "ccc333", 2031, "reply", 200)));
    assert(gTimelineCount == 5);

    Print beforeReboot;
    appendTimelineJsonl(beforeReboot);
    auto beforeLines = splitLines(beforeReboot.buf);
    assert(beforeLines.size() == 5);
    // Newest first even when newest is "up:9" and older is ISO timestamp.
    assert(beforeLines[0].find("LRW3E7FS0NC000002") != std::string::npos);
    assert(beforeLines[1].find("2026-11-28T04:02:00+0800") != std::string::npos);
    assert(beforeLines[2].find("\"ccc333\"") != std::string::npos);
    assert(beforeLines[3].find("\"OBSERVE\"") != std::string::npos);
    assert(beforeLines[4].find("\"preset\"") != std::string::npos);

    // 3. Persist blob -> wipe RAM -> restore -> JSONL must match byte-for-byte.
    TimelineLogBlob saved = {};
    saved.magic = kTimelineLogMagic;
    saved.count = static_cast<uint16_t>(gTimelineCount);
    saved.write = static_cast<uint16_t>(gTimelineWrite);
    for (size_t i = 0; i < kTimelineCapacity; ++i) saved.items[i] = gTimeline[i];

    for (auto& slot : gTimeline) slot = TimelineEntry{};
    gTimelineCount = 0;
    gTimelineWrite = 0;

    assert(restoreTimelineBlob(saved));
    Print afterReboot;
    appendTimelineJsonl(afterReboot);
    assert(afterReboot.buf == beforeReboot.buf);

    // 4. Reject corrupted blobs and sanitize unterminated strings.
    TimelineLogBlob badMagic = saved;
    badMagic.magic = 0x12345678;
    assert(!restoreTimelineBlob(badMagic));
    TimelineLogBlob badCount = saved;
    badCount.count = kTimelineCapacity + 1;
    assert(!restoreTimelineBlob(badCount));
    TimelineLogBlob badWrite = saved;
    badWrite.write = kTimelineCapacity;
    assert(!restoreTimelineBlob(badWrite));
    TimelineLogBlob badKind = saved;
    std::strcpy(badKind.items[0].kind, "BOGUS");
    assert(!restoreTimelineBlob(badKind));

    // Unterminated strings in a valid blob must be NUL-terminated without UB.
    TimelineLogBlob unterminated = {};
    unterminated.magic = kTimelineLogMagic;
    unterminated.count = 1;
    unterminated.write = 1;
    std::memset(&unterminated.items[0], 'x', sizeof(TimelineEntry));
    strlcpy(unterminated.items[0].kind, "CFG", sizeof(unterminated.items[0].kind));
    unterminated.items[0].bytes = 123;
    unterminated.items[0].fields = 5;
    assert(restoreTimelineBlob(unterminated));
    Print sanitizedOut;
    appendTimelineJsonl(sanitizedOut);
    assert(sanitizedOut.buf.find("\"kind\":\"CFG\"") != std::string::npos);

    // 5. Seed migration from legacy cfglog when NVS timeline key does not exist yet.
    for (auto& slot : gTimeline) slot = TimelineEntry{};
    gTimelineCount = 0;
    gTimelineWrite = 0;
    for (auto& c : gConfigHistory) c = ConfigChange{};
    strlcpy(gConfigHistory[0].ts, "2026-11-27T10:00:00+0800", 32);
    strlcpy(gConfigHistory[0].source, "config", 16);
    strlcpy(gConfigHistory[0].md5, "m1", 33);
    gConfigHistory[0].bytes = 100;
    gConfigHistory[0].fields = 2;

    strlcpy(gConfigHistory[1].ts, "2026-11-27T11:00:00+0800", 32);
    strlcpy(gConfigHistory[1].source, "preset", 16);
    strlcpy(gConfigHistory[1].md5, "m1", 33);  // duplicate md5 -> coalesced to newer
    gConfigHistory[1].bytes = 100;
    gConfigHistory[1].fields = 2;

    strlcpy(gConfigHistory[2].ts, "2026-11-27T12:00:00+0800", 32);
    strlcpy(gConfigHistory[2].source, "payload", 16);
    strlcpy(gConfigHistory[2].md5, "m2", 33);
    gConfigHistory[2].bytes = 200;
    gConfigHistory[2].fields = 4;
    gConfigCount = 3;
    gConfigWrite = 3;

    seedTimelineFromConfigHistory();
    Print seeded;
    appendTimelineJsonl(seeded);
    auto seededLines = splitLines(seeded.buf);
    assert(seededLines.size() == 2);
    assert(seededLines[0].find("\"m2\"") != std::string::npos);
    assert(seededLines[1].find("2026-11-27T11:00:00+0800") != std::string::npos);
    return 0;
}
'''
    san = _sanitizer_flags(cxx)
    with tempfile.TemporaryDirectory(prefix="fsd-tl-") as tmp:
        cpp = Path(tmp) / "tl.cpp"
        exe = Path(tmp) / "tl"
        cpp.write_text(harness)
        subprocess.run(
            [cxx, "-std=gnu++26", "-Wall", "-Wextra", "-Werror",
             *san, str(cpp), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


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
    assert len(found) == 6, f"expected 6 payloads, found {len(found)}"
    expected = [
        (602, "241cb01b81a2b9c8b9f389a471bcbc94", 20),
        (654, "bd262907ff9fb2709bb54f9d3e78d07d", 22),
        (476, "b61035d9dea38e737fbf8195ccb39f2c", 16),
        (282, "c212a75ca858750ac302093c138991f2", 9),
        (2031, "f69da5d74c1a4bcae4a6bbdf1128c91a", 62),
        (1466, "0ea421e01be6b6e8f66a8fb752970bb3", 46),
    ]
    for text, (size, digest, fields) in zip(found, expected):
        raw = text.encode("utf-8")
        assert len(raw) == size, (len(raw), size)
        assert hashlib.md5(raw).hexdigest() == digest
        assert len(json.loads(raw)) == fields
        assert b"\n" not in raw and b": " not in raw
        assert f'"{digest}"' in HEADER

    pc_json = json.loads(found[0])
    assert pc_json.get("disable_cabin_camera") is True
    assert len(pc_json) == 20
    default_json = json.loads(found[1])
    assert default_json.get("disable_cabin_camera") is False
    assert default_json.get("enable_eu_fsd_behavior") is False
    assert default_json.get("autoshift_dr") is True
    assert list(default_json)[-1] == "autoshift_dr"
    assert len(default_json) == 22

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
    check_timeline_persistence()
    print(f"FSD payload: {len(found)} presets, URI matcher {len(true_cases)}/{len(false_cases)} cases, "
          f"TLS chain + reply contract + 443/10010 policy OK")


if __name__ == "__main__":
    main()
