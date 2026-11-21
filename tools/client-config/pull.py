#!/usr/bin/env python3
"""Fetch and apply an ESP32 configuration to a JSON file on an owned client."""

import argparse
import getpass
import http.client
import json
import os
from pathlib import Path
import re
import tempfile


def request(connection, token, method, path, payload=None):
    headers = {"Authorization": f"Bearer {token}"}
    if payload is not None:
        headers["Content-Type"] = "application/json"
    connection.request(method, path, payload, headers)
    response = connection.getresponse()
    if response.status != 200:
        raise ValueError(f"ESP32 returned HTTP {response.status} for {method} {path}")
    body = response.read(2049)
    if len(body) > 2048:
        raise ValueError("ESP32 reply exceeded 2048 bytes")
    return json.loads(body)


def apply_config(output, config):
    with tempfile.NamedTemporaryFile(dir=output.parent, encoding="utf-8", mode="w", delete=False) as file:
        temporary = Path(file.name)
        try:
            json.dump(config, file, ensure_ascii=False, indent=2, allow_nan=False)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
            file.close()
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)


def lock_output(file):
    if os.name == "nt":
        import msvcrt

        file.seek(0)
        msvcrt.locking(file.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl

        fcntl.flock(file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="9.9.9.9", help="ESP32 address or fsd.local")
    parser.add_argument("--port", type=int, default=10010, help="management port (443 is FSD-only)")
    parser.add_argument("--cert", help="Ignored; client-config is served over HTTP on the management port")
    parser.add_argument("--output", required=True, type=Path, help="Local configuration file")
    args = parser.parse_args()
    token = os.getenv("ESP32_CLIENT_CONFIG_TOKEN") or getpass.getpass("Client token: ")
    if not re.fullmatch(r"[0-9a-f]{64}", token):
        parser.exit(1, "Invalid client token\n")

    base = "/api/client-config"
    try:
        lock_path = args.output.with_name(args.output.name + ".lock")
        with lock_path.open("a+b") as lock:
            lock_output(lock)
            connection = http.client.HTTPConnection(args.host, args.port, timeout=10)
            try:
                result = request(connection, token, "GET", base)
                revision, config = result["revision"], result["config"]
                if type(revision) is not int or not (1 <= revision <= 0xffffffff) or not isinstance(config, dict):
                    raise ValueError("Invalid configuration response")
                apply_config(args.output, config)
                print(f"Applied revision {revision} to {args.output}", flush=True)
                confirmed = request(connection, token, "POST", base + "/ack", json.dumps({"revision": revision}))
                if not isinstance(confirmed, dict) or confirmed.get("applied") != revision:
                    raise ValueError("ESP32 returned a different applied revision")
            finally:
                connection.close()
    except (http.client.HTTPException, OSError, TypeError, ValueError, KeyError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Confirmed revision {revision} on ESP32")


if __name__ == "__main__":
    main()
