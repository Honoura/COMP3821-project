// sfinder_dp.cpp — offline DP over PC boundaries, with solution-finder
// finding the PC options.
//
// After every PC the board is empty, so the game state is (pieces placed,
// piece in hold). From each reachable state we find every PC option, i.e.
// every (height, piece left in hold) a 2- or 4-line PC can end with; each is
// an edge to a later state. Edges only go forward, so
//
//     best(s') = max over edges s -> s' of best(s) + 1
//
// is filled in order of pieces placed and the state with the largest best()
// gives the most PCs. Pieces after the final PC are not placed.
//
// Finding the options (queries for one queue position are batched):
//   2 lines  `sfinder path` lists every solution; realise() turns each into
//            legal moves and reports the hold piece(s) it can end with.
//   4 lines  `sfinder path` is far too slow here (one query from an empty
//            board ran for 37+ minutes), so:
//            1. `sfinder percent` says which states have any 4-line PC.
//            2. For each piece that could be left in hold, perfect-tetris is
//               asked for a PC saving it, with a short time limit (an empty
//               end hold means hold was never used; our own no-hold search
//               handles that). Solutions come back in milliseconds.
//            3. perfect-tetris is slow only at proving a PC impossible, so
//               options it cannot settle quickly go back to sfinder: a PC
//               ending with X never places X, so if `percent` finds no PC on
//               the window minus any one occurrence of X (with hold, which
//               allows every order we could play), X is impossible. Anything
//               still open is settled by tetris.h's exhaustive search.
// Every move is replayed through our own rules before it is used.
//
// Needs Java, tools/sfinder/sfinder.jar (sfinder.h) and the perfect-tetris
// binary (perfect_tetris.h).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <thread>

#include "perfect_tetris.h"
#include "sfinder.h"
#include "tetris.h"

using namespace tetris;

namespace {

constexpr double SFINDER_TIMEOUT_SEC = 300;
constexpr double QUICK_TIMEOUT_SEC = 3;

// Hold slot index: 0 = empty, k + 1 = piece k.
int holdIndex(Piece p) { return p == Piece::None ? 0 : static_cast<int>(p) + 1; }

struct State {
    int placed, hold;
    bool operator<(const State& o) const { return placed != o.placed ? placed < o.placed : hold < o.hold; }
    bool operator==(const State& o) const { return placed == o.placed && hold == o.hold; }
};

struct Edge {
    State from, to;
    int height;
    std::vector<Move> moves;
};

// The pieces an H-line PC from `game` can draw from (hold, current, next):
// 5H/2 placements plus one left over, or fewer at the end of the queue.
// Returns an empty vector if there are not enough pieces for the PC.
std::vector<Piece> window(const Game& game, int height) {
    int placements = 5 * height / 2;
    std::vector<Piece> w;
    if (game.held() != Piece::None) w.push_back(game.held());
    w.push_back(game.current());
    for (int k = 0; static_cast<int>(w.size()) < placements + 1 && game.peek(k) != Piece::None; ++k) w.push_back(game.peek(k));
    if (static_cast<int>(w.size()) < placements) w.clear();
    return w;
}

std::string letters(const std::vector<Piece>& pieces) { return queueToString(pieces); }

// Batches `percent` queries: add patterns, then run() answers all of them
// with one sfinder call per (length, hold) group.
class PercentBatch {
   public:
    void add(const std::string& pattern, bool hold) { groups_[{pattern.size(), hold}].insert(pattern); }
    void run(const SFinder& sf, int height) {
        for (auto& [key, patterns] : groups_) {
            std::vector<std::string> list(patterns.begin(), patterns.end());
            auto ok = sf.possible(list, height, key.second, SFINDER_TIMEOUT_SEC);
            if (!ok) throw std::runtime_error("sfinder percent timed out");
            for (size_t i = 0; i < list.size(); ++i) answers_[{list[i], key.second}] = (*ok)[i];
            ++calls;
        }
    }
    bool possible(const std::string& pattern, bool hold) const { return answers_.at({pattern, hold}); }
    int calls = 0;

   private:
    std::map<std::pair<size_t, bool>, std::set<std::string>> groups_;
    std::map<std::pair<std::string, bool>, bool> answers_;
};

}  // namespace

std::vector<Move> solve(const Game& start) {
    auto t0 = std::chrono::steady_clock::now();
    auto jar = SFinder::findJar();
    auto pcBinary = PerfectTetris::findBinary();
    if (!jar || !pcBinary) {
        if (!jar) std::cerr << "sfinder_dp: sfinder.jar not found. Run tools/fetch-sfinder.sh or set SFINDER_JAR.\n";
        if (!pcBinary)
            std::cerr << "sfinder_dp: perfect-tetris binary not found. Build it with Zig 0.14 (see README) "
                         "or set PERFECT_TETRIS_PC.\n";
        return {};
    }
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    SFinder sfinder(*jar, "java", static_cast<int>(cores));
    PerfectTetris perfectTetris(*pcBinary);

    std::map<int, std::map<int, Game>> pending;  // queue position -> hold -> game
    pending[0].emplace(holdIndex(start.held()), start);
    std::map<std::pair<State, State>, Edge> found;  // first realisation of each (from, to)
    int states = 0, sfinderCalls = 0, twoLineSolutions = 0, candidates = 0, ruledOut = 0, confirmed = 0,
        exactSearches = 0, unresolved = 0;

    auto addEdge = [&](State from, int height, std::vector<Move> moves, Game after) {
        State to{after.placed(), holdIndex(after.held())};
        if (found.count({from, to})) return;
        found.emplace(std::make_pair(from, to), Edge{from, to, height, std::move(moves)});
        pending[to.placed].emplace(to.hold, std::move(after));
    };

    try {
        while (!pending.empty()) {
            const int placed = pending.begin()->first;
            std::vector<std::pair<int, Game>> level(pending.begin()->second.begin(), pending.begin()->second.end());
            pending.erase(pending.begin());
            states += static_cast<int>(level.size());
            auto lap = std::chrono::steady_clock::now();
            auto seconds = [&] {
                auto now = std::chrono::steady_clock::now();
                double s = std::chrono::duration<double>(now - lap).count();
                lap = now;
                return s;
            };

            // ---- 2 lines: every solution from sfinder path (one call per pattern length).
            std::map<size_t, std::vector<int>> byLength;
            for (size_t i = 0; i < level.size(); ++i)
                if (auto w = window(level[i].second, 2); !w.empty()) byLength[w.size()].push_back(static_cast<int>(i));
            for (auto& [len, members] : byLength) {
                std::vector<SFinder::Query> queries;
                for (int i : members) {
                    const Game& g = level[i].second;
                    auto w = window(g, 2);
                    queries.push_back({Piece::None, w});  // window already starts with the held piece
                }
                auto answers = sfinder.solve(queries, 2, SFINDER_TIMEOUT_SEC);
                ++sfinderCalls;
                for (size_t k = 0; k < members.size(); ++k) {
                    if (!answers[k].complete) throw std::runtime_error("sfinder path timed out");
                    auto& [hold, game] = level[members[k]];
                    for (const SFinder::Solution& s : answers[k].solutions) {
                        ++twoLineSolutions;
                        for (Realisation& r : realise(game, s.picture)) addEdge({placed, hold}, 2, std::move(r.moves), std::move(r.after));
                    }
                }
            }

            double pathSec = seconds();

            // ---- 4 lines, step 1: which states have any 4-line PC (sfinder percent).
            std::vector<std::vector<Piece>> windows(level.size());
            PercentBatch anyPC;
            for (size_t i = 0; i < level.size(); ++i) {
                windows[i] = window(level[i].second, 4);
                if (!windows[i].empty()) anyPC.add(letters(windows[i]), true);
            }
            anyPC.run(sfinder, 4);
            sfinderCalls += anyPC.calls;

            // ---- step 2: one task per (state, end hold); perfect-tetris (or our own
            // no-hold search for an empty end hold) with a short time limit.
            struct Task {
                int state;
                Piece endHold;
                std::optional<std::vector<Move>> moves;
                bool unresolved = false;  // perfect-tetris ran out of time
                std::string error;
            };
            std::vector<Task> tasks;
            for (size_t i = 0; i < level.size(); ++i) {
                const std::vector<Piece>& w = windows[i];
                if (w.empty()) continue;
                std::set<Piece> ends(w.begin(), w.end());
                if (level[i].second.held() == Piece::None) ends.insert(Piece::None);
                if (static_cast<int>(w.size()) < 11) ends = {Piece::None};  // end of the queue: nothing left over
                for (Piece x : ends) {
                    ++candidates;
                    if (anyPC.possible(letters(w), true)) tasks.push_back({static_cast<int>(i), x, std::nullopt, false, {}});
                    else ++ruledOut;
                }
            }
            auto runTasks = [&](std::vector<Task*> todo, double timeoutSec) {
                std::atomic<size_t> next{0};
                std::vector<std::thread> pool;
                for (unsigned t = 0; t < cores; ++t)
                    pool.emplace_back([&] {
                        for (size_t k; (k = next++) < todo.size();) {
                            Task& task = *todo[k];
                            const Game& g = level[task.state].second;
                            try {
                                if (task.endHold == Piece::None) {
                                    if (g.held() != Piece::None) continue;
                                    detail::PCSearch search(g.queue(), false, 50'000'000);
                                    if (search.dfs(g.board(), g.current(), g.held(), g.taken(), 4)) task.moves = search.path;
                                    continue;
                                }
                                PerfectTetris::Query q{g.held(), {g.current()}};
                                int hasHold = g.held() != Piece::None;
                                for (int i = 0; static_cast<int>(q.queue.size()) + hasHold < 12 && g.peek(i) != Piece::None; ++i)
                                    q.queue.push_back(g.peek(i));
                                auto a = perfectTetris.solve({q}, 4, task.endHold, timeoutSec)[0];
                                task.unresolved = a.status == PerfectTetris::Status::TimedOut;
                                if (a.status == PerfectTetris::Status::Solved)
                                    task.moves = replayPlacements(g, a.placements, task.endHold);
                            } catch (const std::exception& e) {
                                task.error = e.what();
                            }
                        }
                    });
                for (std::thread& t : pool) t.join();
            };
            std::vector<Task*> all;
            for (Task& t : tasks) all.push_back(&t);
            runTasks(all, QUICK_TIMEOUT_SEC);
            for (Task& t : tasks)
                if (!t.error.empty()) throw std::runtime_error(t.error);

            // ---- step 3, only for options perfect-tetris could not settle quickly.
            // sfinder percent can rule X out: a PC ending with X never places X,
            // so if no PC exists on the window minus any one occurrence of X
            // (with hold, a superset of the orders we could play), X is impossible.
            std::vector<Task*> hard;
            for (Task& t : tasks)
                if (t.unresolved) hard.push_back(&t);
            if (!hard.empty()) {
                PercentBatch removal;
                for (Task* t : hard) {
                    const std::vector<Piece>& w = windows[t->state];
                    for (size_t j = 0; j < w.size(); ++j)
                        if (w[j] == t->endHold) {
                            std::vector<Piece> rest = w;
                            rest.erase(rest.begin() + static_cast<long>(j));
                            removal.add(letters(rest), true);
                        }
                }
                removal.run(sfinder, 4);
                sfinderCalls += removal.calls;
                std::map<int, std::vector<Task*>> open;  // state -> options still open
                for (Task* t : hard) {
                    const std::vector<Piece>& w = windows[t->state];
                    bool maybe = false;
                    for (size_t j = 0; j < w.size(); ++j)
                        if (w[j] == t->endHold) {
                            std::vector<Piece> rest = w;
                            rest.erase(rest.begin() + static_cast<long>(j));
                            maybe |= removal.possible(letters(rest), true);
                        }
                    if (maybe) {
                        open[t->state].push_back(t);
                    } else {
                        t->unresolved = false;
                        ++ruledOut;
                    }
                }
                // Still open: settle them exactly with tetris.h's exhaustive search,
                // which answers every end-hold piece of a state in one run.
                std::vector<std::pair<int, PCOptions>> exact;
                for (auto& [state, list] : open) exact.push_back({state, {}});
                std::vector<std::thread> searches;
                for (auto& [state, result] : exact)
                    searches.emplace_back([&, state = state, out = &result] {
                        *out = perfectClearOptions(level[state].second, 4, 200'000'000);
                    });
                for (std::thread& t : searches) t.join();
                for (auto& [state, result] : exact) {
                    ++exactSearches;
                    for (Task* t : open[state]) {
                        t->unresolved = !result.complete;
                        for (PCOption& o : result.options)
                            if (o.endHold == t->endHold) t->moves = o.moves;
                        if (t->unresolved) {
                            ++unresolved;
                            std::cerr << "sfinder_dp: could not settle piece " << placed << " save " << toChar(t->endHold) << "\n";
                        }
                    }
                }
            }

            for (Task& task : tasks) {
                auto& [hold, game] = level[task.state];
                Game after = game;
                if (task.moves)
                    for (const Move& m : *task.moves) after.playUnchecked(m);
                if (!task.moves || !after.board().empty()) continue;
                ++confirmed;
                addEdge({placed, hold}, 4, std::move(*task.moves), std::move(after));
            }
            std::cerr << "sfinder_dp: piece " << placed << ": " << level.size() << " states, " << tasks.size()
                      << " 4-line options checked, " << hard.size() << " hard, " << pathSec + seconds() << " s\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "sfinder_dp: " << e.what() << "\n";
        return {};
    }

    // DP over the edges (the map keeps them sorted by source; edges only go forward).
    State origin{0, holdIndex(start.held())};
    std::map<State, int> best{{origin, 0}};
    std::map<State, const Edge*> via;
    for (auto& [key, e] : found) {
        auto from = best.find(e.from);
        if (from == best.end()) continue;
        auto [to, inserted] = best.emplace(e.to, from->second + 1);
        if (inserted || from->second + 1 > to->second) to->second = from->second + 1, via[e.to] = &e;
    }

    // Most PCs, then fewest pieces used.
    State end = origin;
    for (auto& [s, pcs] : best)
        if (pcs > best[end]) end = s;
    std::vector<const Edge*> path;
    for (State s = end; via.count(s); s = via[s]->from) path.push_back(via[s]);
    std::vector<Move> moves;
    for (auto it = path.rbegin(); it != path.rend(); ++it) moves.insert(moves.end(), (*it)->moves.begin(), (*it)->moves.end());

    int two = 0, four = 0;
    for (const Edge* e : path) (e->height == 2 ? two : four)++;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cerr << "sfinder_dp: " << best[end] << " perfect clears (" << two << " two-line, " << four << " four-line)\n"
              << "sfinder_dp: " << states << " boundary states, " << found.size() << " distinct transitions, "
              << sfinderCalls << " sfinder calls, " << static_cast<long long>(ms) << " ms\n"
              << "sfinder_dp: 2-line: " << twoLineSolutions << " sfinder solutions\n"
              << "sfinder_dp: 4-line: " << candidates << " (state, end hold) options, " << ruledOut
              << " ruled out by sfinder, " << confirmed << " found, " << exactSearches
              << " states needed the exhaustive search\n";
    if (unresolved) std::cerr << "sfinder_dp: " << unresolved << " options could not be settled; the result may not be optimal\n";
    return moves;
}
