#!/usr/bin/env python3
"""Long-running FSD delivery + management stability probe.

Exercises the device the way a car and a phone would:
  * TLS POST /feature_configuration on 443 (FSD mock reply), verifying payload,
    length, X-Checksum and field order every --fsd-interval seconds.
  * Periodic non-FSD TLS request to confirm the proxy path fails fast and does
    not poison the next FSD request.
  * HTTP GET /, /fsd, /client-config, /manifest.json, /api/status and
    /api/wifi-bridge/status to catch web-server/heap regressions.
  * Optional serial capture: sends `wifi-diag` periodically and records output,
    including reset reason and heap.

Every event is appended to a JSONL file; a summary JSON is written at the end.
Run it with the PlatformIO/uv Python so pyserial is available:

  ~/.cache/uv/archive-v0/0DB72nRt2QNkvIPe/bin/python \
      tests/fsd_stability_probe.py --host 9.9.9.9 --seconds 3900 \
      --serial /dev/cu.usbmodem21201

Exit code is non-zero if any hard check failed.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import http.client
import json
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

CANONICAL_LEN = 653
CANONICAL_MD5 = "bd262907ff9fb2709bb54f9d3e78d07d"
CANONICAL_FIELDS = 22
SNI = "api-prd.vn.cloud.tesla.cn"
FSD_PATH = "/feature_configuration"


class Probe:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.host = args.host
        self.mgmt = args.mgmt_port
        self.token = args.token or ""
        self.ssl_ctx = ssl.create_default_context()
        self.ssl_ctx.check_hostname = False
        self.ssl_ctx.verify_mode = ssl.CERT_NONE
        self.out_path = Path(args.out)
        self.out_path.parent.mkdir(parents=True, exist_ok=True)
        self.out = self.out_path.open("a", encoding="utf-8")
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.serial = None
        self.serial_thread = None
        self.serial_path = Path(str(self.out_path) + ".serial.log")
        self.stats = {
            "started": time.time(),
            "fsd_ok": 0,
            "fsd_fail": 0,
            "proxy_ok": 0,
            "proxy_fail": 0,
            "page_ok": 0,
            "page_fail": 0,
            "status_ok": 0,
            "status_fail": 0,
            "burst_ok": 0,
            "burst_fail": 0,
            "reboots": 0,
            "heap_min": None,
            "heap_max": None,
            "min_heap_min": None,
            "last_uptime": None,
            "last_reset": None,
            "last_status": {},
            "failures": [],
        }
        self.expected_len = CANONICAL_LEN
        self.expected_md5 = CANONICAL_MD5
        self.expected_fields = CANONICAL_FIELDS
        self.expected_body = None
        self.status_url = f"http://{self.host}:{self.mgmt}/api/wifi-bridge/status"
        self.status_fallback = f"http://{self.host}:{self.mgmt}/api/status"
        self.token_ok = bool(self.token)

    # ── logging ──────────────────────────────────────────────────────────
    def log(self, kind: str, ok: bool | None, **fields) -> None:
        rec = {"ts": round(time.time(), 3), "kind": kind}
        if ok is not None:
            rec["ok"] = ok
        rec.update(fields)
        with self.lock:
            self.out.write(json.dumps(rec, ensure_ascii=False) + "\n")
            self.out.flush()
        if kind in ("fsd", "burst", "reboot", "summary", "start", "payload", "error"):
            print(json.dumps(rec, ensure_ascii=False), flush=True)

    def fail(self, kind: str, reason: str, **fields) -> None:
        with self.lock:
            self.stats["failures"].append(
                {"ts": round(time.time(), 3), "kind": kind, "reason": reason, **fields}
            )
        self.log(kind, False, reason=reason, **fields)

    # ── HTTP ─────────────────────────────────────────────────────────────
    def _url(self, path: str) -> str:
        url = f"http://{self.host}:{self.mgmt}{path}"
        if self.token:
            sep = "&" if "?" in url else "?"
            url += sep + "token=" + urllib.parse.quote(self.token)
        return url

    def http_get(self, path: str, timeout: float = 10.0) -> tuple[int, bytes, dict]:
        req = urllib.request.Request(self._url(path), headers={"User-Agent": "fsd-stability-probe"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            headers = {k.lower(): v for k, v in resp.headers.items()}
            if headers.get("content-encoding") == "gzip":
                body = gzip.decompress(body)
            return resp.status, body, headers

    def poll_status(self) -> dict | None:
        for url in (self.status_url, self.status_fallback):
            try:
                req = urllib.request.Request(
                    url + (("&" if "?" in url else "?") + "token=" + urllib.parse.quote(self.token) if self.token else ""),
                    headers={"User-Agent": "fsd-stability-probe"},
                )
                with urllib.request.urlopen(req, timeout=10) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                self.stats["status_ok"] += 1
                self._track_status(data, url.endswith("/api/status"))
                return data
            except urllib.error.HTTPError as exc:
                if exc.code == 403 and self.token:
                    self.token_ok = False
                continue
            except Exception:
                continue
        self.stats["status_fail"] += 1
        self.fail("status", "all status endpoints failed")
        return None

    def _track_status(self, data: dict, fallback: bool) -> None:
        fsd = data.get("fsdMock") or {}
        heap = data.get("heap", data.get("freeHeap"))
        min_heap = data.get("minHeap")
        uptime = data.get("uptime")
        reset = data.get("resetReason")
        if isinstance(heap, int):
            self.stats["heap_min"] = heap if self.stats["heap_min"] is None else min(self.stats["heap_min"], heap)
            self.stats["heap_max"] = heap if self.stats["heap_max"] is None else max(self.stats["heap_max"], heap)
        if isinstance(min_heap, int):
            self.stats["min_heap_min"] = min_heap if self.stats["min_heap_min"] is None else min(self.stats["min_heap_min"], min_heap)
        if isinstance(uptime, int) and self.stats["last_uptime"] is not None and uptime < self.stats["last_uptime"]:
            self.stats["reboots"] += 1
            self.log("reboot", False, reason="uptime went backwards", uptime=uptime,
                     previous_uptime=self.stats["last_uptime"], reset=reset)
        if reset and self.stats["last_reset"] and reset != self.stats["last_reset"]:
            self.stats["reboots"] += 1
            self.log("reboot", False, reason="reset reason changed", reset=reset,
                     previous_reset=self.stats["last_reset"])
        if isinstance(uptime, int):
            self.stats["last_uptime"] = uptime
        if reset:
            self.stats["last_reset"] = reset
        self.stats["last_status"] = {
            "heap": heap, "minHeap": min_heap, "uptime": uptime, "resetReason": reset,
            "fsdMock": {k: fsd.get(k) for k in ("enabled", "mode", "bytes", "fields", "md5", "hits", "observes", "flows", "dnsRedirects")},
            "handshake": fsd.get("handshake"),
            "statusSource": "fallback" if fallback else "bridge",
        }
        self.log("status", True, **self.stats["last_status"])

    # ── TLS FSD ──────────────────────────────────────────────────────────
    def tls_exchange(self, method: str, path: str, body: bytes, timeout: float = 12.0) -> tuple[int, dict, bytes]:
        raw = socket.create_connection((self.host, 443), timeout=timeout)
        tls = self.ssl_ctx.wrap_socket(raw, server_hostname=SNI)
        try:
            conn = http.client.HTTPConnection(self.host, 443, timeout=timeout)
            conn.sock = tls
            headers = {
                "Host": SNI,
                "User-Agent": "fsd-stability-probe/1.0",
                "Accept": "application/json",
                "Content-Type": "application/json",
                "Connection": "close",
            }
            if body:
                headers["Content-Length"] = str(len(body))
            conn.request(method, path, body=body or None, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            return resp.status, {k.lower(): v for k, v in resp.getheaders()}, data
        finally:
            tls.close()

    def initial_payload(self) -> None:
        try:
            status, body, _ = self.http_get("/api/fsd-mock/payload")
            if status == 200:
                text = body.decode("utf-8", "replace")
                obj = json.loads(text)
                self.expected_body = text.encode("utf-8")
                self.expected_len = len(self.expected_body)
                self.expected_md5 = hashlib.md5(self.expected_body).hexdigest()
                self.expected_fields = len(obj)
                self.log("payload", True, bytes=self.expected_len, md5=self.expected_md5,
                         fields=self.expected_fields, first_key=next(iter(obj), None),
                         last_key=list(obj)[-1] if obj else None)
            else:
                self.fail("payload", f"GET payload returned {status}")
        except Exception as exc:
            self.log("payload", None, note=f"payload fetch unavailable: {exc}",
                     expected_bytes=self.expected_len, expected_md5=self.expected_md5)

    def check_fsd(self, tag: str = "fsd") -> bool:
        t0 = time.monotonic()
        try:
            status, headers, body = self.tls_exchange("POST", FSD_PATH, b"{}")
            ms = round((time.monotonic() - t0) * 1000, 1)
            md5 = hashlib.md5(body).hexdigest()
            checksum = headers.get("x-checksum", "")
            ctype = headers.get("content-type", "")
            problems = []
            if status != 200:
                problems.append(f"status={status}")
            if len(body) != self.expected_len:
                problems.append(f"len={len(body)} expected={self.expected_len}")
            if md5 != self.expected_md5:
                problems.append(f"md5={md5} expected={self.expected_md5}")
            if checksum.lower() != md5.lower():
                problems.append(f"x-checksum={checksum!r} md5={md5}")
            if "application/json" not in ctype.lower():
                problems.append(f"ctype={ctype!r}")
            if body:
                try:
                    obj = json.loads(body)
                    if len(obj) != self.expected_fields:
                        problems.append(f"fields={len(obj)} expected={self.expected_fields}")
                    if obj.get("disable_cabin_camera") is not False:
                        problems.append("disable_cabin_camera!=false")
                    if obj.get("enable_eu_fsd_behavior") is not False:
                        problems.append("enable_eu_fsd_behavior!=false")
                    if obj.get("autoshift_dr") is not True:
                        problems.append("autoshift_dr!=true")
                    if list(obj)[-1] != "autoshift_dr":
                        problems.append("autoshift_dr not last")
                except Exception as exc:
                    problems.append(f"json={exc}")
            ok = not problems
            if ok:
                self.stats[f"{tag}_ok"] += 1
                self.log(tag, True, ms=ms, status=status, bytes=len(body), md5=md5)
            else:
                self.stats[f"{tag}_fail"] += 1
                self.fail(tag, "; ".join(problems), ms=ms, status=status,
                          bytes=len(body), md5=md5, body=body[:160].decode("utf-8", "replace"))
            return ok
        except Exception as exc:
            ms = round((time.monotonic() - t0) * 1000, 1)
            self.stats[f"{tag}_fail"] += 1
            self.fail(tag, f"{type(exc).__name__}: {exc}", ms=ms)
            return False

    def check_proxy(self) -> None:
        t0 = time.monotonic()
        try:
            status, headers, body = self.tls_exchange("POST", "/stability-probe-nonexistent", b"{}")
            ms = round((time.monotonic() - t0) * 1000, 1)
            # 502 upstream_unavailable is the expected AP-only result; 400 is
            # Tesla's real mTLS answer when upstream is reachable.
            ok = status in (200, 400, 404, 502)
            if ok:
                self.stats["proxy_ok"] += 1
                self.log("proxy", True, ms=ms, status=status, bytes=len(body),
                         body=body[:120].decode("utf-8", "replace"))
            else:
                self.stats["proxy_fail"] += 1
                self.fail("proxy", f"unexpected status={status}", ms=ms,
                          body=body[:160].decode("utf-8", "replace"))
        except Exception as exc:
            ms = round((time.monotonic() - t0) * 1000, 1)
            self.stats["proxy_fail"] += 1
            self.fail("proxy", f"{type(exc).__name__}: {exc}", ms=ms)

    def check_pages(self) -> None:
        checks = {
            "/": ("FSD Wi-Fi Bridge", 200),
            "/fsd": ("FSD", 200),
            "/client-config": ("client-config", 200),
            "/manifest.json": ("display", 200),
            "/api/log": ("", 200),
        }
        for path, (needle, want) in checks.items():
            t0 = time.monotonic()
            try:
                status, body, headers = self.http_get(path, timeout=15)
                ms = round((time.monotonic() - t0) * 1000, 1)
                text = body.decode("utf-8", "replace")
                problems = []
                if status != want:
                    problems.append(f"status={status}")
                if len(body) < 100:
                    problems.append(f"body={len(body)}B")
                if needle and needle.lower() not in text.lower():
                    problems.append(f"missing {needle!r}")
                if problems:
                    self.stats["page_fail"] += 1
                    self.fail("page", "; ".join(problems), path=path, ms=ms, bytes=len(body))
                else:
                    self.stats["page_ok"] += 1
                    self.log("page", True, path=path, ms=ms, bytes=len(body),
                             ctype=headers.get("content-type", ""))
            except Exception as exc:
                ms = round((time.monotonic() - t0) * 1000, 1)
                self.stats["page_fail"] += 1
                self.fail("page", f"{type(exc).__name__}: {exc}", path=path, ms=ms)

    def check_payload_roundtrip(self) -> None:
        """POST the current payload back, then re-read it to exercise SPIFFS."""
        if self.expected_body is None:
            return
        try:
            req = urllib.request.Request(
                self._url("/api/fsd-mock/payload"),
                data=self.expected_body,
                method="POST",
                headers={"Content-Type": "application/json", "User-Agent": "fsd-stability-probe"},
            )
            with urllib.request.urlopen(req, timeout=15) as resp:
                status = resp.status
                text = resp.read().decode("utf-8", "replace")
            if status != 200:
                self.fail("payload", f"POST returned {status}: {text[:80]}")
                return
            status, body, _ = self.http_get("/api/fsd-mock/payload")
            md5 = hashlib.md5(body).hexdigest()
            if body != self.expected_body:
                self.fail("payload", "round-trip payload changed", bytes=len(body), md5=md5)
            else:
                self.log("payload", True, note="round-trip OK", bytes=len(body), md5=md5)
        except Exception as exc:
            self.fail("payload", f"round-trip {type(exc).__name__}: {exc}")

    # ── serial ───────────────────────────────────────────────────────────
    def start_serial(self) -> None:
        if not self.args.serial:
            return
        try:
            import serial  # type: ignore
        except Exception as exc:
            self.log("error", None, note=f"pyserial unavailable: {exc}")
            return
        try:
            ser = serial.Serial(self.args.serial, 115200, timeout=0.5)
            ser.dtr = False
            ser.rts = False
            self.serial = ser
            self.serial_fh = self.serial_path.open("a", encoding="utf-8")
            self.serial_thread = threading.Thread(target=self._serial_loop, daemon=True)
            self.serial_thread.start()
            self.log("start", None, note="serial capture started", port=self.args.serial)
        except Exception as exc:
            self.log("error", None, note=f"serial open failed: {exc}")

    def _serial_loop(self) -> None:
        last_cmd = 0.0
        buf = b""
        while not self.stop.is_set():
            try:
                chunk = self.serial.read(4096)
            except Exception:
                time.sleep(0.5)
                continue
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    if text:
                        stamp = time.strftime("%H:%M:%S")
                        self.serial_fh.write(f"[{stamp}] {text}\n")
                        self.serial_fh.flush()
            now = time.monotonic()
            if now - last_cmd >= self.args.serial_interval:
                last_cmd = now
                try:
                    self.serial.write(b"wifi-diag\n")
                except Exception:
                    pass

    # ── main ─────────────────────────────────────────────────────────────
    def run(self) -> int:
        self.log("start", None, note="probe starting", host=self.host, mgmt_port=self.mgmt,
                 seconds=self.args.seconds, token=self.token_ok)
        self.initial_payload()
        self.start_serial()
        self.poll_status()

        start = time.monotonic()
        next_fsd = start
        next_status = start
        next_page = start + 15
        next_proxy = start + 60
        next_payload = start + 1800
        next_burst = start + self.args.burst_every

        try:
            while not self.stop.is_set():
                now = time.monotonic()
                if now - start >= self.args.seconds:
                    break
                if now >= next_fsd:
                    self.check_fsd()
                    next_fsd = now + self.args.fsd_interval
                if now >= next_status:
                    self.poll_status()
                    next_status = now + self.args.http_interval
                if now >= next_page:
                    self.check_pages()
                    next_page = now + self.args.page_interval
                if now >= next_proxy:
                    self.check_proxy()
                    next_proxy = now + self.args.proxy_interval
                if now >= next_payload:
                    self.check_payload_roundtrip()
                    next_payload = now + self.args.payload_interval
                if now >= next_burst:
                    for _ in range(self.args.burst_count):
                        if self.stop.is_set() or time.monotonic() - start >= self.args.seconds:
                            break
                        self.check_fsd(tag="burst")
                        time.sleep(0.8)
                    next_burst = time.monotonic() + self.args.burst_every
                time.sleep(0.5)
        except KeyboardInterrupt:
            self.log("error", None, note="interrupted")
        finally:
            self.stop.set()
            time.sleep(0.6)
            if self.serial:
                try:
                    self.serial.close()
                except Exception:
                    pass
            summary = self.summary()
            self.log("summary", None, **summary)
            self.out.close()
            Path(str(self.out_path).replace(".jsonl", ".summary.json")).write_text(
                json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
            )
            print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
        return 0 if not self.stats["failures"] else 1

    def summary(self) -> dict:
        elapsed = round(time.time() - self.stats["started"], 1)
        s = {k: v for k, v in self.stats.items() if k != "failures"}
        s["elapsed_seconds"] = elapsed
        s["failure_count"] = len(self.stats["failures"])
        s["failures_tail"] = self.stats["failures"][-20:]
        s["jsonl"] = str(self.out_path)
        if self.stats["last_status"].get("fsdMock"):
            s["final_fsd"] = self.stats["last_status"]["fsdMock"]
            s["final_handshake"] = self.stats["last_status"].get("handshake")
        return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="9.9.9.9")
    ap.add_argument("--mgmt-port", type=int, default=10010)
    ap.add_argument("--token", default="")
    ap.add_argument("--seconds", type=int, default=3900, help="test duration (default 65 min)")
    ap.add_argument("--fsd-interval", type=float, default=20.0)
    ap.add_argument("--http-interval", type=float, default=30.0)
    ap.add_argument("--page-interval", type=float, default=300.0)
    ap.add_argument("--proxy-interval", type=float, default=600.0)
    ap.add_argument("--payload-interval", type=float, default=1800.0)
    ap.add_argument("--burst-every", type=float, default=900.0)
    ap.add_argument("--burst-count", type=int, default=5)
    ap.add_argument("--serial", default="")
    ap.add_argument("--serial-interval", type=float, default=120.0)
    ap.add_argument("--out", default=f"probe-fsd-{time.strftime('%Y%m%d-%H%M%S')}.jsonl")
    args = ap.parse_args()
    probe = Probe(args)
    return probe.run()


if __name__ == "__main__":
    sys.exit(main())
