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

"Response" is the check that matters: when the lens is driven, real phase data
changes with it (normal LiveView: about +1.5 defocus per lens-drive unit,
correlation ~0.5); noise doesn't (mcm SAF mode 4: 0.0 and 0.0).
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
    n, slope, corr = response(rows)
    if slope is None:
        print(f"response to lens movement: not enough lens moves ({n})")
    else:
        print(f"response to lens movement ({n} moves): {slope:+.2f} defocus per drive unit, correlation {corr:+.2f}"
              "  (normal LiveView reference: about +1.5, +0.5)")


def response(rows, step=6):
    """slope and correlation of good-area defocus change vs lens drive change,
    over `step` frames, when the lens moved and the AF frame state stayed the same"""
    pts = []
    for a, b in zip(rows, rows[step:]):
        if a["waku_st"] != b["waku_st"]:
            continue
        la, lb = s16(int(a["pd_lens_pos"])), s16(int(b["pd_lens_pos"]))
        if abs(lb - la) < 50 or (la == 0) != (lb == 0):     # drive value resets to 0 between AF runs
            continue
        ga = [int(a[f"d{k}"]) for k in range(10) if good(int(a[f"r{k}"]))]
        gb = [int(b[f"d{k}"]) for k in range(10) if good(int(b[f"r{k}"]))]
        if ga and gb:
            pts.append((lb - la, statistics.median(gb) - statistics.median(ga)))
    if len(pts) < 3:
        return len(pts), None, None
    mx = statistics.mean(p[0] for p in pts); my = statistics.mean(p[1] for p in pts)
    sxy = sum((x - mx) * (y - my) for x, y in pts)
    sxx = sum((x - mx) ** 2 for x, _ in pts); syy = sum((y - my) ** 2 for _, y in pts)
    return len(pts), sxy / sxx, (sxy / (sxx * syy) ** .5 if syy else 0.0)


if __name__ == "__main__":
    main()
