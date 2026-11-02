// sfinder.h — use solution-finder (github.com/knewjade/solution-finder) to
// list every perfect clear available from an empty board.
//
// For each queue we run `sfinder path` (unique solutions, CSV keyed by
// pattern, the same CSV that sfinder-strict-minimal reads). sfinder reports
// each solution only as a coloured picture of the cleared lines, with no
// placement order and with pieces placed after a line clear drawn split
// around it. realise() turns a picture back into moves: it searches orders
// and hold presses from the actual game state, using our own legal-move
// generator, so every move it returns is legal under tetris.h's rules.
//
// Needs Java 8+ and sfinder.jar, found via $SFINDER_JAR or at
// tools/sfinder/sfinder.jar relative to the current directory or one of its
// parents (run tools/fetch-sfinder.sh to download it).
#pragma once

#include <array>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <stdexcept>
#include <string>
#include <vector>

#include "fumen.h"
#include "process.h"
#include "tetris.h"

namespace tetris {

class SFinder {
   public:
    // Locates sfinder.jar (see the top of this file).
    static std::optional<std::string> findJar() {
        if (const char* env = std::getenv("SFINDER_JAR"); env && *env) return std::string(env);
        std::string prefix;
        for (int up = 0; up < 5; ++up, prefix += "../") {
            std::string path = prefix + "tools/sfinder/sfinder.jar";
            if (std::ifstream(path).good()) return path;
        }
        return std::nullopt;
    }

    explicit SFinder(std::string jar, std::string java = "java", int threads = 1)
        : jar_(std::move(jar)), java_(std::move(java)), threads_(threads) {}

    // A queue for a PC from an empty board: the held piece (if any), then the
    // current piece and the next pieces. sfinder has no notion of a piece
    // already in hold, so the held piece goes at the front of the pattern:
    // with an empty hold that gives exactly the same placement orders.
    struct Query {
        Piece hold;
        std::vector<Piece> queue;
        std::string pattern() const {
            std::string s;
            if (hold != Piece::None) s += toChar(hold);
            for (Piece p : queue) s += toChar(p);
            return s;
        }
    };

    // One PC: the cleared lines as fumen piece ids, picture[y][x] with
    // y = 0 the bottom row (pieces placed after a line clear appear split).
    struct Solution {
        std::string fumen;
        std::vector<std::array<uint8_t, Board::WIDTH>> picture;
        std::string order;  // one valid placement order, from sfinder's comment
    };

    struct Answer {
        bool complete = true;  // false if sfinder timed out before answering
        std::vector<Solution> solutions;
    };

    // Every distinct PC of exactly `height` lines from an empty board, for each
    // query. All queries go to one sfinder process (its setup cost is paid
    // once). Throws std::runtime_error if sfinder cannot be run.
    std::vector<Answer> solve(const std::vector<Query>& queries, int height, double timeoutSec = 0) const {
        std::vector<Answer> out(queries.size());
        if (queries.empty()) return out;
        std::string patterns;
        for (const Query& q : queries) patterns += (patterns.empty() ? "" : ";") + q.pattern();
        std::vector<std::string> args = {java_, "-jar", jar_, "path", "-t", "v115@vhAAgH", "-p", patterns,
                                         "-c", std::to_string(height), "-f", "csv", "-k", "pattern",
                                         "-L", "1", "-o", "-", "-lp", "/dev/null", "-th", std::to_string(threads_)};
        auto [text, finished] = runProcess(args, timeoutSec);
        if (!finished) {
            for (Answer& a : out) a.complete = false;
            return out;
        }

        // CSV columns: pattern, solution count, pieces used, piece unused,
        // fumens separated by ';'. sfinder may list a pattern twice
        // (solution-finder issue #3); the rows are identical.
        std::map<std::string, std::vector<std::string>> byPattern;
        size_t lineStart = text.find('\n');  // skip the header
        if (lineStart == std::string::npos) throw std::runtime_error("sfinder: unexpected output: " + text.substr(0, 300));
        for (size_t i = lineStart + 1, j; i < text.size(); i = j + 1) {
            j = text.find('\n', i);
            if (j == std::string::npos) j = text.size();
            std::string line = text.substr(i, j - i);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> cols;
            for (size_t a = 0, b; a <= line.size(); a = b + 1) {
                b = line.find(',', a);
                if (b == std::string::npos) b = line.size();
                cols.push_back(line.substr(a, b - a));
            }
            if (cols.size() < 5) throw std::runtime_error("sfinder: bad CSV row: " + line);
            std::vector<std::string> fumens;
            for (size_t a = 0, b; a < cols[4].size(); a = b + 1) {
                b = cols[4].find(';', a);
                if (b == std::string::npos) b = cols[4].size();
                if (b > a) fumens.push_back(cols[4].substr(a, b - a));
            }
            byPattern[cols[0]] = std::move(fumens);
        }

        for (size_t k = 0; k < queries.size(); ++k) {
            auto it = byPattern.find(queries[k].pattern());
            if (it == byPattern.end()) continue;  // sfinder skips queues too short to clear `height` lines
            for (const std::string& f : it->second) {
                auto pages = fumen::decode(f);
                if (!pages || pages->empty()) throw std::runtime_error("sfinder: bad fumen " + f);
                Solution s{f, {}, (*pages)[0].comment};
                for (int y = 0; y < height; ++y) s.picture.push_back((*pages)[0].field[y]);
                out[k].solutions.push_back(std::move(s));
            }
        }
        return out;
    }

    // Whether a PC of exactly `height` lines from an empty board is possible
    // for each pattern (a string of piece letters; all the same length), via
    // `sfinder percent`. With hold, sfinder may leave one piece unplaced;
    // without hold (`hold` = false) it places the pieces strictly in order.
    // Returns std::nullopt if sfinder did not finish within the time limit.
    std::optional<std::vector<bool>> possible(const std::vector<std::string>& patterns, int height, bool hold,
                                              double timeoutSec = 0) const {
        std::vector<bool> out(patterns.size(), true);
        if (patterns.empty()) return out;
        std::string joined;
        for (const std::string& p : patterns) {
            if (p.size() != patterns[0].size()) throw std::invalid_argument("sfinder: patterns must have equal length");
            joined += (joined.empty() ? "" : ";") + p;
        }
        std::vector<std::string> args = {java_, "-jar", jar_, "percent", "-t", "v115@vhAAgH", "-p", joined,
                                         "-c", std::to_string(height), "-H", hold ? "use" : "avoid",
                                         "-fc", "-1", "-lp", "/dev/null", "-th", std::to_string(threads_)};
        auto [text, finished] = runProcess(args, timeoutSec);
        if (!finished) return std::nullopt;
        size_t at = text.find("Fail pattern (all)");
        if (text.find("success =") == std::string::npos || at == std::string::npos)
            throw std::runtime_error("sfinder: unexpected percent output: " + text.substr(0, 300));
        // Failed patterns are listed one per line as "[T, T, L, J, O, I]".
        std::set<std::string> failed;
        for (size_t i = text.find('\n', at); i != std::string::npos && i + 1 < text.size(); i = text.find('\n', i + 1)) {
            size_t end = text.find('\n', i + 1);
            std::string line = text.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
            if (line.empty() || line[0] != '[') {
                if (line.rfind("#", 0) == 0) break;
                continue;
            }
            std::string letters;
            for (char c : line)
                if (fromChar(c) != Piece::None) letters += c;
            failed.insert(letters);
        }
        for (size_t k = 0; k < patterns.size(); ++k) out[k] = !failed.count(patterns[k]);
        return out;
    }

   private:
    std::string jar_, java_;
    int threads_;
};

// fumen piece id of a piece (as used in sfinder pictures).
inline uint8_t fumenId(Piece p) {
    constexpr uint8_t ids[7] = {1, 3, 5, 2, 6, 7, 4};  // I O T L J S Z
    return p == Piece::None ? 0 : ids[static_cast<int>(p)];
}

// A way to play a solution from a game state: the moves and the game after.
struct Realisation {
    std::vector<Move> moves;
    Game after;
};

// Turns an sfinder picture into moves from `game` (an empty board). Searches
// placement orders and hold presses; each placement must be legal in our
// rules and land exactly on unused cells of its own colour, with picture rows
// removed as they are cleared. Returns one realisation per distinct end
// state (the held piece can differ, e.g. if hold is never used), or none if
// the picture cannot be played under our rules.
inline std::vector<Realisation> realise(const Game& game, const std::vector<std::array<uint8_t, Board::WIDTH>>& picture) {
    const int height = static_cast<int>(picture.size());
    uint64_t all = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < Board::WIDTH; ++x)
            if (picture[y][x]) all |= uint64_t(1) << (y * Board::WIDTH + x);

    std::vector<Realisation> out;
    std::set<int> endHolds;
    std::set<std::tuple<uint64_t, int, int, int>> dead;  // states already explored
    std::vector<Move> path;

    // rows[y] = the picture row that board row y corresponds to.
    std::function<void(const Game&, uint64_t, const std::vector<int>&)> go = [&](const Game& g, uint64_t claimed,
                                                                                 const std::vector<int>& rows) {
        if (claimed == all) {
            if (g.board().empty() && endHolds.insert(static_cast<int>(g.held())).second) out.push_back({path, g});
            return;
        }
        if (!dead.insert({claimed, g.taken(), static_cast<int>(g.held()), static_cast<int>(g.current())}).second) return;
        for (bool hold : {false, true}) {
            if (hold && (!g.canHold() || g.held() == g.current())) continue;
            Piece piece = g.pieceAfter(hold);
            if (piece == Piece::None) continue;
            for (const Placement& p : legalPlacements(g.board(), piece, static_cast<int>(rows.size()))) {
                uint64_t cells = 0;
                bool fits = true;
                for (Cell c : p.cells()) {
                    int bit = rows[c.y] * Board::WIDTH + c.x;
                    if (picture[rows[c.y]][c.x] != fumenId(piece) || (claimed >> bit & 1)) {
                        fits = false;
                        break;
                    }
                    cells |= uint64_t(1) << bit;
                }
                if (!fits) continue;
                // Rows completed by this piece disappear from the board.
                std::vector<int> nextRows;
                Board check = g.board();
                for (Cell c : p.cells()) check.set(c.x, c.y);
                for (int y = 0; y < static_cast<int>(rows.size()); ++y)
                    if (check.row(y) != Board::FULL_ROW) nextRows.push_back(rows[y]);
                Game next = g;
                next.playUnchecked({hold, p});
                path.push_back({hold, p});
                go(next, claimed | cells, nextRows);
                path.pop_back();
            }
        }
    };
    std::vector<int> rows(height);
    for (int y = 0; y < height; ++y) rows[y] = y;
    go(game, 0, rows);
    return out;
}

}  // namespace tetris
