// basic.cpp — a simple greedy offline strategy, for testing the visualiser.
//
// Whenever the board is empty it takes the shortest perfect clear it can find
// (2 lines, then 4, then 6) using findPerfectClear() from tetris.h. If none
// exists it places one piece with a "lowest, fewest holes" heuristic and tries
// again from the new board. It ignores the future entirely: it never asks
// which PC leaves the best hold piece or bag position for the next one.

#include <iostream>

#include "tetris.h"

using namespace tetris;

namespace {

// Lower is better: keep the stack low, flat and free of covered holes.
int score(const Game& game, const Move& m) {
    Board b = game.board();
    b.place(m.placement);
    int bumpiness = 0;
    for (int x = 0; x + 1 < Board::WIDTH; ++x) bumpiness += std::abs(b.columnHeight(x) - b.columnHeight(x + 1));
    return 100 * b.holes() + 10 * b.height() + bumpiness;
}

}  // namespace

std::vector<Move> solve(const Game& start) {
    Game game = start;
    while (!game.over()) {
        bool found = false;
        for (int height : {2, 4, 6}) {
            if (height < game.board().height()) continue;
            if (auto moves = findPerfectClear(game, height, 300'000)) {
                for (const Move& m : *moves) game.play(m);
                found = true;
                break;
            }
        }
        if (found) continue;

        std::vector<Move> options = game.legalMoves();
        if (options.empty()) break;  // nothing fits under the rules: stop here
        Move best = options[0];
        for (const Move& m : options)
            if (score(game, m) < score(game, best)) best = m;
        game.play(best);
    }
    std::cerr << "basic: " << game.pcCount() << " perfect clears\n";
    return game.history();
}
