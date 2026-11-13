// online_value.cpp — online algorithm with 5-piece vision (value-guided).
//
// Sees only the board, the held piece, the current piece and the next five
// pieces, and maximises 1 + V of the state the next PC ends in.
// The algorithm itself is in online.h; the value table V is in value_table.h.
//
// The moves it samples are random, so results vary a little with the sampling
// seed: set ONLINE_SEED to change it (default 1).

#include "online.h"

using namespace tetris;

std::vector<Move> solve(const Game& start) {
    online::Options options;
    options.mode = online::Mode::Value;
    options.vision = 5;
    return online::run(start, options);
}
