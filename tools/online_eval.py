#!/usr/bin/env python3
"""Run the online strategies on seeded sequences and compare them with the
offline results.

Builds simulator/strategies/online_myopic.cpp and online_value.cpp with the
runner, plays every seed with both, and prints a summary. Game logs go to
output/online/ (open them in the visualiser), and the summary to
output/online/summary.json. If results/offline_50.csv has a row for a seed,
the greedy baseline and the exact offline optimum are shown beside it.

    python3 tools/online_eval.py 1-50 [--jobs 4] [--csv results/online_50.csv]

To try another value table, add it as an extra algorithm. It runs the
value-guided strategy with that table file (see results/tables/):

    python3 tools/online_eval.py 1-50 --table hold_preference=results/tables/hold_preference.txt

To play only some of the algorithms, or to keep training games apart:

    python3 tools/online_eval.py 1001-1100 --only myopic --dir output/train

Seeds already played are skipped; delete the output directory to start again.
"""

import argparse
import csv
import json
import os
import statistics as st
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM = ROOT / "simulator"
STRATEGIES = ("myopic", "value")


def parse_seeds(text):
    seeds = []
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-")
            seeds.extend(range(int(a), int(b) + 1))
        else:
            seeds.append(int(part))
    return seeds


def build(cxx, out, strategies):
    for strategy in strategies:
        cmd = [cxx, "-std=c++17", "-O2", "-pthread", "-I", str(SIM / "include"), str(SIM / "runner" / "runner.cpp"),
               str(SIM / "strategies" / f"online_{strategy}.cpp"), "-o", str(out / f"run_{strategy}")]
        subprocess.run(cmd, check=True)


def play(job):
    name, strategy, table, seed, length, out = job
    log = out / f"{name}_seed{seed}.json"
    if not log.exists():
        tmp = out / f"{name}_seed{seed}.tmp"
        env = dict(os.environ)
        env.pop("ONLINE_TABLE", None)
        if table:
            env["ONLINE_TABLE"] = str(table)
        proc = subprocess.run([str(out / f"run_{strategy}"), "--seed", str(seed), "--length", str(length),
                               "--out", str(tmp)], capture_output=True, text=True, env=env)
        if proc.returncode != 0 or not tmp.exists():
            raise RuntimeError(f"{name} seed {seed} failed: {proc.stderr[-500:]}")
        game = json.loads(tmp.read_text())
        game["stderr"] = proc.stderr
        log.write_text(json.dumps(game))
        tmp.unlink()
    game = json.loads(log.read_text())
    if game["error"] is not None or game["endReason"] not in ("sequence finished", "strategy stopped"):
        raise RuntimeError(f"{name} seed {seed}: {game['endReason']} {game['error']}")
    lines = [p["lines"] for p in game["pcs"]]
    failed = 0
    for row in game.get("stderr", "").splitlines():
        if "failed 4-line attempts" in row:
            failed = int(row.split("online:")[1].split()[0])
    return name, seed, {"pcs": game["pcCount"], "two_line": lines.count(2), "four_line": lines.count(4),
                        "longer": sum(1 for x in lines if x > 4), "longest": max(lines, default=0),
                        "failed": failed, "seconds": round(game["solveMs"] / 1000, 1)}


def load_offline():
    path = ROOT / "results" / "offline_50.csv"
    if not path.exists():
        return {}
    with open(path) as f:
        rows = csv.DictReader(line for line in f if not line.startswith("#"))
        return {int(r["seed"]): {k: int(v) for k, v in r.items()} for r in rows}


def describe(xs):
    return f"{st.mean(xs):6.2f} {st.pstdev(xs):5.2f} {st.median(xs):6g}  {min(xs)}-{max(xs)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("seeds", help="e.g. 1-50 or 3,7,9")
    ap.add_argument("--length", type=int, default=200)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--csv", help="also write the per-seed results to this CSV file")
    ap.add_argument("--table", action="append", default=[], metavar="NAME=FILE",
                    help="extra algorithm: the value-guided strategy with this value table file (repeatable)")
    ap.add_argument("--only", help="comma-separated algorithms to play (default: all)")
    ap.add_argument("--dir", default=str(ROOT / "output" / "online"), help="directory for game logs")
    args = ap.parse_args()
    seeds = parse_seeds(args.seeds)
    out = Path(args.dir).resolve()
    out.mkdir(parents=True, exist_ok=True)

    # name -> (strategy, table file or None)
    algos = {"myopic": ("myopic", None), "value": ("value", None)}
    for spec in args.table:
        name, _, path = spec.partition("=")
        if not name or not path or not Path(path).exists():
            sys.exit(f"--table {spec}: expected NAME=FILE with an existing file")
        algos[name] = ("value", Path(path).resolve())
    if args.only:
        wanted = args.only.split(",")
        unknown = [w for w in wanted if w not in algos]
        if unknown:
            sys.exit(f"--only: unknown algorithm {unknown[0]} (have: {', '.join(algos)})")
        algos = {name: algos[name] for name in wanted}
    names = list(algos)
    build(args.cxx, out, sorted({strategy for strategy, _ in algos.values()}))

    results = {seed: {} for seed in seeds}
    jobs = [(name, *algos[name], seed, args.length, out) for seed in seeds for name in names]
    with ThreadPoolExecutor(args.jobs) as pool:
        for name, seed, r in pool.map(play, jobs):
            results[seed][name] = r
            if len(results[seed]) == len(names):
                print(f"seed {seed}: " + ", ".join(f"{n} {results[seed][n]['pcs']}" for n in names),
                      file=sys.stderr, flush=True)

    offline = load_offline() if args.length == 200 else {}
    known = [s for s in seeds if s in offline]
    width = max(26, max(len(n) for n in names) + 13)
    print(f"\n{len(seeds)} sequences of {args.length} pieces, 5-piece vision\n")
    print(f"{'':{width}}   mean    sd median  range   2-line 4-line longer")
    if known:
        for key, name in (("dp", "offline optimum (DP)"), ("greedy", "offline greedy baseline")):
            xs = [offline[s][key] for s in known]
            extra = f"   {st.mean(offline[s]['dp_two_line'] for s in known):5.1f}  {st.mean(offline[s]['dp_four_line'] for s in known):5.1f}" if key == "dp" else ""
            print(f"{name + f' ({len(known)})':{width}} {describe(xs)}{extra}")
    for name in names:
        rs = [results[s][name] for s in seeds]
        print(f"{'online ' + name + f' ({len(seeds)})':{width}} {describe([r['pcs'] for r in rs])}   "
              f"{st.mean(r['two_line'] for r in rs):5.1f}  {st.mean(r['four_line'] for r in rs):5.1f}  "
              f"{st.mean(r['longer'] for r in rs):5.1f}")
    print()
    for name in names:
        rs = [results[s][name] for s in seeds]
        attempts = sum(r["four_line"] + r["failed"] for r in rs)
        failed = sum(r["failed"] for r in rs)
        clean = [r["pcs"] for r in rs if r["failed"] == 0]
        rest = [r["pcs"] for r in rs if r["failed"] > 0]
        strict = st.mean(r["two_line"] + r["four_line"] for r in rs)
        print(f"{name}: {failed} of {attempts} 4-line attempts failed ({100 * failed / max(attempts, 1):.1f}%); "
              f"{len(clean)} games with none (mean {st.mean(clean) if clean else 0:.2f} PCs), "
              f"{len(rest)} with some (mean {st.mean(rest) if rest else 0:.2f}); "
              f"longest PC {max(r['longest'] for r in rs)} lines; counting only 2- and 4-line PCs {strict:.2f}; "
              f"mean time {st.mean(r['seconds'] for r in rs):.0f}s")
    if len(seeds) > 1:
        for a in range(len(names)):
            for b in range(a):
                diff = [results[s][names[a]]["pcs"] - results[s][names[b]]["pcs"] for s in seeds]
                se = st.stdev(diff) / len(diff) ** 0.5
                print(f"{names[a]} minus {names[b]}: mean {st.mean(diff):+.2f} (standard error {se:.2f}), "
                      f"median {st.median(diff):+g}; better on {sum(d > 0 for d in diff)}, "
                      f"equal on {sum(d == 0 for d in diff)}, worse on {sum(d < 0 for d in diff)}")
    if known:
        for name in names:
            gap = [offline[s]["dp"] - results[s][name]["pcs"] for s in known]
            vs_greedy = [results[s][name]["pcs"] - offline[s]["greedy"] for s in known]
            print(f"{name}: optimum minus online {st.mean(gap):.2f} (median {st.median(gap):g}, range {min(gap)} to {max(gap)}); "
                  f"online minus offline greedy {st.mean(vs_greedy):+.2f}")

    summary = {"length": args.length, "seeds": seeds, "algorithms": {n: str(algos[n][1] or "") for n in names},
               "per_seed": [{"seed": s, **({"offline": offline[s]} if s in offline else {}), **results[s]} for s in seeds]}
    (out / "summary.json").write_text(json.dumps(summary, indent=1))
    if args.csv:
        fields = ("pcs", "two_line", "four_line", "longer", "failed")
        with open(args.csv, "w", newline="") as f:
            f.write(f"# Online results, 5-piece vision, {args.length}-piece 7-bag sequences (tools/online_eval.py).\n"
                    "# Per algorithm: PCs, 2-line PCs, 4-line PCs, longer PCs, failed 4-line attempts.\n")
            w = csv.writer(f)
            w.writerow(["seed"] + [f"{name}_{k}" for name in names for k in fields])
            for s in seeds:
                w.writerow([s] + [results[s][name][k] for name in names for k in fields])


if __name__ == "__main__":
    main()
