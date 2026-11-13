# COMP3821-project

Perfect-clear (PC) sequencing over a 200-piece Tetris queue.

## Layout

```
simulator/
  include/tetris.h        helper library every strategy includes
  runner/runner.cpp       runs a strategy, verifies every move, writes a JSON game log
  strategies/basic.cpp    example greedy strategy (myopic baseline)
  include/perfect_tetris.h  calls the perfect-tetris solver from a strategy
  strategies/dp.cpp       exact offline DP over PC boundaries, perfect-tetris as PC checker
  strategies/dp_builtin.cpp  same DP using tetris.h's own PC search (slow; cross-check)
  strategies/dp_search.cpp   same DP using online.h's exhaustive PC search: no external solver, no time limits
  strategies/sfinder_dp.cpp  DP with solution-finder finding the PC options
  include/sfinder.h       solution-finder wrapper (path/percent, solution -> moves)
  include/online.h        the online setting: limited-vision game loop, PC search, both online algorithms
  include/value_table.h   fitted value table for the value-guided algorithm (generated)
  strategies/online_myopic.cpp  online, 5-piece vision: maximise the chance of the next PC
  strategies/online_value.cpp   online, 5-piece vision: also weigh the state the PC ends in
  tests/online_check.cpp  checks for online.h
tools/fetch-sfinder.sh    downloads solution-finder into tools/sfinder/
tools/online_eval.py      runs both online strategies over a range of seeds and summarises
tools/fit_value.py        fits value_table.h to offline DP results
tools/fit_value_online.py fits a value table to online game logs instead (experimental)
tools/online_tables.py    prints the report's online tables from the two results files
results/offline_50.csv    greedy and offline DP results for seeds 1-50 (the report's table)
results/online_50.csv     online results for the same seeds
results/tables/           value tables as text files (the fitted one, an earlier one, the hold-preference ablation)
visualiser/
  server.py               local server: web UI + compiles/runs uploaded strategies
  public/                 the web UI (board, timeline, fumen import/export)
hydra/, perfect-tetris/   existing solvers (git submodules, for reference)
```

## Building perfect-tetris

`dp.cpp` needs the `pc` binary. Build it with Zig **0.14** (newer Zig versions
break the build):

```bash
cd perfect-tetris && zig build -Doptimize=ReleaseFast   # -> zig-out/bin/pc
```

The server finds it automatically; from the command line set
`PERFECT_TETRIS_PC` or run from inside the repo.

## Getting solution-finder

`sfinder_dp.cpp` needs Java 8+ and solution-finder:

```bash
./tools/fetch-sfinder.sh      # -> tools/sfinder/sfinder.jar
```

## Visualiser

Requires Python 3 and a C++17 compiler (`c++`, or set `CXX` / `--cxx`).

```bash
python3 visualiser/server.py          # then open http://localhost:8000
```

- **Run a strategy:** drop a `.cpp` file on the Strategy card (or load
  the `basic.cpp` / `dp.cpp` / `online_*.cpp` examples), pick a seeded 7-bag or paste a custom queue,
  and press *Run strategy*. The server compiles it with `tetris.h` and the
  runner, then replays the returned moves. An illegal move stops the replay
  and is marked in red on the timeline.
- **View a fumen:** paste a fumen string or URL (`v115@…`) into the Fumen card,
  or open `http://localhost:8000/#v115@…`. Quiz comments in perfect-tetris's
  `#Q=[hold](current)next…` format fill the hold and next panels.
- **Export:** *Copy game as fumen* writes one page per piece, with the whole
  queue as a quiz comment, so you can open it in fumen.zui.jp or feed it to
  perfect-tetris.
- **Queue:** the column beside the board lists every upcoming piece as a letter
  (piece key above the board); dashed lines mark 7-bag boundaries. For a
  limited-vision strategy, the pieces it could not see yet are dimmed.
- **Keys:** Space play/pause, ←/→ step, Shift+←/→ previous/next PC, Home/End.

The fumen viewer also works without the server: open
`visualiser/public/index.html` directly.

## Writing a strategy

A strategy is one `.cpp` file that defines:

```cpp
#include "tetris.h"
using namespace tetris;

std::vector<Move> solve(const Game& start) {
    Game game = start;                 // the whole 200-piece queue is in game.queue()
    while (!game.over()) {
        auto moves = game.legalMoves();  // every reachable placement, with/without hold
        if (moves.empty()) break;
        game.play(moves[0]);
    }
    return game.history();
}
```

Useful parts of `tetris.h`:

| Helper | What it does |
| --- | --- |
| `Game` | state (`board()`, `current()`, `held()`, `queue()`, `peek(k)`, `taken()`, `pcCount()`), `legalMoves()`, `play()` |
| `legalPlacements(board, piece, cap)` | every resting position reachable with SRS shifts, rotations and soft drops |
| `findPerfectClear(game, height)` | DFS for a PC of exactly `height` lines from the current state (heights 1–6) |
| `perfectClearOptions(game, height)` | every distinct hold piece such a PC can end with, with moves for each (DP transitions) |
| `Board` | bitboard with `fits`, `place`, `height`, `holes`, `toString` |
| `sevenBag(seed, n)`, `parseQueue(s)` | piece sequences |
| `parityOffset(placement)` | the column-colour offset from the parity lemmas |

A `Move` is `{hold, Placement{piece, rot, x, y}}`, where `(x, y)` is the
rotation centre in the same convention as fumen. Anything the strategy prints
to stdout/stderr shows in the visualiser's console.

## Command line

```bash
cd simulator
c++ -std=c++17 -O2 -pthread -Iinclude runner/runner.cpp strategies/dp.cpp -o run
./run --seed 42 --length 200 --out game.json
./run --queue "IOTLJSZ..." --cap 4 --no-hold --out game.json
```

## Strategies

| Strategy | Idea | Seeds 1–4 (PCs) | Time |
| --- | --- | --- | --- |
| `basic.cpp` | take the shortest PC available, else place greedily | 19, 20, 20, 20 | < 1 s |
| `dp.cpp` | exact DP over (pieces placed, hold) states; every (height, end hold) combination checked with perfect-tetris | 21 (seed 1) | ~80 s on 8 threads |
| `sfinder_dp.cpp` | same DP; sfinder lists 2-line PCs and decides 4-line options, perfect-tetris supplies 4-line moves | 21 (seed 1) | ~5 min on 8 threads |
| `dp_builtin.cpp` | the same DP using `perfectClearOptions` from `tetris.h` | — | too slow since its search was made sound |
| `dp_search.cpp` | same DP as `dp.cpp`; the PC checker is the exhaustive search of `online.h`, with no limits | 20, 21, 21, 19 | ~85 s on 1 thread |
| `online_myopic.cpp` | 5-piece vision; samples the hidden pieces and maximises the chance of the next PC | 19, 19, 20, 19 | ~50 s on 1 thread |
| `online_value.cpp` | 5-piece vision; same, but scores each PC by the fitted value of the state it ends in | 19, 20, 21, 19 | ~60 s on 1 thread |

`dp.cpp` stops after its last PC, so it reports "strategy stopped" with a few
pieces unplaced.

## Online setting

`online_myopic.cpp` and `online_value.cpp` play with 5-piece vision: besides
the board, the held piece and the current piece, they see only the next 5
pieces. Both are thin wrappers around `include/online.h`.

```bash
cd simulator
c++ -std=c++17 -O2 -pthread -Iinclude runner/runner.cpp strategies/online_value.cpp -o run_online
./run_online --seed 1 --length 200 --out game.json       # about 40 s
```

**The vision limit is enforced by the code, not by convention.** `playOnline()`
in `online.h` owns the `Game`. The algorithm is called once per piece with a
`View` (board, hold, current piece, at most 5 upcoming pieces, counters) and
never receives the `Game` or the queue. The runner still verifies every move,
and writes `"vision": 5` into the game log, so the visualiser dims the part of
the queue the algorithm could not see.

**How the algorithms decide.**

- They remember every piece seen so far, so they know which pieces of the
  current 7-bag are still to come.
- A 2-line PC places 5 pieces, so it is always completely visible and is found
  exactly.
- A 4-line PC places 10 pieces, and up to 4 of them are hidden when it starts.
  The algorithm samples the hidden pieces from the 7-bag (40-120 futures, or
  every possible future when at most 2 pieces are hidden), solves a few
  sampled futures with full knowledge and takes the first moves of those
  solutions as candidates. Every candidate is scored on every sampled future,
  and the best average is played. One piece is revealed per placement, so
  after about 4 placements the rest of the PC is visible and is solved
  exactly.
- `online_myopic.cpp` scores a future 1 if the PC can still be completed and 0
  if not, and takes a visible 2-line PC whenever there is one.
- `online_value.cpp` scores a completed PC `1 + V(i, h)`, where `(i, h)` is the
  boundary state it ends in (pieces drawn, held piece) and `V` is the value
  table: an estimate of how many further PCs the offline optimum makes from
  that state. So it also chooses *which* PC to make and what to leave in hold.
- If the 4-line PC becomes impossible, they aim for a 6-line PC in the same
  way, then for any PC that fits in 6 rows, and otherwise stack with a
  heuristic until one is possible. Because of this the online algorithms
  sometimes make PCs of 6 or more lines, which the offline DP (2- and 4-line
  PCs only) never does.

Sampling uses its own random seed (1 by default, `ONLINE_SEED=n` to change),
so a game is reproducible.

**The value table.** `V(i, h) = alpha * (pieces left) + theta(h, b)`, where `b`
is the bag offset `(i - 1) mod 7`. `value_table.h` is generated by
`tools/fit_value.py` from offline DP results: the exact number of further PCs
from every boundary state of 39 training sequences (seeds 1001-1039, 7583
states), solved with `dp_search.cpp`. To refit:

```bash
cd simulator
c++ -std=c++17 -O2 -pthread -Iinclude runner/runner.cpp strategies/dp_search.cpp -o run_dp
mkdir -p ../output/values
for s in $(seq 1001 1040); do DP_DUMP=../output/values/seed$s.json ./run_dp --seed $s --out /dev/null; done
python3 ../tools/fit_value.py ../output/values/*.json
```

`dp.cpp` writes the same files when `DP_DUMP` is set (that code compiles but
has not been run). Use training seeds that are not evaluation seeds. The value
is fitted to the offline optimum, which knows the whole sequence, so it is
optimistic (hindsight bias).

A table can also be given as a text file, without recompiling:
`ONLINE_TABLE=results/tables/hold_preference.txt ./run_online --seed 1`.

**Evaluation.**

```bash
python3 tools/online_eval.py 1-50 --jobs 4 \
    --table hold_preference=results/tables/hold_preference.txt \
    --table offline_10seq=results/tables/offline_10seq.txt \
    --csv results/online_50.csv          # about a minute per game
```

It plays every algorithm on every seed, skips games already played, and
compares with `results/offline_50.csv`. Each `--table name=file` adds the
value-guided strategy with that table as another algorithm.

Results on seeds 1-50, 200 pieces:

| | Mean PCs (s.d.) | Range | 2-line / 4-line / longer per game | Failed 4-line attempts |
| --- | --- | --- | --- | --- |
| Offline optimum | 20.84 (0.83) | 19-23 | 3.1 / 17.7 / 0 | |
| Offline greedy (`basic.cpp`) | 19.38 (0.52) | 18-20 | 0.5 / 18.7 / 0.2 | |
| Online, value-guided | 19.66 (0.82) | 18-21 | 1.3 / 17.9 / 0.4 | 22 of 917 (2.4%) |
| Online, myopic | 18.98 (0.68) | 17-20 | 0.5 / 17.6 / 0.8 | 42 of 923 (4.6%) |
| Ablation: hold preference only | 19.42 (0.64) | 18-21 | 0.9 / 18.0 / 0.5 | 27 of 926 (2.9%) |
| Earlier table (10 sequences) | 19.60 (0.85) | 18-22 | 1.3 / 17.8 / 0.5 | 27 of 917 (2.9%) |

- The value-guided algorithm is 1.18 PCs below the optimum on average and
  equals it on 15 of the 50 seeds; the myopic one is 1.86 below and equals it
  on 5. Neither exceeds it.
- Value-guided minus myopic: +0.68 PCs (standard error 0.14).
- The fitted table is nearly flat (held pieces differ by at most 0.10 PCs),
  but its ranking matters: the value-guided algorithm ends 81% of its PCs
  holding I, J, L or O, the only pieces a 2-line PC can use, against 51%.
- Ablation: a table with no fitted values, only "I, J, L, O in hold is worth
  0.05 more", gets 0.44 of that 0.68. The fitted table is 0.24 ahead of it
  (standard error 0.13), so not clearly better on 50 seeds.
- Counting only 2- and 4-line PCs (what the DP is allowed), the online
  algorithms make 19.24 and 18.14.

`python3 tools/online_tables.py` prints these numbers and the report's LaTeX
tables from the two CSV files.

**Check of the offline optima.** `dp_search.cpp` was run on seeds 1-50
(about 85 s per seed on one core). It agrees with `dp.cpp` (the report's
per-seed table) on 49 seeds, in the number of PCs and the 2-/4-line mix. On
**seed 38** `dp.cpp` reported 19 PCs and `dp_search.cpp` finds 20: twenty
4-line PCs that place all 200 pieces and never use hold. The runner verifies
that plan (`./run_dp --seed 38`). `results/offline_50.csv` records 20. Why
`dp.cpp` misses it is not known yet; it is the only plan that ends on the last
piece of the sequence with the hold slot empty.

**Checks.**

```bash
cd simulator
c++ -std=c++17 -O2 -Iinclude tests/online_check.cpp -o online_check && ./online_check
```
