#!/usr/bin/env python3
"""Mint a lab-only leaf certificate from the eMudhra chain in fsd_tls_identity.h.

This is for an isolated lab and an owned vehicle only.  It reuses the same
EM OV TLS CA - G3C key material as the existing FSD certificate, adds the
requested SANs, and writes:

  certs/lab-leaf.pem   leaf certificate
  certs/lab-chain.pem  intermediates (EM OV -> emSign Root -> AAA)
  certs/lab-cert.pem   leaf + intermediates, for --cert
  certs/lab-key.pem    private key, for --key

The script never installs anything on a vehicle and never contacts Tesla.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PEM_RE = re.compile(r'R"PEM\((.*?)\)PEM"', re.S)
CERT_RE = re.compile(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", re.S)
ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HEADER = ROOT / "include" / "fsd_tls_identity.h"
DEFAULT_OUT = Path(__file__).resolve().parent / "certs"


def run(command: list[str], *, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        input=input_text,
        capture_output=True,
        check=True,
    )


def parse_header(header: Path) -> tuple[list[str], str, str]:
    text = header.read_text(encoding="utf-8")
    blocks = PEM_RE.findall(text)
    if len(blocks) != 3:
        raise SystemExit(f"expected 3 PEM blocks in {header}, found {len(blocks)}")
    chain, key, ca = blocks
    certs = CERT_RE.findall(chain)
    if len(certs) < 4:
        raise SystemExit(f"expected leaf + 3 intermediates in {header}, found {len(certs)}")
    return certs, key, ca


def subject_of(cert_pem: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False) as handle:
        handle.write(cert_pem)
        path = Path(handle.name)
    try:
        result = run(["openssl", "x509", "-in", str(path), "-noout", "-subject"])
        return result.stdout.strip()
    finally:
        path.unlink(missing_ok=True)


def find_issuer(certs: list[str]) -> int:
    for index, cert in enumerate(certs[1:], start=1):
        subject = subject_of(cert)
        if "EM OV TLS CA" in subject:
            return index
    raise SystemExit("could not find 'EM OV TLS CA' in the header chain")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER, help=f"source header (default: {DEFAULT_HEADER})")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"output directory (default: {DEFAULT_OUT})")
    parser.add_argument("--san", action="append", required=True, help="DNS SAN to add; repeat for each host")
    parser.add_argument("--cn", help="leaf common name (default: first --san)")
    parser.add_argument("--days", type=int, default=3000, help="validity in days (default: 3000)")
    args = parser.parse_args(argv)

    openssl = shutil.which("openssl")
    if not openssl:
        raise SystemExit("openssl not found")

    certs, key, ca_bundle = parse_header(args.header)
    issuer_index = find_issuer(certs)
    issuer = certs[issuer_index]
    chain = "\n".join(certs[issuer_index:]) + "\n"
    san = [value.strip().lower() for value in args.san if value.strip()]
    if not san:
        raise SystemExit("at least one --san is required")
    cn = args.cn or san[0]

    args.out.mkdir(parents=True, exist_ok=True)
    ca_path = args.out / "lab-ca.pem"
    ca_key_path = args.out / "lab-ca-key.pem"
    trust_path = args.out / "lab-trust.pem"
    leaf_path = args.out / "lab-leaf.pem"
    chain_path = args.out / "lab-chain.pem"
    cert_path = args.out / "lab-cert.pem"
    key_path = args.out / "lab-key.pem"
    ca_path.write_text(issuer + "\n", encoding="utf-8")
    ca_key_path.write_text(key, encoding="utf-8")
    trust_path.write_text(ca_bundle, encoding="utf-8")
    chain_path.write_text(chain, encoding="utf-8")
    key_path.write_text(key, encoding="utf-8")

    alt_lines = ["[alt_names]"]
    for index, host in enumerate(san, start=1):
        alt_lines.append(f"DNS.{index} = {host}")
    ext_text = "\n".join(
        [
            "[leaf_ext]",
            "basicConstraints = critical,CA:FALSE",
            "keyUsage = critical,digitalSignature",
            "extendedKeyUsage = serverAuth",
            "authorityKeyIdentifier = keyid,issuer",
            "subjectAltName = @alt_names",
            "",
            *alt_lines,
            "",
        ]
    )
    with tempfile.TemporaryDirectory(prefix="hermes-lab-cert-") as tmp:
        tmpdir = Path(tmp)
        csr_path = tmpdir / "leaf.csr"
        ext_path = tmpdir / "leaf.ext"
        serial_path = tmpdir / "ca.srl"
        ext_path.write_text(ext_text, encoding="utf-8")
        run(
            [
                openssl,
                "req",
                "-new",
                "-sha384",
                "-key",
                str(ca_key_path),
                "-out",
                str(csr_path),
                "-subj",
                f"/CN={cn}/O=Tesla Inc.",
            ]
        )
        run(
            [
                openssl,
                "x509",
                "-req",
                "-sha384",
                "-in",
                str(csr_path),
                "-CA",
                str(ca_path),
                "-CAkey",
                str(ca_key_path),
                "-CAserial",
                str(serial_path),
                "-CAcreateserial",
                "-out",
                str(leaf_path),
                "-days",
                str(args.days),
                "-extfile",
                str(ext_path),
                "-extensions",
                "leaf_ext",
            ]
        )
    cert_path.write_text(leaf_path.read_text(encoding="utf-8").rstrip() + "\n" + chain, encoding="utf-8")

    try:
        verified = run(
            [
                openssl,
                "verify",
                "-CAfile",
                str(trust_path),
                "-untrusted",
                str(chain_path),
                str(leaf_path),
            ]
        )
    except subprocess.CalledProcessError as exc:
        print(exc.stdout, file=sys.stderr)
        print(exc.stderr, file=sys.stderr)
        raise SystemExit("certificate verification failed; check the eMudhra trust bundle") from exc
    print(verified.stdout.strip())
    print(f"leaf:  {leaf_path}")
    print(f"chain: {chain_path}")
    print(f"cert:  {cert_path}")
    print(f"key:   {key_path}")
    print(f"trust: {trust_path}")
    print(f"SANs:  {', '.join(san)}")
    print("LAB ONLY: do not install this material on a vehicle or use it against Tesla infrastructure.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
