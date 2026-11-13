// online_myopic.cpp — online algorithm with 5-piece vision (myopic).
//
// Sees only the board, the held piece, the current piece and the next five
// pieces, and maximises the chance of completing the next PC.
// The algorithm itself is in online.h.
//
// The moves it samples are random, so results vary a little with the sampling
// seed: set ONLINE_SEED to change it (default 1).

#include "online.h"

using namespace tetris;

std::vector<Move> solve(const Game& start) {
    online::Options options;
    options.mode = online::Mode::Myopic;
    options.vision = 5;
    return online::run(start, options);
}
