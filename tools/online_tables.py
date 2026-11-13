#!/usr/bin/env python3
"""Print the numbers and LaTeX tables of the report's online section from
results/online_50.csv and results/offline_50.csv.

    python3 tools/online_tables.py [--online results/online_50.csv] [--offline results/offline_50.csv]
"""

import argparse
import csv
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODES = (("myopic", "Online, myopic"), ("value", "Online, value-guided"))
# Other value tables run by online_eval.py --table, shown as extra rows when present.
EXTRA = (("hold_preference", "Online, hold preference only"), ("offline_10seq", "Online, earlier table (10 sequences)"))


def read(path):
    with open(path) as f:
        rows = csv.DictReader(line for line in f if not line.startswith("#"))
        return {int(r["seed"]): {k: int(v) for k, v in r.items()} for r in rows}


def cells(xs):
    return f"${st.mean(xs):.2f}$ (${st.pstdev(xs):.2f}$) & ${st.median(xs):g}$ & ${min(xs)}$--${max(xs)}$"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--online", default=str(ROOT / "results" / "online_50.csv"))
    ap.add_argument("--offline", default=str(ROOT / "results" / "offline_50.csv"))
    args = ap.parse_args()
    online, offline = read(args.online), read(args.offline)
    seeds = sorted(s for s in online if s in offline)
    n = len(seeds)
    on = [online[s] for s in seeds]
    off = [offline[s] for s in seeds]
    print(f"% {n} seeds: {seeds[0]} to {seeds[-1]}\n")

    print("\\begin{tabular}{lcccccc}")
    print("  & Mean (s.d.) & Median & Range & 2-line & 4-line & Longer \\\\ \\hline")
    print(f"  Offline optimum & {cells([r['dp'] for r in off])} & ${st.mean(r['dp_two_line'] for r in off):.1f}$ & "
          f"${st.mean(r['dp_four_line'] for r in off):.1f}$ & $0$ \\\\")
    print(f"  Offline greedy & {cells([r['greedy'] for r in off])} & & & \\\\")
    for mode, name in MODES:
        print(f"  {name} & {cells([r[mode + '_pcs'] for r in on])} & ${st.mean(r[mode + '_two_line'] for r in on):.1f}$ & "
              f"${st.mean(r[mode + '_four_line'] for r in on):.1f}$ & ${st.mean(r[mode + '_longer'] for r in on):.1f}$ \\\\")
    extra = [(m, name) for m, name in EXTRA if m + "_pcs" in on[0]]
    for mode, name in extra:
        print(f"  {name} & {cells([r[mode + '_pcs'] for r in on])} & ${st.mean(r[mode + '_two_line'] for r in on):.1f}$ & "
              f"${st.mean(r[mode + '_four_line'] for r in on):.1f}$ & ${st.mean(r[mode + '_longer'] for r in on):.1f}$ \\\\")
    print("\\end{tabular}\n")
    for mode, name in MODES + tuple(extra):
        strict = [r[mode + "_two_line"] + r[mode + "_four_line"] for r in on]
        print(f"% {name}, counting only 2- and 4-line PCs: mean {st.mean(strict):.2f}, "
              f"gap {st.mean(o['dp'] - x for o, x in zip(off, strict)):.2f}")
    for mode, name in extra:
        for base in ("myopic", "value"):
            d = [r[base + "_pcs"] - r[mode + "_pcs"] for r in on]
            print(f"% {base} minus {mode}: mean {st.mean(d):+.2f}, s.e. {st.stdev(d) / n ** 0.5:.2f}; "
                  f"more on {sum(x > 0 for x in d)}, equal on {sum(x == 0 for x in d)}, fewer on {sum(x < 0 for x in d)}")
        failed = sum(r[mode + "_failed"] for r in on)
        attempts = sum(r[mode + "_four_line"] + r[mode + "_failed"] for r in on)
        print(f"% {name}: failed 4-line attempts {failed} of {attempts} ({100 * failed / attempts:.1f}%), "
              f"gap to optimum {st.mean(o['dp'] - r[mode + '_pcs'] for o, r in zip(off, on)):.2f}")
    print()

    for mode, name in MODES:
        pcs = [r[mode + "_pcs"] for r in on]
        failed = [r[mode + "_failed"] for r in on]
        attempts = sum(r[mode + "_four_line"] + r[mode + "_failed"] for r in on)
        gap = [o["dp"] - p for o, p in zip(off, pcs)]
        lead = [p - o["greedy"] for o, p in zip(off, pcs)]
        clean = [k for k in range(n) if failed[k] == 0]
        rest = [k for k in range(n) if failed[k] > 0]
        mean = lambda ks, xs: st.mean(xs[k] for k in ks) if ks else float("nan")
        print(f"% {name}:")
        print(f"%   optimum minus online: mean {st.mean(gap):.2f}, median {st.median(gap):g}, range {min(gap)} to {max(gap)}; "
              f"equal to the optimum on {sum(g == 0 for g in gap)} seeds, above it on {sum(g < 0 for g in gap)}")
        print(f"%   online minus greedy: mean {st.mean(lead):+.2f} (s.e. {st.stdev(lead) / n ** 0.5:.2f}); "
              f"more on {sum(d > 0 for d in lead)}, equal on {sum(d == 0 for d in lead)}, fewer on {sum(d < 0 for d in lead)}")
        print(f"%   failed 4-line attempts: {sum(failed)} of {attempts} ({100 * sum(failed) / attempts:.1f}%)")
        print(f"%   {len(clean)} games with no failed attempt: mean {mean(clean, pcs):.2f} PCs, gap {mean(clean, gap):.2f}; "
              f"{len(rest)} with at least one: mean {mean(rest, pcs):.2f} PCs, gap {mean(rest, gap):.2f}")
    diff = [r["value_pcs"] - r["myopic_pcs"] for r in on]
    se = st.stdev(diff) / n ** 0.5
    print(f"% value minus myopic: mean {st.mean(diff):+.2f}, s.e. {se:.2f} (t = {st.mean(diff) / se:.1f}), "
          f"median {st.median(diff):+g}; more on {sum(d > 0 for d in diff)}, equal on {sum(d == 0 for d in diff)}, "
          f"fewer on {sum(d < 0 for d in diff)}")
    gm = st.mean(o["dp"] - r["myopic_pcs"] for o, r in zip(off, on))
    print(f"% share of the myopic gap that the value table recovers: {100 * st.mean(diff) / gm:.0f}%")
    print(f"% 2-line PCs per sequence: optimum {st.mean(r['dp_two_line'] for r in off):.2f}, "
          + ", ".join(f"{mode} {st.mean(r[mode + '_two_line'] for r in on):.2f}" for mode, _ in MODES))

    print("\n\\begin{tabular}{rcccc|rcccc}")
    print("  Seed & Greedy & Optimum & Myopic & Value & Seed & Greedy & Optimum & Myopic & Value \\\\ \\hline")
    half = (n + 1) // 2
    for a in range(half):
        row = []
        for k in (a, a + half):
            if k < n:
                s = seeds[k]
                row += [str(s), str(offline[s]["greedy"]), str(offline[s]["dp"]),
                        str(online[s]["myopic_pcs"]), str(online[s]["value_pcs"])]
            else:
                row += [""] * 5
        print("  " + " & ".join(row) + " \\\\")
    print("\\end{tabular}")


if __name__ == "__main__":
    main()
