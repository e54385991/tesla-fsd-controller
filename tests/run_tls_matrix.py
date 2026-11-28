#!/usr/bin/env python3
"""External TLS version matrix for the FSD 443 listener.

Run this from a phone/PC connected to the device AP (default 9.9.9.9):

    python3 tests/run_tls_matrix.py --host 9.9.9.9 --rounds 10 --concurrency 4

The script verifies:
  * TLS 1.2 and TLS 1.3 handshakes, plus the default negotiation path
  * TLS 1.0/1.1 are rejected with a protocol-version alert
  * certificate chain use (CA bundle extracted from fsd_tls_identity.h)
  * api-prd / mothership / fsd.local SNI, and a no-SNI client
  * ALPN h2 offers fall back to HTTP/1.1
  * keep-alive reuse on one TLS connection
  * parallel handshakes (the regression that produced
    mbedtls_ssl_handshake -0x0050 when max_open_sockets was 1)

Exit code is non-zero if any required case fails. Optional --json-out writes a
machine-readable summary.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import http.client
import json
import re
import socket
import ssl
import statistics
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TLS_IDENTITY = ROOT / "include" / "fsd_tls_identity.h"
FSD_PATH = "/feature_configuration"
DEFAULT_SNI = "api-prd.vn.cloud.tesla.cn"


@dataclass
class Result:
    name: str
    ok: bool
    detail: str = ""
    elapsed_ms: float = 0.0
    tls_version: str = ""
    alpn: str = ""
    status: int | None = None


@dataclass
class Matrix:
    results: list[Result] = field(default_factory=list)

    def add(self, name: str, ok: bool, detail: str = "", **kwargs) -> Result:
        r = Result(name=name, ok=ok, detail=detail, **kwargs)
        self.results.append(r)
        mark = "PASS" if ok else "FAIL"
        extra = f" tls={r.tls_version}" if r.tls_version else ""
        if r.status is not None:
            extra += f" status={r.status}"
        if r.elapsed_ms:
            extra += f" ms={r.elapsed_ms:.0f}"
        print(f"[{mark}] {name}{extra}" + (f" {detail}" if detail else ""), flush=True)
        return r

    @property
    def failed(self) -> list[Result]:
        return [r for r in self.results if not r.ok]


def extract_ca_bundle(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    m = re.search(r'kFsdTlsCaCertificate\[\][^=]*=\s*R"PEM\((.*?)\)PEM"', text, re.S)
    if not m:
        raise SystemExit(f"cannot find kFsdTlsCaCertificate in {path}")
    pem = m.group(1).strip()
    if "BEGIN CERTIFICATE" not in pem:
        raise SystemExit(f"no PEM certificates in {path}")
    return pem + "\n"


def make_context(version: str | None, alpn: list[str] | None,
                 ca_file: str | None, verify: bool) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    if verify:
        if not ca_file:
            raise SystemExit("--ca is required unless --insecure is used")
        ctx.load_verify_locations(cafile=ca_file)
        ctx.check_hostname = True
        ctx.verify_mode = ssl.CERT_REQUIRED
    else:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    if version == "1.2":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    elif version == "1.3":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    if alpn:
        ctx.set_alpn_protocols(alpn)
    return ctx


def tls_request(host: str, port: int, sni: str | None, method: str, path: str,
                version: str | None, alpn: list[str] | None, ca_file: str | None,
                verify: bool, timeout: float,
                extra_headers: dict[str, str] | None = None) -> tuple[Result, bytes]:
    t0 = time.monotonic()
    tls = None
    try:
        raw = socket.create_connection((host, port), timeout=timeout)
        ctx = make_context(version, alpn, ca_file, verify)
        tls = ctx.wrap_socket(raw, server_hostname=sni if verify else (sni or None))
        headers = {"Host": sni or host, "Connection": "close"}
        if extra_headers:
            headers.update(extra_headers)
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.sock = tls
        conn.request(method, path, headers=headers)
        resp = conn.getresponse()
        body = resp.read()
        elapsed = (time.monotonic() - t0) * 1000
        result = Result(
            name="", ok=True,
            elapsed_ms=elapsed,
            tls_version=tls.version() or "",
            alpn=tls.selected_alpn_protocol() or "",
            status=resp.status,
        )
        result.detail = f"cipher={tls.cipher()[0] if tls.cipher() else '?'}"
        return result, body
    except Exception as exc:  # noqa: BLE001 - the matrix reports every failure
        elapsed = (time.monotonic() - t0) * 1000
        return Result(name="", ok=False, detail=f"{type(exc).__name__}: {exc}",
                      elapsed_ms=elapsed), b""
    finally:
        try:
            if tls is not None:
                tls.close()
        except Exception:
            pass


def check_fsd(host: str, port: int, version: str | None, ca_file: str | None,
              verify: bool, timeout: float, matrix: Matrix) -> None:
    label = "default" if version is None else f"tls{version}"
    t0 = time.monotonic()
    tls = None
    ok = False
    detail = ""
    status = None
    tls_ver = ""
    try:
        raw = socket.create_connection((host, port), timeout=timeout)
        ctx = make_context(version, None, ca_file, verify)
        tls = ctx.wrap_socket(raw, server_hostname=DEFAULT_SNI)
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.sock = tls
        conn.request("HEAD", FSD_PATH, headers={
            "Host": DEFAULT_SNI, "Connection": "close"})
        resp = conn.getresponse()
        resp.read()
        status = resp.status
        clen = resp.getheader("Content-Length")
        checksum = resp.getheader("X-Checksum")
        ctype = resp.getheader("Content-Type")
        ok = (status == 200 and clen is not None and checksum is not None
              and ctype == "application/json")
        detail = f"len={clen} checksum={checksum} type={ctype}"
        tls_ver = tls.version() or ""
    except Exception as exc:  # noqa: BLE001
        detail = f"{type(exc).__name__}: {exc}"
    finally:
        try:
            if tls is not None:
                tls.close()
        except Exception:
            pass
    matrix.add(f"{label}-fsd-head", ok, detail,
               elapsed_ms=(time.monotonic() - t0) * 1000,
               tls_version=tls_ver, status=status)


def check_sni(host: str, port: int, sni: str, ca_file: str | None,
              verify: bool, timeout: float, matrix: Matrix) -> None:
    result, _ = tls_request(host, port, sni, "GET", "/", "1.2", None,
                            ca_file, verify, timeout)
    # Any HTTP status proves the handshake and HTTP layer completed. The FSD
    # listener intentionally returns 502/400 for non-FSD/unknown Host paths.
    matrix.add(f"sni-{sni}", result.ok, result.detail,
               elapsed_ms=result.elapsed_ms, tls_version=result.tls_version,
               status=result.status)


def check_no_sni(host: str, port: int, ca_file: str | None, timeout: float,
                 matrix: Matrix) -> None:
    # Verification needs a hostname; no-SNI is only meaningful in insecure mode
    # or when the caller explicitly accepts the certificate.
    result, _ = tls_request(host, port, None, "GET", "/", "1.2", None,
                            ca_file, False, timeout)
    matrix.add("no-sni", result.ok, result.detail,
               elapsed_ms=result.elapsed_ms, tls_version=result.tls_version,
               status=result.status)


def check_alpn(host: str, port: int, ca_file: str | None, verify: bool,
               timeout: float, matrix: Matrix) -> None:
    result, _ = tls_request(host, port, DEFAULT_SNI, "GET", "/", "1.3", ["h2", "http/1.1"],
                            ca_file, verify, timeout)
    # The server does not advertise ALPN; a client offering h2 must still
    # complete TLS and HTTP/1.1. selected_alpn_protocol() should be empty.
    ok = result.ok and not result.alpn
    matrix.add("alpn-h2-fallback", ok,
               f"selected={result.alpn or '-'} {result.detail}",
               elapsed_ms=result.elapsed_ms, tls_version=result.tls_version,
               status=result.status)


def check_keepalive(host: str, port: int, ca_file: str | None, verify: bool,
                    timeout: float, matrix: Matrix) -> None:
    t0 = time.monotonic()
    tls = None
    ok = False
    detail = ""
    tls_ver = ""
    try:
        raw = socket.create_connection((host, port), timeout=timeout)
        ctx = make_context("1.2", None, ca_file, verify)
        tls = ctx.wrap_socket(raw, server_hostname=DEFAULT_SNI)
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.sock = tls
        statuses = []
        for _ in range(2):
            conn.request("HEAD", FSD_PATH, headers={
                "Host": DEFAULT_SNI, "Connection": "keep-alive"})
            resp = conn.getresponse()
            resp.read()
            statuses.append(resp.status)
        ok = all(s == 200 for s in statuses)
        detail = f"statuses={statuses}"
        tls_ver = tls.version() or ""
    except Exception as exc:  # noqa: BLE001
        detail = f"{type(exc).__name__}: {exc}"
    finally:
        try:
            if tls is not None:
                tls.close()
        except Exception:
            pass
    matrix.add("keepalive-2x", ok, detail,
               elapsed_ms=(time.monotonic() - t0) * 1000, tls_version=tls_ver)


def check_legacy_rejected(host: str, port: int, version: str, timeout: float,
                          matrix: Matrix) -> None:
    try:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        v = ssl.TLSVersion.TLSv1 if version == "1.0" else ssl.TLSVersion.TLSv1_1
        ctx.minimum_version = v
        ctx.maximum_version = v
    except (ValueError, ssl.SSLError) as exc:
        matrix.add(f"tls{version}-rejected", True,
                   f"SKIP client cannot offer TLS{version}: {exc}")
        return
    try:
        raw = socket.create_connection((host, port), timeout=timeout)
        try:
            tls = ctx.wrap_socket(raw, server_hostname=DEFAULT_SNI)
            tls.close()
            matrix.add(f"tls{version}-rejected", False, "server accepted legacy TLS")
        finally:
            raw.close()
    except (ssl.SSLError, OSError) as exc:
        text = str(exc)
        low = text.lower()
        # OpenSSL 3 / Python may refuse to build a legacy ClientHello before
        # contacting the server. That is a client limitation, not a server
        # acceptance; record it as a skip.
        if "no protocols available" in low or "no protocols" in low:
            matrix.add(f"tls{version}-rejected", True,
                       f"SKIP client cannot offer TLS{version}: {text}")
            return
        # Protocol-version alerts, EOF after alert, and resets all prove the
        # server refused the legacy ClientHello.
        ok = any(s in low for s in (
            "protocol version", "unsupported protocol", "alert",
            "eof", "reset", "wrong version"))
        matrix.add(f"tls{version}-rejected", ok, text)


def sequential_rounds(host: str, port: int, version: str | None, rounds: int,
                      ca_file: str | None, verify: bool, timeout: float,
                      matrix: Matrix) -> None:
    label = "default" if version is None else f"tls{version}"
    times: list[float] = []
    failures = 0
    for _ in range(max(1, rounds)):
        result, _ = tls_request(host, port, DEFAULT_SNI, "HEAD", FSD_PATH,
                                version, None, ca_file, verify, timeout)
        if result.ok:
            times.append(result.elapsed_ms)
        else:
            failures += 1
    if failures:
        matrix.add(f"{label}-rounds-{rounds}", False,
                   f"{failures}/{rounds} handshakes failed")
        return
    p50 = statistics.median(times)
    p95 = sorted(times)[max(0, int(len(times) * 0.95) - 1)]
    matrix.add(f"{label}-rounds-{rounds}", True,
               f"p50={p50:.0f}ms p95={p95:.0f}ms max={max(times):.0f}ms")


def concurrent_rounds(host: str, port: int, concurrency: int, rounds: int,
                      ca_file: str | None, verify: bool, timeout: float,
                      matrix: Matrix) -> None:
    versions = [None, "1.2", "1.3"]
    jobs = []
    for i in range(max(1, concurrency) * max(1, rounds)):
        jobs.append(versions[i % len(versions)])
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, concurrency)) as pool:
        futs = [pool.submit(tls_request, host, port, DEFAULT_SNI, "HEAD", FSD_PATH,
                            v, None, ca_file, verify, timeout)
                for v in jobs]
        results = [f.result()[0] for f in futs]
    failures = [r for r in results if not r.ok]
    elapsed = (time.monotonic() - t0) * 1000
    detail = f"{len(results) - len(failures)}/{len(results)} in {elapsed:.0f}ms"
    if failures:
        detail += " first=" + failures[0].detail
    matrix.add(f"concurrent-{concurrency}x{rounds}", not failures, detail)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="9.9.9.9")
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--rounds", type=int, default=10)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--concurrency-rounds", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--ca", type=Path, default=None,
                        help="CA PEM file; by default extracted from include/fsd_tls_identity.h")
    parser.add_argument("--insecure", action="store_true")
    parser.add_argument("--json-out", type=Path, default=None)
    args = parser.parse_args()

    ca_file: str | None = None
    tmp = None
    if not args.insecure:
        if args.ca:
            ca_file = str(args.ca)
        else:
            tmp = tempfile.NamedTemporaryFile("w", prefix="fsd-ca-", suffix=".pem",
                                              delete=False)
            tmp.write(extract_ca_bundle(TLS_IDENTITY))
            tmp.close()
            ca_file = tmp.name

    matrix = Matrix()
    try:
        check_fsd(args.host, args.port, "1.2", ca_file, not args.insecure,
                  args.timeout, matrix)
        check_fsd(args.host, args.port, "1.3", ca_file, not args.insecure,
                  args.timeout, matrix)
        check_fsd(args.host, args.port, None, ca_file, not args.insecure,
                  args.timeout, matrix)
        for sni in ("api-prd.vn.cloud.tesla.cn", "mothership.vn.teslamotors.com",
                    "fsd.local"):
            check_sni(args.host, args.port, sni, ca_file, not args.insecure,
                      args.timeout, matrix)
        check_no_sni(args.host, args.port, ca_file, args.timeout, matrix)
        check_alpn(args.host, args.port, ca_file, not args.insecure,
                   args.timeout, matrix)
        check_keepalive(args.host, args.port, ca_file, not args.insecure,
                        args.timeout, matrix)
        check_legacy_rejected(args.host, args.port, "1.0", args.timeout, matrix)
        check_legacy_rejected(args.host, args.port, "1.1", args.timeout, matrix)
        sequential_rounds(args.host, args.port, "1.2", args.rounds, ca_file,
                          not args.insecure, args.timeout, matrix)
        sequential_rounds(args.host, args.port, "1.3", args.rounds, ca_file,
                          not args.insecure, args.timeout, matrix)
        sequential_rounds(args.host, args.port, None, args.rounds, ca_file,
                          not args.insecure, args.timeout, matrix)
        concurrent_rounds(args.host, args.port, args.concurrency,
                          args.concurrency_rounds, ca_file, not args.insecure,
                          args.timeout, matrix)
    finally:
        if tmp is not None:
            try:
                Path(tmp.name).unlink()
            except OSError:
                pass

    failures = matrix.failed
    summary = {
        "host": args.host,
        "port": args.port,
        "total": len(matrix.results),
        "passed": len(matrix.results) - len(failures),
        "failed": len(failures),
        "results": [r.__dict__ for r in matrix.results],
    }
    if args.json_out:
        args.json_out.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\n{summary['passed']}/{summary['total']} checks passed")
    if failures:
        print("FAILURES:")
        for r in failures:
            print(f"  - {r.name}: {r.detail}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
