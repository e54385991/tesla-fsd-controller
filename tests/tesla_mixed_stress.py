#!/usr/bin/env python3
"""Mixed Tesla-style traffic stress for the ESP32 FSD Wi-Fi bridge.

Runs alongside tests/fsd_stability_probe.py and deliberately mixes:
  * FSD reply variants (VIN prefixes, trailing slash, query strings)
  * ordinary Tesla cloud endpoints (vehicle_data, commands, connectivity,
    telemetry, navigation, media, software, auth, energy, ...)
  * GET/POST/PUT/DELETE/HEAD/OPTIONS, realistic Tesla headers
  * different SNI values, TLS 1.2/1.3, ALPN h2, no-SNI
  * short concurrent bursts to overflow the single 443 socket
  * malformed TLS/HTTP probes that must be rejected without bricking
  * management-page/API concurrency on 10010
  * DNS A/AAAA/TXT/ANY and malformed queries on 53

A hard failure is only: reboot (uptime reset), device unreachable for several
consecutive health checks, heap below 10 KB, or FSD never recovering after a
burst. Rejected/502/timeout responses under concurrency are expected and are
recorded as soft results.

Run:
  ~/.cache/uv/archive-v0/0DB72nRt2QNkvIPe/bin/python \
      tests/tesla_mixed_stress.py --host 9.9.9.9 --seconds 4000 \
      --out /tmp/tesla-mixed.jsonl
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import http.client
import json
import os
import random
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SNI_MAIN = "api-prd.vn.cloud.tesla.cn"
SNI_ALT = "mothership.vn.teslamotors.com"
FSD_MD5 = "bd262907ff9fb2709bb54f9d3e78d07d"
FSD_LEN = 653
VIN = "5YJ3E1EA7KF000001"

FSD_PATHS = [
    "/feature_configuration",
    "/feature_configuration/",
    f"/feature_configuration?vin={VIN}",
    f"/{VIN}/feature_configuration",
    f"/api/1/vehicles/{VIN}/feature_configuration",
    "/feature_configuration/?x=1",
]
FSD_NEGATIVE = [
    "/FEATURE_CONFIGURATION",
    "/feature_configuration%2F",
    "/feature-configuration",
]
TESLA_PATHS = [
    "/api/1/vehicles",
    f"/api/1/vehicles/{VIN}",
    f"/api/1/vehicles/{VIN}/vehicle_data",
    f"/api/1/vehicles/{VIN}/command/door_unlock",
    f"/api/1/vehicles/{VIN}/command/set_temps",
    f"/api/1/vehicles/{VIN}/command/charge_start",
    f"/api/1/vehicles/{VIN}/wake_up",
    "/connectivity",
    "/odometer",
    "/telemetry",
    "/telemetry/config",
    "/sync",
    "/navigation",
    "/media",
    "/software/update",
    "/auth",
    "/oauth2/token",
    "/health",
    "/version",
    "/mothership",
    "/streaming",
    "/supercharging",
    "/api/1/energy_sites",
    "/powerwall",
    "/fleet",
    "/map",
    "/.well-known/tesla",
    "/api/1/vehicles/" + VIN + "/firmware",
]
METHODS = ["GET", "GET", "GET", "POST", "POST", "PUT", "DELETE"]
MGMT_PATHS = [
    "/", "/fsd", "/client-config", "/manifest.json",
    "/api/status", "/api/wifi-bridge/status", "/api/fsd-mock/payload",
    "/api/fsd-mock/hits", "/api/fsd-mock/handshakes",
    "/api/fsd-mock/config-history", "/api/log", "/api/ota/status",
    "/api/ota/partinfo", "/api/unknown", "/api/status?x=" + "A" * 512,
    "/api/wifi-bridge/status?token=WRONG", "/tesla", "/feature_configuration",
]
TESLA_HEADERS = {
    "User-Agent": "Tesla/2024.44.25.1 (Model 3; Linux) QtCarBrowser",
    "Accept": "application/json",
    "Accept-Encoding": "gzip, deflate",
    "X-Tesla-User-Agent": "Tesla/2024.44.25.1",
    "X-Tesla-Client": "vehicle",
    "X-Tesla-Protocol": "1.0",
    "Cookie": "tesla_auth=probe; vin=" + VIN,
    "Authorization": "Bearer PROBE-TOKEN",
}


class Stress:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.host = args.host
        self.mgmt_port = args.mgmt_port
        self.out_path = Path(args.out)
        self.out_path.parent.mkdir(parents=True, exist_ok=True)
        self.out = self.out_path.open("a", encoding="utf-8")
        self.lock = threading.Lock()
        self.stats: dict = {
            "started": time.time(),
            "tls_ok": 0, "tls_soft": 0, "tls_error": 0,
            "fsd_ok": 0, "fsd_soft": 0,
            "mgmt_ok": 0, "mgmt_soft": 0,
            "dns_ok": 0, "dns_soft": 0,
            "burst_ok": 0, "burst_soft": 0,
            "malformed_ok": 0, "malformed_soft": 0,
            "health_ok": 0, "health_fail": 0,
            "hard_failures": [],
            "consecutive_health_fail": 0,
            "heap_min": None, "last_uptime": None, "reboots": 0,
            "last_heap": None, "last_reset": None,
        }
        self.expected_payload = FSD_LEN
        self.expected_md5 = FSD_MD5
        self.payload_body: bytes | None = None
        self.dns_allow = ""
        self.dns_block = ""
        self.stop = threading.Event()

    def log(self, kind: str, ok: bool | None, **fields) -> None:
        rec = {"ts": round(time.time(), 3), "kind": kind}
        if ok is not None:
            rec["ok"] = ok
        rec.update(fields)
        with self.lock:
            self.out.write(json.dumps(rec, ensure_ascii=False) + "\n")
            self.out.flush()
        if kind in ("hard", "health", "burst", "fsd", "malformed", "summary", "start"):
            print(json.dumps(rec, ensure_ascii=False), flush=True)

    def hard(self, reason: str, **fields) -> None:
        with self.lock:
            self.stats["hard_failures"].append({"ts": round(time.time(), 3), "reason": reason, **fields})
        self.log("hard", False, reason=reason, **fields)

    # ── TLS ──────────────────────────────────────────────────────────────
    def _context(self, tls_version: str | None, alpn: list[str] | None) -> ssl.SSLContext:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        if tls_version == "1.2":
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        elif tls_version == "1.3":
            ctx.minimum_version = ssl.TLSVersion.TLSv1_3
            ctx.maximum_version = ssl.TLSVersion.TLSv1_3
        if alpn:
            ctx.set_alpn_protocols(alpn)
        return ctx

    def tls_request(self, method: str, path: str, *, sni: str | None = SNI_MAIN,
                    body: bytes | None = None, tls_version: str | None = None,
                    alpn: list[str] | None = None, timeout: float = 8.0,
                    extra_headers: dict | None = None, chunked: bool = False) -> dict:
        t0 = time.monotonic()
        tls = None
        try:
            raw = socket.create_connection((self.host, 443), timeout=timeout)
            tls = self._context(tls_version, alpn).wrap_socket(raw, server_hostname=sni)
            conn = http.client.HTTPConnection(self.host, 443, timeout=timeout)
            conn.sock = tls
            headers = dict(TESLA_HEADERS)
            if sni:
                headers["Host"] = sni
            if body:
                headers["Content-Type"] = "application/json"
            if extra_headers:
                headers.update(extra_headers)
            conn.request(method, path, body=body or None, headers=headers, encode_chunked=chunked)
            resp = conn.getresponse()
            data = resp.read()
            ms = round((time.monotonic() - t0) * 1000, 1)
            return {
                "ok": True, "ms": ms, "status": resp.status,
                "bytes": len(data), "ctype": resp.getheader("content-type", ""),
                "md5": hashlib.md5(data).hexdigest() if data else "",
                "tls": tls.version() if tls else "", "alpn": tls.selected_alpn_protocol() if tls else "",
                "body": data[:120].decode("utf-8", "replace"),
            }
        except Exception as exc:
            ms = round((time.monotonic() - t0) * 1000, 1)
            return {"ok": False, "ms": ms, "error": f"{type(exc).__name__}: {exc}"}
        finally:
            try:
                if tls:
                    tls.close()
            except Exception:
                pass

    # ── HTTP management ──────────────────────────────────────────────────
    def mgmt_request(self, path: str, timeout: float = 8.0) -> dict:
        t0 = time.monotonic()
        try:
            req = urllib.request.Request(
                f"http://{self.host}:{self.mgmt_port}{path}",
                headers={"User-Agent": "tesla-mixed-stress", "Accept": "*/*"},
            )
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                body = resp.read()
                hdrs = {k.lower(): v for k, v in resp.headers.items()}
                if hdrs.get("content-encoding") == "gzip":
                    body = gzip.decompress(body)
                return {"ok": True, "status": resp.status, "bytes": len(body),
                        "ms": round((time.monotonic() - t0) * 1000, 1),
                        "body": body[:80].decode("utf-8", "replace")}
        except urllib.error.HTTPError as exc:
            return {"ok": True, "status": exc.code, "bytes": 0,
                    "ms": round((time.monotonic() - t0) * 1000, 1), "body": ""}
        except Exception as exc:
            return {"ok": False, "ms": round((time.monotonic() - t0) * 1000, 1),
                    "error": f"{type(exc).__name__}: {exc}"}

    # ── DNS ──────────────────────────────────────────────────────────────
    @staticmethod
    def _dns_packet(name: str, qtype: int, txid: int) -> bytes:
        qname = b"".join(bytes([len(p)]) + p.encode("ascii") for p in name.strip(".").split(".")) + b"\0"
        return struct.pack(">HHHHHH", txid, 0x0100, 1, 0, 0, 0) + qname + struct.pack(">HH", qtype, 1)

    @staticmethod
    def _parse_dns(data: bytes, txid: int) -> dict:
        if len(data) < 12:
            return {"rcode": None, "answers": [], "error": "short"}
        rid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", data[:12])
        if rid != txid:
            return {"rcode": None, "answers": [], "error": "txid"}
        rcode = flags & 0xF
        offset = 12
        for _ in range(qd):
            while offset < len(data) and data[offset]:
                offset += data[offset] + 1
            offset += 5
        answers = []
        for _ in range(an):
            if offset + 12 > len(data):
                break
            # name (possibly compressed)
            if data[offset] & 0xC0 == 0xC0:
                offset += 2
            else:
                while offset < len(data) and data[offset]:
                    offset += data[offset] + 1
                offset += 1
            rtype, rclass, ttl, rdlen = struct.unpack(">HHIH", data[offset:offset + 10])
            offset += 10
            rdata = data[offset:offset + rdlen]
            offset += rdlen
            if rtype == 1 and rdlen == 4:
                answers.append(socket.inet_ntoa(rdata))
            else:
                answers.append(f"type{rtype}:{rdata[:16].hex()}")
        return {"rcode": rcode, "answers": answers, "error": ""}

    def dns_query(self, name: str, qtype: int = 1, timeout: float = 3.0,
                  raw: bytes | None = None) -> dict:
        txid = random.randint(0, 65535)
        packet = raw if raw is not None else self._dns_packet(name, qtype, txid)
        t0 = time.monotonic()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(timeout)
        try:
            sock.sendto(packet, (self.host, 53))
            data, _ = sock.recvfrom(2048)
            parsed = self._parse_dns(data, txid)
            parsed.update({"ok": True, "ms": round((time.monotonic() - t0) * 1000, 1),
                           "name": name, "qtype": qtype})
            return parsed
        except Exception as exc:
            return {"ok": False, "ms": round((time.monotonic() - t0) * 1000, 1),
                    "name": name, "qtype": qtype, "error": f"{type(exc).__name__}: {exc}"}
        finally:
            sock.close()

    # ── health ───────────────────────────────────────────────────────────
    def health(self, tag: str = "health") -> dict | None:
        res = self.mgmt_request("/api/wifi-bridge/status")
        if not res.get("ok") or res.get("status") != 200:
            self.stats["consecutive_health_fail"] += 1
            self.stats["health_fail"] += 1
            if self.stats["consecutive_health_fail"] in (3, 6, 10):
                self.hard("device unreachable", consecutive=self.stats["consecutive_health_fail"])
            self.log("health", False, status=res.get("status"), error=res.get("error"),
                     consecutive=self.stats["consecutive_health_fail"])
            return None
        self.stats["consecutive_health_fail"] = 0
        self.stats["health_ok"] += 1
        # Re-read the body because mgmt_request truncates it for logging.
        try:
            with urllib.request.urlopen(f"http://{self.host}:{self.mgmt_port}/api/wifi-bridge/status", timeout=8) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except Exception:
            data = {}
        heap = data.get("heap")
        uptime = data.get("uptime")
        reset = data.get("resetReason")
        if isinstance(heap, int):
            self.stats["last_heap"] = heap
            self.stats["heap_min"] = heap if self.stats["heap_min"] is None else min(self.stats["heap_min"], heap)
            if heap < 10000:
                self.hard("heap below 10 KB", heap=heap)
        if isinstance(uptime, int):
            if self.stats["last_uptime"] is not None and uptime < self.stats["last_uptime"]:
                self.stats["reboots"] += 1
                self.hard("uptime reset (reboot)", uptime=uptime, previous=self.stats["last_uptime"], reset=reset)
            self.stats["last_uptime"] = uptime
        if reset:
            self.stats["last_reset"] = reset
        fsd = data.get("fsdMock") or {}
        self.log(tag, True, heap=heap, uptime=uptime, reset=reset,
                 fsd={k: fsd.get(k) for k in ("enabled", "mode", "bytes", "md5", "hits", "observes")},
                 handshake=fsd.get("handshake"))
        return data

    # ── request classification ───────────────────────────────────────────
    @staticmethod
    def is_fsd_path(path: str) -> bool:
        return "/feature_configuration" in path and not path.startswith("/FEATURE")

    def run_tls(self, method: str, path: str, *, sni: str | None = SNI_MAIN,
                tls_version: str | None = None, alpn: list[str] | None = None,
                body: bytes | None = None, tag: str = "tls", extra_headers: dict | None = None,
                chunked: bool = False, concurrent: bool = False) -> bool:
        res = self.tls_request(method, path, sni=sni, body=body, tls_version=tls_version,
                               alpn=alpn, extra_headers=extra_headers, chunked=chunked)
        # Only a plain HTTP/1.1 TLS request with the real SNI is expected to
        # return the 653-byte FSD payload. HEAD, h2-ALPN, TLS1.3 and alternate
        # SNI probes are edge cases whose rejection is allowed.
        fsd_expected = (self.is_fsd_path(path) and sni == SNI_MAIN and
                        tls_version != "1.3" and alpn is None and method != "HEAD")
        problems = []
        if not res.get("ok"):
            problems.append(res.get("error", "connect failed"))
        elif fsd_expected:
            if res.get("status") != 200:
                problems.append(f"fsd status={res.get('status')}")
            if res.get("bytes") != self.expected_payload:
                problems.append(f"fsd len={res.get('bytes')}")
            if res.get("md5") != self.expected_md5:
                problems.append(f"fsd md5={res.get('md5')}")
        else:
            # Any HTTP response is fine for non-FSD / negative-path probes.
            if not (100 <= (res.get("status") or 0) <= 599):
                problems.append(f"status={res.get('status')}")
        # Inside a concurrent burst the device intentionally keeps only one
        # 443 socket, so remote disconnects / 502s are expected backpressure,
        # not a firmware failure.
        soft = bool(problems) and (not fsd_expected or concurrent)
        ok = not problems
        bucket = "ok" if ok else ("soft" if soft else "error")
        self.stats[f"{tag}_{bucket}"] = self.stats.get(f"{tag}_{bucket}", 0) + 1
        if fsd_expected:
            self.stats[f"fsd_{bucket}"] = self.stats.get(f"fsd_{bucket}", 0) + 1
        self.log(tag, ok, fsd=fsd_expected, method=method, path=path, sni=sni,
                 tlsver=tls_version, soft=soft, concurrent=concurrent,
                 **{k: v for k, v in res.items() if k != "ok"})
        return ok

    def check_payload(self) -> None:
        try:
            res = self.mgmt_request("/api/fsd-mock/payload")
            if not res.get("ok") or res.get("status") != 200:
                self.log("payload", False, error=res.get("error"), status=res.get("status"))
                return
            req = urllib.request.Request(
                f"http://{self.host}:{self.mgmt_port}/api/fsd-mock/payload",
                data=self.payload_body, method="POST",
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=10) as resp:
                status = resp.status
            if status != 200:
                self.log("payload", False, status=status)
                return
            self.log("payload", True, bytes=len(self.payload_body or b""),
                     md5=hashlib.md5(self.payload_body or b"").hexdigest())
        except Exception as exc:
            self.log("payload", False, error=f"{type(exc).__name__}: {exc}")

    # ── scheduled actions ────────────────────────────────────────────────
    def basic_tls(self) -> None:
        roll = random.random()
        if roll < 0.35:
            path = random.choice(FSD_PATHS)
            self.run_tls(random.choice(["POST", "GET"]), path, body=b"{}", tag="tls")
        elif roll < 0.45:
            path = random.choice(FSD_NEGATIVE)
            self.run_tls(random.choice(["GET", "POST"]), path, body=b"{}", tag="tls")
        else:
            path = random.choice(TESLA_PATHS)
            method = random.choice(METHODS)
            body = b'{"vin":"' + VIN.encode() + b'"}' if method in ("POST", "PUT") else None
            self.run_tls(method, path, body=body, tag="tls")

    def mgmt_basic(self) -> None:
        path = random.choice(MGMT_PATHS)
        res = self.mgmt_request(path)
        ok = res.get("status") in (200, 204, 400, 401, 403, 404, 405, 413)
        self.stats["mgmt_ok" if ok else "mgmt_soft"] += 1
        self.log("mgmt", ok, path=path, soft=not ok,
                 **{k: v for k, v in res.items() if k != "ok"})

    def dns_basic(self) -> None:
        choices = [
            (SNI_MAIN, 1, "hijack"),
            (SNI_MAIN, 28, "empty-aaaa"),
            (SNI_MAIN, 16, "txt"),
            ("connman.vn.cloud.tesla.cn", 1, "allowed-forward"),
            ("tesla.com", 1, "blocked"),
            (f"random-{random.randrange(1 << 30):x}.example.com", 1, "unknown"),
        ]
        name, qtype, kind = random.choice(choices)
        res = self.dns_query(name, qtype)
        if not res.get("ok"):
            # Forwarded queries legitimately time out while upstream is off.
            # A hijacked-name timeout is a soft failure too (UDP can drop).
            self.stats["dns_soft"] += 1
            self.log("dns", False, qkind=kind, soft=True,
                     **{k: v for k, v in res.items() if k != "ok"})
            return
        answers = res.get("answers") or []
        if kind == "hijack":
            ok = self.host in answers
            if not ok:
                self.hard("DNS hijack broken", answers=answers, rcode=res.get("rcode"))
        elif kind == "blocked":
            ok = answers == ["0.0.0.0"] or res.get("rcode") == 3 or not answers
        else:
            ok = res.get("rcode") in (0, 2, 3, 5) or bool(answers)
        self.stats["dns_ok" if ok else "dns_soft"] += 1
        self.log("dns", ok, qkind=kind, answers=answers, rcode=res.get("rcode"),
                 soft=not ok, ms=res.get("ms"))

    def burst(self) -> None:
        jobs = [
            ("POST", FSD_PATHS[0]), ("POST", FSD_PATHS[1]),
            ("POST", FSD_PATHS[3]), ("GET", TESLA_PATHS[0]),
            ("POST", TESLA_PATHS[2]), ("GET", TESLA_PATHS[11]),
        ]
        with ThreadPoolExecutor(max_workers=len(jobs)) as pool:
            futs = [pool.submit(self.run_tls, m, p, tag="burst", body=b"{}",
                                concurrent=True) for m, p in jobs]
            results = [f.result() for f in futs]
        ok_count = sum(1 for r in results if r)
        self.log("burst", ok_count > 0, ok_count=ok_count, total=len(results))
        if ok_count == 0:
            self.stats["burst_soft"] += 1
        else:
            self.stats["burst_ok"] += 1
        time.sleep(2)
        # Recovery: after a concurrent burst, a single canonical FSD request
        # and a health poll must succeed. Retry three times before declaring a
        # hard failure (the device intentionally keeps one TLS socket).
        for attempt in range(3):
            if self.run_tls("POST", FSD_PATHS[0], tag="fsd", body=b"{}"):
                self.log("burst", True, note="FSD recovered", attempt=attempt + 1)
                self.health("health")
                return
            time.sleep(2)
        self.hard("FSD did not recover after burst")
        self.health("health")

    def mgmt_burst(self) -> None:
        paths = ["/", "/fsd", "/api/status", "/api/wifi-bridge/status"]
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(self.mgmt_request, paths))
        ok = sum(1 for r in results if r.get("status") in (200, 204, 403, 404))
        self.log("mgmt_burst", ok >= 2, ok_count=ok, total=len(results),
                 statuses=[r.get("status") for r in results])

    def malformed(self) -> None:
        kind = random.choice(["plaintext", "partial_tls", "huge_header", "http10",
                              "options", "head", "chunked", "tls13", "alpn_h2", "no_sni"])
        soft = kind in ("huge_header", "tls13", "alpn_h2", "no_sni")
        counted = False
        try:
            if kind == "plaintext":
                s = socket.create_connection((self.host, 443), timeout=5)
                s.sendall(b"GET / HTTP/1.1\r\nHost: " + SNI_MAIN.encode() + b"\r\n\r\n")
                s.settimeout(2)
                try:
                    s.recv(64)
                except Exception:
                    pass
                s.close()
                self.log("malformed", True, mkind=kind, note="plaintext rejected/closed")
            elif kind == "partial_tls":
                raw = socket.create_connection((self.host, 443), timeout=5)
                tls = self._context(None, None).wrap_socket(raw, server_hostname=SNI_MAIN)
                tls.sendall(b"GET /feature_configuration HTTP/1.1\r\nHost: ")
                tls.close()
                self.log("malformed", True, mkind=kind, note="partial HTTP closed")
            elif kind == "huge_header":
                extra = {"X-Filler": "B" * 4096}
                res = self.tls_request("GET", "/connectivity", extra_headers=extra, timeout=8)
                self.log("malformed", True, mkind=kind, soft=not res.get("ok"),
                         status=res.get("status"), error=res.get("error"))
            elif kind == "http10":
                raw = socket.create_connection((self.host, 443), timeout=5)
                tls = self._context("1.2", None).wrap_socket(raw, server_hostname=SNI_MAIN)
                tls.sendall(b"GET /feature_configuration HTTP/1.0\r\n\r\n")
                data = tls.recv(128)
                tls.close()
                self.log("malformed", True, mkind=kind, bytes=len(data),
                         head=data[:48].decode("latin1", "replace"))
            elif kind == "options":
                self.run_tls("OPTIONS", "*", tag="malformed")
                counted = True
            elif kind == "head":
                self.run_tls("HEAD", "/feature_configuration", tag="malformed")
                counted = True
            elif kind == "chunked":
                self.run_tls("POST", FSD_PATHS[0], body=[b"{", b"}"], tag="malformed", chunked=True)
                counted = True
            elif kind == "tls13":
                res = self.tls_request("GET", "/feature_configuration", tls_version="1.3")
                self.log("malformed", True, mkind=kind, soft=True,
                         status=res.get("status"), error=res.get("error"))
            elif kind == "alpn_h2":
                res = self.tls_request("GET", "/feature_configuration", alpn=["h2"])
                self.log("malformed", True, mkind=kind, soft=True,
                         status=res.get("status"), alpn=res.get("alpn"), error=res.get("error"))
            elif kind == "no_sni":
                res = self.tls_request("GET", "/feature_configuration", sni=None)
                self.log("malformed", True, mkind=kind, soft=True,
                         status=res.get("status"), error=res.get("error"))
        except Exception as exc:
            soft = True
            self.log("malformed", True, mkind=kind, soft=True,
                     error=f"{type(exc).__name__}: {exc}")
        finally:
            if not counted:
                self.stats["malformed_soft" if soft else "malformed_ok"] += 1

    # ── main ─────────────────────────────────────────────────────────────
    def run(self) -> int:
        self.log("start", None, note="mixed stress starting", host=self.host,
                 seconds=self.args.seconds, seed=self.args.seed)
        random.seed(self.args.seed)
        # Read the current payload and DNS rules once.
        try:
            res = self.mgmt_request("/api/fsd-mock/payload")
            req = urllib.request.Request(f"http://{self.host}:{self.mgmt_port}/api/fsd-mock/payload")
            with urllib.request.urlopen(req, timeout=8) as resp:
                self.payload_body = resp.read()
                obj = json.loads(self.payload_body)
                self.expected_payload = len(self.payload_body)
                self.expected_md5 = hashlib.md5(self.payload_body).hexdigest()
                self.log("payload", True, bytes=self.expected_payload,
                         md5=self.expected_md5, fields=len(obj))
        except Exception as exc:
            self.log("payload", None, note=f"payload read failed: {exc}")
        self.health("start")
        start = time.monotonic()
        next_tls = start
        next_mgmt = start + 1
        next_dns = start + 2
        next_health = start + 5
        next_burst = start + 20
        next_mgmt_burst = start + 40
        next_malformed = start + 30
        next_payload = start + 1800
        try:
            while not self.stop.is_set() and time.monotonic() - start < self.args.seconds:
                now = time.monotonic()
                if now >= next_tls:
                    self.basic_tls()
                    next_tls = now + self.args.tls_interval
                if now >= next_mgmt:
                    self.mgmt_basic()
                    next_mgmt = now + self.args.mgmt_interval
                if now >= next_dns:
                    self.dns_basic()
                    next_dns = now + self.args.dns_interval
                if now >= next_health:
                    self.health()
                    next_health = now + self.args.health_interval
                if now >= next_burst:
                    self.burst()
                    next_burst = now + self.args.burst_interval
                if now >= next_mgmt_burst:
                    self.mgmt_burst()
                    next_mgmt_burst = now + self.args.mgmt_burst_interval
                if now >= next_malformed:
                    self.malformed()
                    next_malformed = now + self.args.malformed_interval
                if now >= next_payload:
                    self.check_payload()
                    next_payload = now + self.args.payload_interval
                time.sleep(0.2)
        except KeyboardInterrupt:
            self.log("start", None, note="interrupted")
        finally:
            time.sleep(1)
            # One last canonical FSD + health check after all chaos. Retry so a
            # collision with the concurrently running stability probe is not
            # mistaken for a dead FSD server.
            recovered = False
            for attempt in range(6):
                if self.run_tls("POST", FSD_PATHS[0], tag="fsd", body=b"{}"):
                    recovered = True
                    break
                time.sleep(2)
            self.health("final")
            self.log("summary", None, **self.summary(recovered))
            self.out.close()
            Path(str(self.out_path).replace(".jsonl", ".summary.json")).write_text(
                json.dumps(self.summary(recovered), ensure_ascii=False, indent=2), encoding="utf-8")
        return 0 if not self.stats["hard_failures"] and recovered else 1

    def summary(self, recovered: bool = False) -> dict:
        s = {k: v for k, v in self.stats.items() if k != "hard_failures"}
        s["hard_failure_count"] = len(self.stats["hard_failures"])
        s["hard_failures_tail"] = self.stats["hard_failures"][-20:]
        s["fsd_recovered_at_end"] = recovered
        s["elapsed_seconds"] = round(time.time() - self.stats["started"], 1)
        s["jsonl"] = str(self.out_path)
        return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="9.9.9.9")
    ap.add_argument("--mgmt-port", type=int, default=10010)
    ap.add_argument("--seconds", type=int, default=4000)
    ap.add_argument("--tls-interval", type=float, default=2.0)
    ap.add_argument("--mgmt-interval", type=float, default=8.0)
    ap.add_argument("--dns-interval", type=float, default=10.0)
    ap.add_argument("--health-interval", type=float, default=20.0)
    ap.add_argument("--burst-interval", type=float, default=120.0)
    ap.add_argument("--mgmt-burst-interval", type=float, default=300.0)
    ap.add_argument("--malformed-interval", type=float, default=180.0)
    ap.add_argument("--payload-interval", type=float, default=1800.0)
    ap.add_argument("--seed", type=int, default=20261122)
    ap.add_argument("--out", default=f"tesla-mixed-{time.strftime('%Y%m%d-%H%M%S')}.jsonl")
    args = ap.parse_args()
    return Stress(args).run()


if __name__ == "__main__":
    sys.exit(main())
