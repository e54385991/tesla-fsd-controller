#!/usr/bin/env python3
"""Summarize the FSD stability probe + mixed Tesla traffic stress logs."""
from __future__ import annotations

import argparse
import json
import statistics
from collections import Counter, defaultdict
from pathlib import Path


def load(path: Path) -> list[dict]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return out


def pct(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    idx = min(len(values) - 1, max(0, int(round((p / 100.0) * (len(values) - 1)))))
    return values[idx]


def counts(events: list[dict], kinds: tuple[str, ...]) -> Counter:
    c = Counter()
    for e in events:
        if e.get("kind") in kinds:
            c[e["kind"]] += 1
            if e.get("ok") is True:
                c[e["kind"] + ".ok"] += 1
            elif e.get("ok") is False:
                c[e["kind"] + ".fail"] += 1
            if e.get("soft"):
                c[e["kind"] + ".soft"] += 1
    return c


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("probe")
    ap.add_argument("stress")
    args = ap.parse_args()
    probe = load(Path(args.probe))
    stress = load(Path(args.stress))

    print("== probe ==")
    pc = counts(probe, ("fsd", "burst", "status", "page", "proxy", "payload", "reboot"))
    for k in sorted(pc):
        print(f"  {k}: {pc[k]}")
    fsd = [e for e in probe if e.get("kind") == "fsd"]
    fsd_ok = [e for e in fsd if e.get("ok") is True]
    fsd_bad = [e for e in fsd if e.get("ok") is False]
    lat = [e.get("ms", 0) for e in fsd_ok]
    print(f"  fsd success: {len(fsd_ok)}/{len(fsd)}")
    if lat:
        print(f"  fsd latency ms: p50={pct(lat,50):.0f} p95={pct(lat,95):.0f} max={max(lat):.0f}")
    statuses = [e for e in probe if e.get("kind") == "status"]
    heaps = [e["heap"] for e in statuses if isinstance(e.get("heap"), int)]
    mins = [e["minHeap"] for e in statuses if isinstance(e.get("minHeap"), int)]
    if heaps:
        print(f"  heap min={min(heaps)} last={heaps[-1]}")
    if mins:
        print(f"  device minHeap low-water={min(mins)}")
    resets = Counter(e.get("resetReason") for e in statuses if e.get("resetReason"))
    print(f"  reset reasons: {dict(resets)}")
    uptimes = [e["uptime"] for e in statuses if isinstance(e.get("uptime"), int)]
    print(f"  uptime first/last: {uptimes[0] if uptimes else None}/{uptimes[-1] if uptimes else None}")
    if fsd_bad:
        print("  failed FSD timestamps (tail):")
        for e in fsd_bad[-12:]:
            print(f"    {e.get('ts')}: {e.get('reason','')} ms={e.get('ms')}")

    print("== stress ==")
    sc = counts(stress, ("tls", "fsd", "burst", "mgmt", "mgmt_burst", "dns",
                         "malformed", "health", "hard", "payload"))
    for k in sorted(sc):
        print(f"  {k}: {sc[k]}")
    hard = [e for e in stress if e.get("kind") == "hard"]
    print(f"  hard failures: {len(hard)}")
    for e in hard[-10:]:
        print(f"    {e.get('ts')}: {e.get('reason')} {e.get('reset','')} {e.get('heap','')}")
    health = [e for e in stress if e.get("kind") in ("health", "start", "final") and e.get("ok") is not None]
    hheaps = [e["heap"] for e in health if isinstance(e.get("heap"), int)]
    hupt = [e["uptime"] for e in health if isinstance(e.get("uptime"), int)]
    if hheaps:
        print(f"  stress heap min={min(hheaps)} last={hheaps[-1]}")
    if hupt:
        print(f"  stress uptime first/last: {hupt[0]}/{hupt[-1]}")
    tls_status = Counter(e.get("status") for e in stress if e.get("kind") in ("tls", "burst"))
    print(f"  TLS status distribution: {dict(tls_status)}")
    dns_kinds = Counter(e.get("qkind") for e in stress if e.get("kind") == "dns")
    print(f"  DNS query kinds: {dict(dns_kinds)}")
    mal = Counter(e.get("mkind") for e in stress if e.get("kind") == "malformed")
    print(f"  malformed kinds: {dict(mal)}")

    # Correlate probe FSD failures with stressor bursts.
    burst_ts = sorted({e.get("ts") for e in stress
                       if e.get("kind") == "burst" and "ok_count" in e})
    windows = [(t - 8, t + 8) for t in burst_ts]
    def in_burst(ts: float) -> bool:
        return any(a <= ts <= b for a, b in windows)
    bad_in = [e for e in fsd_bad if in_burst(e.get("ts", 0))]
    bad_out = [e for e in fsd_bad if not in_burst(e.get("ts", 0))]
    print("== correlation ==")
    print(f"  stress bursts: {len(burst_ts)}")
    print(f"  probe FSD failures inside burst windows: {len(bad_in)}")
    print(f"  probe FSD failures outside burst windows: {len(bad_out)}")
    for e in bad_out[-10:]:
        print(f"    outside: {e.get('ts')} {e.get('reason','')}")

    # Final device state from the last probe status.
    last = next((e for e in reversed(statuses) if e.get("fsdMock")), None)
    if last:
        print("== final ==")
        print(f"  heap={last.get('heap')} minHeap={last.get('minHeap')} reset={last.get('resetReason')}")
        print(f"  fsdMock={last.get('fsdMock')}")
        print(f"  handshake={last.get('handshake')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
