#!/usr/bin/env python3
"""Summarise a crop_rec AF log (ML/AFLOGnn.CSV or ptp_ml.py aflog output).

  af_view.py AFLOG00.CSV [--all]

Prints state changes (SAF mode, flags, AF frame state), how big the defocus
values are for each reliability byte, and a timeline: per frame the AF frame
state, the lens drive value (result bytes 30..31, signed) and the defocus of
the "good" areas (reliability bit 7 set, bits 2 and 3 clear) with their median
and spread. --all prints every frame instead of state changes and every 10th.

Reading it (normal LiveView, M2): when in focus the good areas agree within a
few hundred and sit near 0; a consistent offset across areas means real
defocus, and AF then drives the lens until it returns to ~0.
"""
import argparse
import collections
import csv
import statistics


def s16(v):
    return v - 65536 if v > 32767 else v


def good(r):
    return (r & 0x80) and not (r & 0x0C)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    rows = list(csv.DictReader(open(a.csv)))
    if not rows:
        raise SystemExit("empty log")
    I = lambda r, k: int(r[k])
    frames = [I(r, "frame") for r in rows]
    gaps = sum(1 for x, y in zip(frames, frames[1:]) if y - x != 1)
    print(f"{len(rows)} records, frames {frames[0]}..{frames[-1]}, {gaps} gaps")

    print("\nreliability byte: count, median |d|, 90th percentile |d|  (* = counted as good)")
    agg = collections.defaultdict(list)
    for r in rows:
        for i in range(10):
            agg[I(r, f"r{i}")].append(abs(I(r, f"d{i}")))
    for k in sorted(agg):
        v = sorted(agg[k])
        print(f"  {k:3d} {k:08b}{' *' if good(k) else '  '} n={len(v):5d} median {v[len(v) // 2]:6d}"
              f"  p90 {v[int(len(v) * .9)]:6d}")

    print("\nframe  flags        st sub  lens | good  median spread | good areas")
    prev = None
    spreads = []
    for n, r in enumerate(rows):
        key = (r["saf_mode"], r["flags"], r["af_mode"], r["waku_st"], r["waku_sub"])
        g = [I(r, f"d{i}") for i in range(10) if good(I(r, f"r{i}"))]
        med = int(statistics.median(g)) if g else 0
        spread = int(statistics.median([abs(x - med) for x in g])) if len(g) >= 3 else -1
        if spread >= 0 and r["waku_st"] != "9":
            spreads.append(spread)
        if key != prev:
            print(f"  -- SAF mode {key[0]}, AF method {key[2]}, flags {key[1]}, frame state {key[3]}/{key[4]}")
        if a.all or key != prev or n % 10 == 0:
            print(f"{I(r, 'frame'):6d} {r['flags']:12s} {r['waku_st']:>2} {r['waku_sub']:>3} "
                  f"{s16(I(r, 'pd_lens_pos')):5d} | {len(g):3d} {med:7d} {spread:6d} | "
                  + " ".join(map(str, g[:8])))
        prev = key
    if spreads:
        spreads.sort()
        print(f"\nagreement between good areas (median absolute deviation, frames with >= 3 good areas,"
              f" not focusing): median {spreads[len(spreads) // 2]}, p90 {spreads[int(len(spreads) * .9)]}")


if __name__ == "__main__":
    main()
