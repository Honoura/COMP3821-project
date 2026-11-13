#!/usr/bin/env python3
"""Fit the value table to online play and write it as a table file.

The table fitted by tools/fit_value.py comes from the offline optimum, which
knows the whole sequence (hindsight bias). This tool fits the same model to
what an online algorithm actually achieves:

    V(i, h) = alpha * (pieces left) + theta(h, b),    b = (i - 1) mod 7

Input: game logs of the *myopic* online algorithm on training sequences. The
myopic algorithm has no preference between held pieces, so its games visit
every (held piece, bag offset) state. Using its value to choose moves is one
step of policy improvement (a rollout policy).

    python3 tools/online_eval.py 1001-1100 --only myopic --dir output/train
    python3 tools/fit_value_online.py output/train/myopic_seed*.json --out results/tables/online_fitted.txt
    python3 tools/online_eval.py 1-50 --table online_fitted=results/tables/online_fitted.txt

Use training seeds that are not evaluation seeds.

How the fit works. At every PC boundary of every game we know the state
(i, h) and what happened next.
  * alpha and the overall level come from a straight-line fit of "PCs made
    from here to the end" against "pieces left".
  * The effect of the held piece and of the bag offset comes from the number
    of PCs made in the next `--window` pieces (35 by default: five bags, so
    the bag offset is the same again at the end of the window). A short window
    is used because the held piece only matters for the next PC or two, and
    counting PCs to the end of the game would add noise without adding signal.
    The model is additive, theta(h, b) = level + hold(h) + offset(b): there is
    too little data to fit all 49 combinations separately.
"""

import argparse
import json
import statistics as st
from collections import defaultdict
from pathlib import Path

PIECES = "IOTLJSZ"  # the order of tetris.h


def load(paths, window):
    to_go, local = [], []  # (pieces left, PCs to the end); (hold, offset, PCs in the window)
    games = 0
    for path in paths:
        with open(path) as f:
            g = json.load(f)
        if g.get("error") is not None:
            continue
        games += 1
        n = len(g["queue"])
        ends = [p["position"] for p in g["pcs"]]
        for k, p in enumerate(g["pcs"]):
            i, h = p["position"], p.get("hold")
            if not h or h not in PIECES:
                continue
            to_go.append((n - i, len(ends) - k - 1))
            if i + window <= n - 10:  # the window must not run into the end of the game
                local.append((h, (i - 1) % 7, sum(1 for e in ends[k + 1:] if e <= i + window)))
    return games, to_go, local


def line_fit(points):
    mx = st.mean(x for x, _ in points)
    my = st.mean(y for _, y in points)
    slope = sum((x - mx) * (y - my) for x, y in points) / sum((x - mx) ** 2 for x, _ in points)
    return slope, my - slope * mx


def additive_fit(local, sweeps=200):
    """y = mean + hold[h] + offset[b], least squares, both effects centred on zero."""
    mean = st.mean(y for _, _, y in local)
    hold = {h: 0.0 for h in PIECES}
    offset = {b: 0.0 for b in range(7)}
    by_h, by_b = defaultdict(list), defaultdict(list)
    for h, b, y in local:
        by_h[h].append((b, y))
        by_b[b].append((h, y))
    for _ in range(sweeps):
        for h, rows in by_h.items():
            hold[h] = st.mean(y - mean - offset[b] for b, y in rows)
        for b, rows in by_b.items():
            offset[b] = st.mean(y - mean - hold[h] for h, y in rows)
    for effect in (hold, offset):
        shift = st.mean(effect.values())
        for k in effect:
            effect[k] -= shift
    resid = [y - mean - hold[h] - offset[b] for h, b, y in local]
    sd = st.pstdev(resid)
    se_hold = {h: sd / len(by_h[h]) ** 0.5 if by_h[h] else float("nan") for h in PIECES}
    se_offset = {b: sd / len(by_b[b]) ** 0.5 if by_b[b] else float("nan") for b in range(7)}
    counts = {h: len(by_h[h]) for h in PIECES}
    return hold, offset, se_hold, se_offset, counts, sd


def write_table(path, alpha, theta, comment):
    lines = ["# " + c for c in comment]
    lines.append("# First number: alpha (PCs per piece). Then theta: one row per held piece")
    lines.append("# (I, O, T, L, J, S, Z), one column per bag offset 0..6.")
    lines.append(f"{alpha:.5f}")
    for h in PIECES:
        lines.append(" ".join(f"{theta[(h, b)]:+.4f}" for b in range(7)) + f"   # {h}")
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="game logs of the myopic online algorithm (training seeds)")
    ap.add_argument("--window", type=int, default=35, help="pieces after a boundary over which PCs are counted")
    ap.add_argument("--out", help="table file to write")
    args = ap.parse_args()

    games, to_go, local = load(args.logs, args.window)
    alpha, level = line_fit(to_go)
    hold, offset, se_hold, se_offset, counts, sd = additive_fit(local)
    theta = {(h, b): level + hold[h] + offset[b] for h in PIECES for b in range(7)}

    print(f"{games} games, {len(to_go)} PC boundaries, {len(local)} with a full {args.window}-piece window")
    print(f"alpha = {alpha:.4f} PCs per piece, level = {level:+.3f}; residual s.d. {sd:.2f} PCs")
    print("held piece   effect   s.e.   boundaries")
    for h in sorted(PIECES, key=lambda p: -hold[p]):
        print(f"    {h}       {hold[h]:+.3f}   {se_hold[h]:.3f}   {counts[h]:5d}")
    print("bag offset   effect   s.e.")
    for b in range(7):
        print(f"    {b}       {offset[b]:+.3f}   {se_offset[b]:.3f}")
    print(f"range between held pieces: {max(hold.values()) - min(hold.values()):.2f} PCs")
    if args.out:
        write_table(args.out, alpha, theta, [
            f"Value table fitted to online play by tools/fit_value_online.py: {games} games of the myopic",
            f"online algorithm, {len(local)} PC boundaries, PCs counted over the next {args.window} pieces."])
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
