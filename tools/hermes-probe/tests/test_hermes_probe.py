#!/usr/bin/env python3
"""Unit tests for the lab Hermes probe. No network or vehicle required."""

from __future__ import annotations

import base64
import hashlib
import json
import socket
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "hermes-probe"))

import hermes_probe as probe  # noqa: E402


class ProtobufTests(unittest.TestCase):
    def test_varint_roundtrip(self) -> None:
        for value in (0, 1, 127, 128, 300, 16384, 2**32 - 1, 2**63 - 1):
            encoded = probe.varint(value)
            decoded, offset = probe.read_varint(encoded, 0)
            self.assertEqual(decoded, value)
            self.assertEqual(offset, len(encoded))

    def test_command_roundtrip(self) -> None:
        command = probe.encode_command_message(
            txid="tx-1",
            topic="internal",
            created_at=1700000000,
            sender_id="HermesServer",
            command_type=probe.COMMAND_TYPE_STREAMING_CONFIG,
            expiry=1700003600,
            payload=b'{"expiration":1700003600}',
        )
        message = probe.encode_hermes_message(command)
        summary = probe.decode_hermes_summary(message)
        self.assertEqual(summary["command"]["txid"], "tx-1")
        self.assertEqual(summary["command"]["topic"], "internal")
        self.assertEqual(summary["command"]["sender_id"], "HermesServer")
        self.assertEqual(summary["command"]["command_type"], probe.COMMAND_TYPE_STREAMING_CONFIG)
        self.assertEqual(summary["command"]["payload_json"]["expiration"], 1700003600)
        self.assertEqual(summary["command"]["expiry"]["seconds"], 1700003600)

    def test_malformed_frame_is_reported(self) -> None:
        summary = probe.decode_hermes_summary(b"\x0a\xff")
        self.assertIn("decode_error", summary)


class WebSocketTests(unittest.TestCase):
    def test_accept_vector(self) -> None:
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        self.assertEqual(probe.ws_accept(key), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")

    def test_masked_frame_roundtrip(self) -> None:
        left, right = socket.socketpair()
        try:
            payload = b'{"hello":"world"}'
            left.sendall(probe.build_frame(probe.OP_BINARY, payload, mask=True))
            frame = probe.read_frame(right)
            self.assertEqual(frame.opcode, probe.OP_BINARY)
            self.assertTrue(frame.fin)
            self.assertEqual(frame.payload, payload)
        finally:
            left.close()
            right.close()

    def test_unmasked_server_frame_roundtrip(self) -> None:
        left, right = socket.socketpair()
        try:
            payload = b"server-frame"
            left.sendall(probe.build_frame(probe.OP_TEXT, payload))
            frame = probe.read_frame(right)
            self.assertEqual(frame.opcode, probe.OP_TEXT)
            self.assertEqual(frame.payload, payload)
        finally:
            left.close()
            right.close()

    def test_large_frame_header(self) -> None:
        payload = b"x" * 200
        frame = probe.build_frame(probe.OP_BINARY, payload)
        self.assertEqual(frame[1] & 0x7F, 126)
        left, right = socket.socketpair()
        try:
            left.sendall(frame)
            self.assertEqual(probe.read_frame(right).payload, payload)
        finally:
            left.close()
            right.close()


class HttpTests(unittest.TestCase):
    def test_parse_upgrade_headers(self) -> None:
        left, right = socket.socketpair()
        try:
            request = (
                b"GET /v1/mobile HTTP/1.1\r\n"
                b"Host: hermes-prd.vn.cloud.tesla.cn\r\n"
                b"Upgrade: websocket\r\n"
                b"Connection: Upgrade\r\n"
                b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                b"X-Jwt: secret-token\r\n"
                b"\r\n"
            )
            left.sendall(request)
            line, headers = probe.read_http_request(right)
            self.assertIn("/v1/mobile", line)
            self.assertEqual(headers["host"], "hermes-prd.vn.cloud.tesla.cn")
            self.assertEqual(headers["upgrade"], "websocket")
            self.assertEqual(headers["x-jwt"], "secret-token")
        finally:
            left.close()
            right.close()


class ConfigTests(unittest.TestCase):
    def test_default_config_payload_is_json(self) -> None:
        payload = probe.default_config_payload()
        encoded = json.dumps(payload, separators=(",", ":")).encode()
        decoded = json.loads(encoded)
        self.assertEqual(decoded["connection_timeout"], 180000)
        self.assertEqual(decoded["ping_frequency"], 60000)
        self.assertIn("expiration", decoded)
        self.assertIn("autopark", decoded)
        self.assertEqual(decoded["ice_servers"], [])

    def test_probe_has_no_upstream_client(self) -> None:
        source = (ROOT / "tools" / "hermes-probe" / "hermes_probe.py").read_text()
        self.assertNotIn("create_connection", source)
        self.assertNotIn(".connect(", source)
        self.assertNotIn("esp_http_client", source)


if __name__ == "__main__":
    unittest.main()
