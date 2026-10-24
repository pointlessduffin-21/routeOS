"""Summarise an RVDP soak: reads the log and the health CSV the soak writes, prints the verdict.

Usage: soak_report.py [soak log] [report file]
Defaults to soak-4h.log beside this folder and writes soak-4h-report.md next to it.
"""
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "soak-4h.log")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(os.path.abspath(LOG))[0] + "-report.md"
CSV = os.path.splitext(LOG)[0] + "-health.csv"


def read_log(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as handle:
        return [line.rstrip("\n") for line in handle]


def read_health(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.strip().split(",")
            if len(parts) < 6:
                continue
            try:
                rows.append((int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]),
                             int(parts[4]) if parts[4] else None, int(parts[5]) if parts[5] else None))
            except ValueError:
                continue
    return rows


def stamp(epoch):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(epoch))


def main():
    lines = read_log(LOG)
    health = read_health(CSV)
    out = []
    out.append(f"RVDP soak report, generated {stamp(time.time())}")
    out.append(f"log {os.path.abspath(LOG)}")
    out.append("")

    starts = [l for l in lines if "soak start" in l]
    opens = [l for l in lines if "session open" in l]
    ended = [l for l in lines if ": ENDED" in l]
    repaints = [l for l in lines if "full repaint" in l]
    tap_ok = [l for l in lines if "new page in" in l]
    tap_bad = [l for l in lines if "NO REPAINT" in l]
    boards = [l for l in lines if "BOARD REBOOTED" in l]
    hours = [l for l in lines if l.startswith("--- after") or "=== soak end" in l]

    if starts:
        out.append(f"run started: {starts[-1][:19]}")
    out.append(f"sessions opened: {len(opens)}   ended early: {len(ended)}")
    for line in ended[-10:]:
        out.append(f"  {line}")
    if boards:
        out.append(f"reboots seen by the soak: {len(boards)}")
        for line in boards[-5:]:
            out.append(f"  {line}")

    times = []
    rects = []
    for line in repaints:
        try:
            seconds = float(line.split(" in ")[1].rstrip("s"))
            count = int(line.split(": ")[1].split(" rectangles")[0])
        except (IndexError, ValueError):
            continue
        times.append(seconds)
        rects.append(count)
    if times:
        out.append("")
        out.append(f"full repaints: {len(times)} samples, {rects[-1]} rectangles each")
        out.append(f"  mean {statistics.mean(times):.1f}s, median {statistics.median(times):.1f}s, "
                   f"best {min(times):.1f}s, worst {max(times):.1f}s")
        if len(times) >= 4:
            first = statistics.mean(times[:3])
            last = statistics.mean(times[-3:])
            out.append(f"  first three {first:.1f}s, last three {last:.1f}s, drift {last - first:+.1f}s")
        if len(times) >= 6:
            out.append("  every sample: " + ", ".join(f"{t:.1f}" for t in times))

    latencies = []
    for line in tap_ok:
        try:
            latencies.append(float(line.split("new page in ")[1].split("s")[0]))
        except (IndexError, ValueError):
            continue
    out.append("")
    out.append(f"taps: {len(tap_ok)} changed the page, {len(tap_bad)} with no page change")
    if latencies:
        out.append(f"  tap to repaint: mean {statistics.mean(latencies):.1f}s, median {statistics.median(latencies):.1f}s, "
                   f"best {min(latencies):.1f}s, worst {max(latencies):.1f}s")
        if len(latencies) >= 6:
            out.append(f"  first three {statistics.mean(latencies[:3]):.1f}s, "
                       f"last three {statistics.mean(latencies[-3:]):.1f}s")
    for line in tap_bad[-5:]:
        out.append(f"  {line}")

    if health:
        frees = [r[1] for r in health]
        blocks = [r[2] for r in health]
        rssis = [r[4] for r in health if r[4] is not None]
        reboots = sum(1 for a, b in zip(health, health[1:]) if b[3] < a[3] - 60)
        span = health[-1][0] - health[0][0]
        out.append("")
        out.append(f"health samples: {len(health)} over {span / 60:.0f} min "
                   f"({stamp(health[0][0])} to {stamp(health[-1][0])})")
        out.append(f"  heap free KB: first {frees[0]}, last {frees[-1]}, low {min(frees)}, high {max(frees)}, "
                   f"trend {frees[-1] - frees[0]:+d} KB")
        out.append(f"  largest block KB: low {min(blocks)}, high {max(blocks)}")
        out.append(f"  board uptime: {health[-1][3] / 60:.0f} min at the last sample, reboots in the samples: {reboots}")
        if rssis:
            out.append(f"  rssi dBm: low {min(rssis)}, high {max(rssis)}, mean {statistics.mean(rssis):.0f}, "
                       f"channel {health[-1][5]}")
        gaps = [b[0] - a[0] for a, b in zip(health, health[1:])]
        if gaps:
            out.append(f"  sample gaps: mean {statistics.mean(gaps):.0f}s, worst {max(gaps)}s")

    if hours:
        out.append("")
        out.extend(hours[-6:])

    verdict = "clean so far"
    if ended:
        verdict = f"{len(ended)} early ending(s) recorded"
    if tap_bad:
        verdict += f", {len(tap_bad)} tap(s) with no repaint"
    out.append("")
    out.append(f"verdict: {verdict}")

    text = "\n".join(out)
    print(text)
    with open(OUT, "w", encoding="utf-8") as handle:
        handle.write(text + "\n")
    print(f"\nwritten to {OUT}")


if __name__ == "__main__":
    main()
