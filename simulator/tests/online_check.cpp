// Checks for online.h. Takes under a minute.
//
//     cd simulator
//     c++ -std=c++17 -O2 -Iinclude tests/online_check.cpp -o online_check && ./online_check
#include <iostream>

#include "online.h"

using namespace tetris;
using namespace tetris::online;

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

// Plays `moves` on a fresh game with full legality checks.
static bool replays(const Game& start, const std::vector<Move>& moves, Game* end = nullptr) {
    Game game = start;
    for (const Move& m : moves)
        if (!game.play(m)) return false;
    if (end) *end = game;
    return true;
}

// findPC on an empty board with nothing in hold: `pieces` are the current
// piece and the queue; `end` is the piece to leave in hold (None = no hold).
static PCAnswer search(const std::string& pieces, int rows, Piece end) {
    std::vector<Piece> q = parseQueue(pieces);
    Piece cur = q.front();
    q.erase(q.begin());
    return findPC(Board(), Piece::None, cur, q, rows, {end});
}

int main() {
    std::cout << "findPC\n";
    {
        PCAnswer a = search("IIIIO", 2, Piece::None);
        check(a.ends.size() == 1 && a.ends[0].moves.size() == 5, "IIIIO makes a 2-line PC without hold");
        Game end;
        check(!a.ends.empty() && replays(Game(parseQueue("IIIIO")), a.ends[0].moves, &end) && end.pcCount() == 1,
              "its moves are legal and clear the board");

        a = search("ISIIIO", 2, Piece::S);
        check(a.ends.size() == 1 && a.ends[0].endHold == Piece::S, "ISIIIO makes a 2-line PC by holding the S");
        check(!a.ends.empty() && replays(Game(parseQueue("ISIIIO")), a.ends[0].moves, &end) && end.pcCount() == 1 &&
                  end.held() == Piece::S,
              "its moves are legal and end with S in hold");

        a = search("ISIIIO", 2, Piece::I);
        check(a.ends.empty() && a.proved, "ISIIIO has no 2-line PC that places the S (2-line PCs use only I, J, L, O)");
        a = search("IIIJO", 2, Piece::None);
        check(a.ends.empty() && a.proved, "IIIJO has no 2-line PC (odd number of L and J)");
        a = search("TSZTS", 2, Piece::None);
        check(a.ends.empty() && a.proved, "TSZTS has no 2-line PC");

        // The standard opener: the first bag plus three pieces of the second.
        a = search("IOTLJSZIOT", 4, Piece::None);
        check(a.ends.size() == 1 && a.ends[0].moves.size() == 10, "IOTLJSZIOT makes a 4-line PC");
        check(!a.ends.empty() && replays(Game(parseQueue("IOTLJSZIOT")), a.ends[0].moves, &end) && end.pcCount() == 1,
              "its moves are legal and clear the board");

        a = findPC(Board(), Piece::None, Piece::I, parseQueue("OTLJSZIOTL"), 4, {Piece::S}, 1, 50);
        check(a.ends.empty() ? !a.proved : true, "a search stopped by its budget does not claim a proof");
    }

    std::cout << "limited vision\n";
    {
        Game start(sevenBag(1, 60));
        int widest = 0, views = 0;
        bool consistent = true;
        Game shadow = start;
        std::vector<Move> moves = playOnline(start, 5, [&](const View& v) -> std::optional<Move> {
            widest = std::max(widest, static_cast<int>(v.preview.size()));
            ++views;
            // The view's own move list must agree with the game's.
            consistent = consistent && v.legalMoves().size() == shadow.legalMoves().size();
            std::vector<Move> legal = v.legalMoves();
            if (legal.empty()) return std::nullopt;
            shadow.play(legal[0]);
            return legal[0];
        });
        check(widest == 5, "a policy is shown at most 5 upcoming pieces");
        check(consistent && views > 0, "moves worked out from a View match Game::legalMoves()");
        check(declaredVision() == 5, "the vision is recorded for the game log");
    }

    std::cout << "the two algorithms (70 pieces of seed 1)\n";
    for (Mode mode : {Mode::Myopic, Mode::Value}) {
        std::string name = mode == Mode::Myopic ? "myopic" : "value-guided";
        Game start(sevenBag(1, 70));
        Options options;
        options.mode = mode;
        Player first(options), second(options);
        std::vector<Move> a = playOnline(start, options.vision, [&](const View& v) { return first.choose(v); });
        std::vector<Move> b = playOnline(start, options.vision, [&](const View& v) { return second.choose(v); });
        Game end;
        check(replays(start, a, &end), name + ": every move is legal");
        check(end.pcCount() >= 4, name + ": at least 4 perfect clears (" + std::to_string(end.pcCount()) + ")");
        bool same = a.size() == b.size();
        for (size_t k = 0; same && k < a.size(); ++k)
            same = a[k].hold == b[k].hold && a[k].placement.piece == b[k].placement.piece &&
                   a[k].placement.rot == b[k].placement.rot && a[k].placement.x == b[k].placement.x &&
                   a[k].placement.y == b[k].placement.y;
        check(same, name + ": the same sampling seed gives the same game");
    }

    std::cout << "value table\n";
    {
        ValueTable v;
        check(v.alpha > 0.09 && v.alpha < 0.12, "about 0.1 PCs per piece");
        check(v(200, Piece::O, 200) < v(100, Piece::O, 200), "fewer pieces left, lower value");
    }

    std::cout << (failures ? "FAILED: " + std::to_string(failures) + " checks\n" : "all checks passed\n");
    return failures ? 1 : 0;
}
