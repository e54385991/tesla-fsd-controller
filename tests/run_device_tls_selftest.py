#!/usr/bin/env python3
"""Run the on-device TLS matrix over the USB serial console.

The firmware command `fsd-selftest` exercises TLS 1.2, TLS 1.3, default
negotiation, SNI, keep-alive, repeated handshakes, the 443 proxy flow and three
concurrent TLS sockets. This runner sends the command and fails on any line
containing `[fsd-selftest] ... FAIL`.

pyserial is not part of the firmware build. Run with uv:

    uv run --with pyserial tests/run_device_tls_selftest.py \
        --port /dev/cu.usbmodem21201

Or install pyserial into the current Python environment.
"""
from __future__ import annotations

import argparse
import glob
import re
import sys
import time

try:
    import serial  # type: ignore
except ImportError:
    print("pyserial is required: uv run --with pyserial tests/run_device_tls_selftest.py ...",
          file=sys.stderr)
    raise SystemExit(2)

DONE_RE = re.compile(r"\[fsd-selftest\] done pass=(\d+)/(\d+)")
FAIL_RE = re.compile(r"\[fsd-selftest\].*\bFAIL\b")


def autodetect_port() -> str | None:
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyUSB*", "/dev/ttyACM*"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None,
                        help="serial port; auto-detects /dev/cu.usbmodem*")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--diag", action="store_true",
                        help="send wifi-diag before and after the selftest")
    args = parser.parse_args()

    port = args.port or autodetect_port()
    if not port:
        print("no serial port found; pass --port", file=sys.stderr)
        return 2

    try:
        ser = serial.Serial(port, args.baud, timeout=0.25, write_timeout=2)
    except Exception as exc:  # noqa: BLE001
        print(f"cannot open {port}: {exc}", file=sys.stderr)
        return 2

    lines: list[str] = []
    failures: list[str] = []
    done: tuple[int, int] | None = None

    def send(cmd: str) -> None:
        ser.write(b"\n" + cmd.encode() + b"\n")
        ser.flush()

    def pump_until_done(deadline: float) -> None:
        nonlocal done
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not line:
                continue
            lines.append(line)
            print(line, flush=True)
            if FAIL_RE.search(line):
                failures.append(line)
            m = DONE_RE.search(line)
            if m:
                done = (int(m.group(1)), int(m.group(2)))
                return

    try:
        ser.reset_input_buffer()
        if args.diag:
            send("wifi-diag")
            time.sleep(0.5)
            while ser.in_waiting:
                line = ser.readline().decode("utf-8", "replace").rstrip()
                print(line, flush=True)
                lines.append(line)
        send("fsd-selftest")
        pump_until_done(time.monotonic() + args.timeout)
        if args.diag:
            send("wifi-diag")
            time.sleep(0.5)
            while ser.in_waiting:
                line = ser.readline().decode("utf-8", "replace").rstrip()
                print(line, flush=True)
                lines.append(line)
    finally:
        ser.close()

    if done is None:
        print(f"\nFAIL: no '[fsd-selftest] done' within {args.timeout:.0f}s",
              file=sys.stderr)
        return 1
    passed, total = done
    print(f"\nselftest: {passed}/{total} passed")
    if failures:
        print("FAIL lines:", file=sys.stderr)
        for line in failures:
            print("  " + line, file=sys.stderr)
    if args.diag:
        # The post-run wifi-diag line carries the handshake counters.
        for line in lines:
            if "fsdHs" in line:
                print(line)
    return 0 if passed == total and not failures else 1


if __name__ == "__main__":
    sys.exit(main())
