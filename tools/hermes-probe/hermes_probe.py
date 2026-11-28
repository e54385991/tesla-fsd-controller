#!/usr/bin/env python3
"""Lab-only local Hermes stub / probe.

This tool listens for the vehicle's Hermes connection, optionally completes a
WebSocket upgrade, and optionally sends a minimal STREAMING_CONFIG frame.  It
never opens an upstream connection and never forwards vehicle commands.

Stages:
  tls      complete TLS only, then close (trust / pinning probe)
  ws       TLS + WebSocket 101, log frames, do not send config
  config   TLS + WebSocket 101 + one STREAMING_CONFIG, no ACK
  full     TLS + WebSocket 101 + STREAMING_CONFIG + CLIENT_ACK + ping/pong

The certificate must be supplied by the operator.  See README.md and
make_lab_cert.py.  This is not a production service.
"""

from __future__ import annotations

import argparse
import atexit
import base64
import hashlib
import json
import os
import select
import signal
import socket
import ssl
import struct
import sys
import tempfile
import threading
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA

STATUS_CODE_SERVER_ACK = 1202
STATUS_CODE_CLIENT_ACK = 2202
COMMAND_TYPE_STREAMING_CONFIG = 1056


# ── logging ───────────────────────────────────────────────────────────────────


class JsonlLogger:
    def __init__(self, path: Path | None) -> None:
        self.path = path
        self._lock = threading.Lock()
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)

    def emit(self, event: str, **fields: Any) -> None:
        record: dict[str, Any] = {
            "ts": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
            "event": event,
        }
        record.update(fields)
        line = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
        with self._lock:
            if self.path is not None:
                with self.path.open("a", encoding="utf-8") as handle:
                    handle.write(line + "\n")
            print(line, flush=True)


def fingerprint(value: str | None) -> dict[str, Any] | None:
    """Redact a bearer token while keeping enough to correlate retries."""
    if not value:
        return None
    return {
        "present": True,
        "length": len(value),
        "sha256_12": hashlib.sha256(value.encode("utf-8", "replace")).hexdigest()[:12],
    }


# ── protobuf helpers (only the fields the probe needs) ────────────────────────


def varint(value: int) -> bytes:
    if value < 0:
        raise ValueError("varint must be non-negative")
    out = bytearray()
    while True:
        chunk = value & 0x7F
        value >>= 7
        if value:
            out.append(chunk | 0x80)
        else:
            out.append(chunk)
            return bytes(out)


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if offset >= len(data):
            raise ValueError("truncated varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def _tag(field_number: int, wire_type: int) -> bytes:
    return varint((field_number << 3) | wire_type)


def pb_bytes(field_number: int, value: bytes | str) -> bytes:
    if isinstance(value, str):
        value = value.encode("utf-8")
    return _tag(field_number, 2) + varint(len(value)) + value


def pb_varint(field_number: int, value: int) -> bytes:
    return _tag(field_number, 0) + varint(value)


def pb_message(field_number: int, value: bytes) -> bytes:
    return pb_bytes(field_number, value)


def encode_timestamp(seconds: int, nanos: int = 0) -> bytes:
    out = b""
    if seconds:
        out += pb_varint(1, seconds)
    if nanos:
        out += pb_varint(2, nanos)
    return out


def decode_fields(data: bytes) -> list[tuple[int, int, Any]]:
    fields: list[tuple[int, int, Any]] = []
    offset = 0
    while offset < len(data):
        key, offset = read_varint(data, offset)
        field_number = key >> 3
        wire_type = key & 7
        if wire_type == 0:
            value, offset = read_varint(data, offset)
        elif wire_type == 1:
            value = data[offset : offset + 8]
            offset += 8
        elif wire_type == 2:
            length, offset = read_varint(data, offset)
            if offset + length > len(data):
                raise ValueError("truncated length-delimited field")
            value = data[offset : offset + length]
            offset += length
        elif wire_type == 5:
            value = data[offset : offset + 4]
            offset += 4
        else:
            raise ValueError(f"unsupported wire type {wire_type}")
        fields.append((field_number, wire_type, value))
    return fields


def safe_text(value: bytes) -> str | None:
    try:
        text = value.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if all(32 <= ord(char) < 127 or char in "\r\n\t" for char in text):
        return text
    return None


def decode_timestamp(data: bytes) -> dict[str, int]:
    result = {"seconds": 0, "nanos": 0}
    for field_number, wire_type, value in decode_fields(data):
        if wire_type != 0:
            continue
        if field_number == 1:
            result["seconds"] = value
        elif field_number == 2:
            result["nanos"] = value
    return result


def decode_hermes_summary(data: bytes) -> dict[str, Any]:
    """Best-effort decode of the fields documented in timdorr #769."""
    result: dict[str, Any] = {"bytes": len(data), "command": None}
    try:
        for field_number, wire_type, value in decode_fields(data):
            if field_number != 1 or wire_type != 2:
                continue
            command: dict[str, Any] = {}
            for cmd_field, cmd_wire, cmd_value in decode_fields(value):
                if cmd_wire == 2 and cmd_field in (1, 2, 4, 6, 8, 12):
                    name = {
                        1: "txid",
                        2: "topic",
                        4: "sender_id",
                        6: "request_txid",
                        8: "response_topic",
                        12: "message_id",
                    }[cmd_field]
                    text = safe_text(cmd_value)
                    command[name] = text if text is not None else {"hex": cmd_value.hex()}
                elif cmd_wire == 0 and cmd_field in (5, 7):
                    command["command_type" if cmd_field == 5 else "status_code"] = cmd_value
                elif cmd_wire == 2 and cmd_field == 10:
                    text = safe_text(cmd_value)
                    if text is not None:
                        try:
                            command["payload_json"] = json.loads(text)
                        except json.JSONDecodeError:
                            command["payload_text"] = text
                    else:
                        command["payload_hex_len"] = len(cmd_value)
                elif cmd_wire == 2 and cmd_field == 9:
                    command["expiry"] = decode_timestamp(cmd_value)
                elif cmd_wire == 2 and cmd_field == 3:
                    command["created_at"] = decode_timestamp(cmd_value)
            result["command"] = command
    except Exception as exc:  # noqa: BLE001 - probe must log malformed frames
        result["decode_error"] = str(exc)
    return result


def encode_command_message(
    *,
    txid: str | bytes,
    topic: str | bytes,
    created_at: int,
    sender_id: str | bytes = "HermesServer",
    command_type: int = 0,
    request_txid: str | bytes | None = None,
    status_code: int | None = None,
    expiry: int | None = None,
    payload: bytes | None = None,
) -> bytes:
    out = b""
    out += pb_bytes(1, txid)
    out += pb_bytes(2, topic)
    out += pb_message(3, encode_timestamp(created_at))
    out += pb_bytes(4, sender_id)
    if command_type:
        out += pb_varint(5, command_type)
    if request_txid:
        out += pb_bytes(6, request_txid)
    if status_code is not None:
        out += pb_varint(7, status_code)
    if expiry is not None:
        out += pb_message(9, encode_timestamp(expiry))
    if payload:
        out += pb_bytes(10, payload)
    return out


def encode_hermes_message(command_message: bytes) -> bytes:
    return pb_message(1, command_message)


def default_config_payload() -> dict[str, Any]:
    return {
        "expiration": int(time.time()) + 3600,
        "connection_timeout": 180000,
        "ping_frequency": 60000,
        "autopark": {
            "heartbeat_frequency": 100,
            "autopark_pause_timeout": 2000,
            "autopark_stop_timeout": 10000,
        },
        "ice_servers": [],
    }


# ── WebSocket helpers ─────────────────────────────────────────────────────────


def recv_exact(sock: socket.socket, length: int) -> bytes:
    buffer = bytearray()
    while len(buffer) < length:
        chunk = sock.recv(length - len(buffer))
        if not chunk:
            raise ConnectionError("peer closed")
        buffer.extend(chunk)
    return bytes(buffer)


@dataclass
class Frame:
    fin: bool
    opcode: int
    payload: bytes


def read_frame(sock: socket.socket) -> Frame:
    first, second = recv_exact(sock, 2)
    fin = bool(first & 0x80)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", recv_exact(sock, 8))[0]
    if length > 16 * 1024 * 1024:
        raise ValueError("frame too large")
    mask = recv_exact(sock, 4) if masked else None
    payload = recv_exact(sock, length)
    if mask is not None:
        payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    return Frame(fin=fin, opcode=opcode, payload=payload)


def build_frame(opcode: int, payload: bytes = b"", *, fin: bool = True, mask: bool = False) -> bytes:
    first = (0x80 if fin else 0) | (opcode & 0x0F)
    length = len(payload)
    header = bytearray([first])
    mask_bit = 0x80 if mask else 0
    if length < 126:
        header.append(mask_bit | length)
    elif length < 65536:
        header.append(mask_bit | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(mask_bit | 127)
        header.extend(struct.pack("!Q", length))
    if mask:
        key = os.urandom(4)
        header.extend(key)
        payload = bytes(byte ^ key[index % 4] for index, byte in enumerate(payload))
    return bytes(header) + payload


def send_frame(sock: socket.socket, opcode: int, payload: bytes = b"") -> None:
    sock.sendall(build_frame(opcode, payload))


def ws_accept(key: str) -> str:
    digest = hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def read_http_request(sock: socket.socket, limit: int = 16384) -> tuple[str, dict[str, str]]:
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("peer closed before HTTP headers")
        data.extend(chunk)
        if len(data) > limit:
            raise ValueError("HTTP headers too large")
    head, _, _ = bytes(data).partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    request_line = lines[0].decode("latin1", "replace")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if b":" not in line:
            continue
        name, value = line.split(b":", 1)
        headers[name.decode("latin1").strip().lower()] = value.decode("latin1").strip()
    return request_line, headers


# ── probe ─────────────────────────────────────────────────────────────────────


class HermesProbe:
    def __init__(self, args: argparse.Namespace, logger: JsonlLogger) -> None:
        self.args = args
        self.logger = logger
        self.stop = threading.Event()
        self.connections = 0
        self._tls_local = threading.local()
        self._combined_path: Path | None = None
        self.context = self._build_context()
        self.config_payload = self._load_config_payload()
        self.config_is_default = self.args.config_json is None

    def _build_context(self) -> ssl.SSLContext:
        cert_path = Path(self.args.cert)
        if not cert_path.exists():
            raise SystemExit(f"certificate not found: {cert_path}")
        key_path = Path(self.args.key)
        if not key_path.exists():
            raise SystemExit(f"private key not found: {key_path}")
        chain_path = Path(self.args.chain) if self.args.chain else None
        if chain_path is not None:
            if not chain_path.exists():
                raise SystemExit(f"chain not found: {chain_path}")
            combined = cert_path.read_bytes().rstrip() + b"\n" + chain_path.read_bytes().lstrip()
            handle = tempfile.NamedTemporaryFile(prefix="hermes-probe-cert-", suffix=".pem", delete=False)
            handle.write(combined)
            handle.close()
            self._combined_path = Path(handle.name)
            atexit.register(lambda: self._combined_path and self._combined_path.unlink(missing_ok=True))
            cert_path = self._combined_path

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.set_alpn_protocols(["http/1.1"])
        context.options |= ssl.OP_NO_COMPRESSION
        context.sni_callback = self._sni_callback
        context.load_cert_chain(str(cert_path), str(key_path))
        return context

    def _sni_callback(self, _sslobj: ssl.SSLObject, server_name: str | None, _ctx: ssl.SSLContext) -> None:
        self._tls_local.sni = server_name

    def _load_config_payload(self) -> bytes:
        if self.args.config_json:
            raw = Path(self.args.config_json).read_bytes()
            value = json.loads(raw)
        else:
            value = default_config_payload()
        return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")

    def _streaming_config_frame(self) -> bytes:
        now = int(time.time())
        value = json.loads(self.config_payload)
        if self.config_is_default or int(value.get("expiration", 0)) <= now:
            value["expiration"] = now + 3600
        payload = json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        command = encode_command_message(
            txid=uuid.uuid4().hex,
            topic=self.args.config_topic,
            created_at=now,
            sender_id="HermesServer",
            command_type=COMMAND_TYPE_STREAMING_CONFIG,
            expiry=now + 3600,
            payload=payload,
        )
        return encode_hermes_message(command)

    def _ack_frame(self, incoming: Frame) -> bytes | None:
        summary = decode_hermes_summary(incoming.payload)
        command = summary.get("command") or {}
        txid = command.get("txid")
        topic = command.get("topic") or "internal"
        if isinstance(txid, dict):
            txid = None
        if not isinstance(topic, (str, bytes)):
            topic = "internal"
        # Do not ACK ACKs; otherwise both sides can loop forever.
        if command.get("status_code") in (STATUS_CODE_CLIENT_ACK, STATUS_CODE_SERVER_ACK):
            return None
        ack = encode_command_message(
            txid=uuid.uuid4().hex,
            topic=topic,
            created_at=int(time.time()),
            sender_id="HermesServer",
            request_txid=txid,
            status_code=STATUS_CODE_CLIENT_ACK,
        )
        return encode_hermes_message(ack)

    def handle(self, raw: socket.socket, address: tuple[str, int]) -> None:
        remote = f"{address[0]}:{address[1]}"
        raw.settimeout(10.0)
        self._tls_local.sni = None
        tls: ssl.SSLSocket | None = None
        try:
            tls = self.context.wrap_socket(raw, server_side=True)
            sni = getattr(self._tls_local, "sni", None)
            self.logger.emit(
                "tls_ok",
                remote=remote,
                sni=sni,
                alpn=tls.selected_alpn_protocol(),
                tls=tls.version(),
                cipher=tls.cipher()[0] if tls.cipher() else None,
            )
            if self.args.stage == "tls":
                return

            request_line, headers = read_http_request(tls)
            self.logger.emit(
                "http_request",
                remote=remote,
                request_line=request_line,
                host=headers.get("host"),
                upgrade=headers.get("upgrade"),
                connection=headers.get("connection"),
                subprotocol=headers.get("sec-websocket-protocol"),
                x_jwt=fingerprint(headers.get("x-jwt")),
                authorization=fingerprint(headers.get("authorization")),
                user_agent=headers.get("user-agent"),
            )
            if headers.get("upgrade", "").lower() != "websocket":
                tls.sendall(
                    b"HTTP/1.1 426 Upgrade Required\r\n"
                    b"Content-Type: text/plain\r\n"
                    b"Content-Length: 20\r\n"
                    b"Connection: close\r\n\r\n"
                    b"WebSocket required\n"
                )
                return
            key = headers.get("sec-websocket-key")
            if not key:
                tls.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                return
            response = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {ws_accept(key)}\r\n"
            )
            if self.args.ws_subprotocol:
                response += f"Sec-WebSocket-Protocol: {self.args.ws_subprotocol}\r\n"
            response += "\r\n"
            tls.sendall(response.encode("ascii"))
            self.logger.emit("ws_open", remote=remote, path=request_line.split(" ")[1] if " " in request_line else None)

            if self.args.stage in ("config", "full"):
                time.sleep(self.args.config_delay)
                frame = self._streaming_config_frame()
                send_frame(tls, OP_BINARY, frame)
                self.logger.emit(
                    "frame_sent",
                    remote=remote,
                    opcode=OP_BINARY,
                    bytes=len(frame),
                    summary=decode_hermes_summary(frame),
                )

            self._frame_loop(tls, remote)
        except ssl.SSLError as exc:
            self.logger.emit(
                "tls_error",
                remote=remote,
                sni=getattr(self._tls_local, "sni", None),
                error=str(exc),
            )
        except (ConnectionError, socket.timeout, OSError) as exc:
            self.logger.emit("connection_error", remote=remote, error=type(exc).__name__, detail=str(exc))
        except Exception as exc:  # noqa: BLE001 - probe logs and continues
            self.logger.emit("probe_error", remote=remote, error=type(exc).__name__, detail=str(exc))
        finally:
            try:
                if tls is not None:
                    tls.close()
                else:
                    raw.close()
            except OSError:
                pass
            self.logger.emit("closed", remote=remote)

    def _frame_loop(self, tls: ssl.SSLSocket, remote: str) -> None:
        last_ping = time.monotonic()
        ping_enabled = self.args.stage == "full" and self.args.ping_interval > 0
        while not self.stop.is_set():
            timeout = 1.0
            if ping_enabled:
                timeout = min(timeout, max(0.1, self.args.ping_interval - (time.monotonic() - last_ping)))
            readable, _, _ = select.select([tls], [], [], timeout)
            if readable:
                frame = read_frame(tls)
                self.logger.emit(
                    "frame",
                    remote=remote,
                    opcode=frame.opcode,
                    fin=frame.fin,
                    bytes=len(frame.payload),
                    summary=decode_hermes_summary(frame.payload) if frame.opcode == OP_BINARY else None,
                    text=safe_text(frame.payload) if frame.opcode == OP_TEXT else None,
                )
                if frame.opcode == OP_PING:
                    send_frame(tls, OP_PONG, frame.payload)
                elif frame.opcode == OP_CLOSE:
                    send_frame(tls, OP_CLOSE, frame.payload[:2])
                    return
                elif frame.opcode == OP_BINARY and self.args.stage == "full":
                    ack = self._ack_frame(frame)
                    if ack:
                        send_frame(tls, OP_BINARY, ack)
                        self.logger.emit(
                            "frame_sent",
                            remote=remote,
                            opcode=OP_BINARY,
                            bytes=len(ack),
                            summary=decode_hermes_summary(ack),
                        )
            if ping_enabled and time.monotonic() - last_ping >= self.args.ping_interval:
                send_frame(tls, OP_PING, b"probe")
                self.logger.emit("ping_sent", remote=remote)
                last_ping = time.monotonic()

    def serve(self) -> None:
        with socket.create_server((self.args.bind, self.args.port), reuse_port=False) as server:
            server.settimeout(1.0)
            self.logger.emit(
                "listen",
                bind=self.args.bind,
                port=self.args.port,
                stage=self.args.stage,
                cert=self.args.cert,
                chain=self.args.chain,
                upstream="disabled",
            )
            while not self.stop.is_set():
                try:
                    raw, address = server.accept()
                except socket.timeout:
                    continue
                self.connections += 1
                if self.args.once:
                    self.handle(raw, address)
                    return
                thread = threading.Thread(target=self.handle, args=(raw, address), daemon=True)
                thread.start()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bind", default="0.0.0.0", help="listen address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=443, help="listen port (default: 443)")
    parser.add_argument("--cert", required=True, help="leaf certificate PEM (may already include the chain)")
    parser.add_argument("--key", required=True, help="leaf private key PEM")
    parser.add_argument("--chain", help="optional intermediate chain PEM appended to --cert")
    parser.add_argument(
        "--stage",
        choices=("tls", "ws", "config", "full"),
        default="tls",
        help="probe stage (default: tls)",
    )
    parser.add_argument("--log", type=Path, default=Path("hermes-probe.jsonl"), help="JSONL output path")
    parser.add_argument("--config-json", type=Path, help="use this JSON as the STREAMING_CONFIG payload")
    parser.add_argument("--config-topic", default="internal", help="topic for STREAMING_CONFIG (default: internal)")
    parser.add_argument("--config-delay", type=float, default=0.2, help="seconds between 101 and config (default: 0.2)")
    parser.add_argument("--ping-interval", type=float, default=60.0, help="WebSocket ping interval seconds; 0 disables")
    parser.add_argument("--ws-subprotocol", help="optional Sec-WebSocket-Protocol value to echo")
    parser.add_argument("--once", action="store_true", help="handle one connection then exit")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    logger = JsonlLogger(args.log)
    probe = HermesProbe(args, logger)

    def stop(_signum: int, _frame: Any) -> None:
        probe.stop.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    probe.serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
