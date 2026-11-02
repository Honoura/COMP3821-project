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
  strategies/dp_builtin.cpp  same DP using tetris.h's own PC search (cross-check)
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

## Visualiser

Requires Python 3 and a C++17 compiler (`c++`, or set `CXX` / `--cxx`).

```bash
python3 visualiser/server.py          # then open http://localhost:8000
```

- **Run a strategy:** drop a `.cpp` file on the Strategy card (or load
  the `basic.cpp` / `dp.cpp` examples), pick a seeded 7-bag or paste a custom queue,
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
  (piece key above the board); dashed lines mark 7-bag boundaries.
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
| `dp_builtin.cpp` | the same DP using `perfectClearOptions` from `tetris.h` | 21, 21, 20, 21 | ~17–20 s on 8 threads |

`dp.cpp` stops after its last PC, so it reports "strategy stopped" with a few
pieces unplaced.
