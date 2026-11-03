#!/usr/bin/env python3
"""EOS M2: measure raw slurp timing in the current LiveView mode over USB (remote LiveView).

  slurp_timing.py [--mode 0|1] [--seconds N]

--mode 0 forces vsync arming (raw_isr_mode_override), 1 interrupt mode; the
override is set back to -1 (crop_rec decides) at the end. Prints Canon's frame
timing registers (crop_rec mailbox 'timers'), and per frame: completion of the
slurp transfer relative to the readout-done interrupt, the vsync arm latency
after it, and whether the transfer was still busy at readout-done.
"""
import argparse, os, statistics as st, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptp_ml as m

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--mode", type=int, default=0)
ap.add_argument("--seconds", type=float, default=4)
ap.add_argument("--sym", default=m.DEFAULT_SYM)
a = ap.parse_args()
S = lambda n: m.sym_address(a.sym, n)
TIMER = 0x100000    # DIGIC timer wraps at 20 bits (us)
d = lambda x, y: (x - y) % TIMER

p = m.PTP()
try:
    p.write32(S("raw_isr_mode_override"), a.mode & 0xFFFFFFFF)
    out = p.remote_lv(True, (1,))
    t = time.time()
    while time.time() - t < 3 + a.seconds:
        p.eos_events(); time.sleep(0.2)
    print("remote LiveView, EVF output", out, "| lv_raw_enabled", p.words(S("lv_raw_enabled"))[0],
          "| arming mode", a.mode)
    slots = {name: (ptr, ln) for _, name, ptr, ln in m.read_mailbox(p, a.sym)}
    if "timers" in slots:
        raw = p.read(*slots["timers"])
        magic, n, writes = struct.unpack_from("<III", raw)
        regs = struct.unpack_from("<12I", raw, 12); vals = struct.unpack_from("<12I", raw, 60)
        tv = {r: v for r, v in zip(regs, vals) if r}
        print(f"Canon timing registers ({writes} writes):", " ".join(f"{r & 0xFFFF:04X}={v:x}" for r, v in tv.items()))
        if 0xC0F06014 in tv and 0xC0F06008 in tv:
            B = (tv[0xC0F06014] & 0xFFFF) + 1; A = (tv[0xC0F06008] & 0xFFFF) + 1
            line_us = 41708.0 / B
            y0 = (tv.get(0xC0F06800, 0) >> 16); y1 = (tv.get(0xC0F06804, 0) >> 16)
            print(f"  timer A {A} x B {B} (lines per frame) -> {line_us:.2f} us per line at 23.976 fps;"
                  f" readout lines {y0:#x}..{y1:#x} = {y1 - y0}; outside the readout {B - (y1 - y0)} lines"
                  f" = {(B - (y1 - y0)) * line_us / 1000:.1f} ms")
    else:
        print("no 'timers' mailbox slot (preset not active?)")
    L = S("raw_isr_log")
    raw = p.read(L, 8 + 64 * 24); w = struct.unpack_from("<I", raw, 4)[0]
    E = [struct.unpack_from("<6I", raw, 8 + 24 * (i % 64)) for i in range(w - 64, w)]
    comp = [d(e[0], e[2]) for e in E]                      # readout-done minus last completion
    lat = [d(E[k + 1][1], E[k][0]) for k in range(len(E) - 1) if d(E[k + 1][1], E[k][0]) < 41000]
    busy = sum(e[3] & 1 for e in E)
    print(f"readout-done after last completion (us): min {min(comp)} median {st.median(comp):.0f} max {max(comp)}")
    if lat:
        print(f"vsync arm after readout-done (us): n {len(lat)} min {min(lat)} median {st.median(lat):.0f} max {max(lat)}")
    print(f"busy at readout-done: {busy} of {len(E)}")
    p.remote_lv(False)
finally:
    try:
        p.write32(S("raw_isr_mode_override"), 0xFFFFFFFF)    # -1
    finally:
        p.close()
