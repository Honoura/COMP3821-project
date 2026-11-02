// perfect_tetris.h — use the perfect-tetris solver (github.com/TemariVirus/
// perfect-tetris) as a PC checker from a strategy.
//
// Queries are sent to the `pc fumen` command as empty-board fumens whose
// comment holds the queue (#Q=[hold](current)next...); its answers come back
// as fumens and are replayed on a Game to recover the hold presses.
//
// The `pc` binary is found via $PERFECT_TETRIS_PC, or at
// perfect-tetris/zig-out/bin/pc relative to the current directory or one of
// its parents. Build it with Zig 0.14: `cd perfect-tetris && zig build
// -Doptimize=ReleaseFast`.
#pragma once

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "fumen.h"
#include "process.h"
#include "tetris.h"

namespace tetris {

// Plays `placements` from `game`, inferring when hold was pressed (fumen only
// records placements). If `endHold` is given, prefers a reading that leaves
// that piece in hold. Returns the moves, or std::nullopt if no reading is legal.
inline std::optional<std::vector<Move>> replayPlacements(const Game& game, const std::vector<Placement>& placements,
                                                         std::optional<Piece> endHold = std::nullopt) {
    std::optional<std::vector<Move>> fallback;
    std::vector<Move> path;
    std::function<bool(const Game&, size_t)> go = [&](const Game& g, size_t i) {
        if (i == placements.size()) {
            if (!endHold || g.held() == *endHold) return true;
            if (!fallback) fallback = path;
            return false;
        }
        for (bool hold : {false, true}) {
            Move m{hold, placements[i]};
            if (g.pieceAfter(hold) != m.placement.piece || (hold && g.held() == g.current())) continue;
            Game next = g;
            if (!next.play(m)) continue;
            path.push_back(m);
            if (go(next, i + 1)) return true;
            path.pop_back();
        }
        return false;
    };
    if (go(game, 0)) return path;
    return fallback;
}

class PerfectTetris {
   public:
    // Locates the `pc` binary (see the top of this file).
    static std::optional<std::string> findBinary() {
        if (const char* env = std::getenv("PERFECT_TETRIS_PC"); env && *env) return std::string(env);
        std::string prefix;
        for (int up = 0; up < 5; ++up, prefix += "../") {
            std::string path = prefix + "perfect-tetris/zig-out/bin/pc";
            if (std::ifstream(path).good()) return path;
        }
        return std::nullopt;
    }

    explicit PerfectTetris(std::string binary, std::string kicks = "srs")
        : binary_(std::move(binary)), kicks_(std::move(kicks)) {}

    struct Query {
        Piece hold;
        std::vector<Piece> queue;  // current piece first
    };

    enum class Status { Solved, NoSolution, TimedOut };
    struct Answer {
        Status status = Status::NoSolution;
        std::vector<Placement> placements;
    };

    // Solves a batch of queries in one process, all with the same minimum
    // height and end-hold piece (Piece::None = any). Gives up after
    // `timeoutSec` seconds (0 = no limit), marking unfinished queries TimedOut.
    // Safe to call from several threads. Throws std::runtime_error if the
    // binary cannot be run or its output is unreadable.
    std::vector<Answer> solve(const std::vector<Query>& queries, int minHeight, Piece save = Piece::None,
                              double timeoutSec = 0) const {
        std::vector<Answer> out(queries.size());
        if (queries.empty()) return out;
        std::vector<std::string> args = {binary_, "fumen", "-k", kicks_, "-m", std::to_string(minHeight)};
        if (save != Piece::None) args.insert(args.end(), {"-s", std::string(1, toChar(save))});
        for (const Query& q : queries) args.push_back(fumen::encodeEmptyPage(fumen::quizComment(q.hold, q.queue)));

        auto [text, finished] = runProcess(args, timeoutSec);
        if (!finished) text.erase(text.rfind('\n') == std::string::npos ? 0 : text.rfind('\n') + 1);  // drop a cut-off line
        std::vector<std::string> lines;
        for (size_t i = 0, j; i < text.size(); i = j + 1) {
            j = text.find('\n', i);
            if (j == std::string::npos) j = text.size();
            if (j > i) lines.push_back(text.substr(i, j - i));
        }
        if (finished && lines.size() != queries.size())
            throw std::runtime_error("perfect-tetris: unexpected output: " + (lines.empty() ? text : lines[0]));
        for (size_t i = 0; i < queries.size(); ++i) {
            if (i >= lines.size()) {
                out[i].status = Status::TimedOut;
            } else if (lines[i].rfind("Unable to solve", 0) != 0) {
                auto placements = fumen::decodePlacements(lines[i]);
                if (!placements) throw std::runtime_error("perfect-tetris: bad fumen: " + lines[i]);
                out[i] = {Status::Solved, std::move(*placements)};
            }
        }
        return out;
    }

   private:
    std::string binary_, kicks_;
};

}  // namespace tetris
