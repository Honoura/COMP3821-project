// dp.cpp — exact offline dynamic program over PC boundaries, using
// perfect-tetris as the PC checker.
//
// After every PC the board is empty, so the whole game state is
// (pieces placed, piece in hold); by Corollary 2.6 there are at most 321 such
// states for 200 pieces. From each reachable state we ask perfect-tetris for
// every combination of
//
//     PC height  in {2, 4}
//     end hold   in {any, I, O, T, L, J, S, Z}
//
// and, from a state whose hold slot is empty, we also search for a PC that
// leaves it empty (see below). Each query gets only enough pieces for a PC of
// that height, so the answer is an H-line PC or nothing. Each answer is an
// edge to a later state (pieces placed + 5H/2, end hold). Edges only go
// forward, so
//
//     best(s') = max over edges s -> s' of best(s) + 1
//
// is filled in order of pieces placed, and the state with the largest best()
// gives the maximum number of PCs. Pieces after the final PC are not placed.
//
// perfect-tetris cannot be asked for a PC that leaves the hold slot *empty*:
// -s names a piece to leave in hold, and without -s the solver returns
// whichever solution it finds first, which is almost never the hold-free one.
// That case has to be covered separately, because a PC that never touches
// hold draws exactly 5H/2 pieces, one fewer than any other, so a hold-free
// line stays a piece ahead of every other line for the rest of the sequence.
// In the middle of the sequence this costs nothing -- the state it reaches,
// (placed, empty), has the same pieces ahead of it as (placed, next piece),
// so the two have the same future -- but at the end of the queue that spare
// piece decides whether one more PC fits. (Seed 38 is such a sequence: its
// optimum is twenty 4-line PCs that place all 200 pieces and never hold.)
// These edges come from the exhaustive hold-free search of online.h, which
// has no node budget, so a "no" from it is a proof. When the rules disable
// hold altogether it is the only search used.
//
// Before each query, Lemma 2.3 is applied as a pruning rule: the pieces a PC
// places are the hold, current and next pieces minus the one left over, and
// if none of them is a T the number of L and J pieces must be even.
// perfect-tetris finds solutions in milliseconds but can take many seconds to
// prove there are none, so this removes most of the slow queries.
//
// Every (state, height, end hold) query runs as its own perfect-tetris process
// with a time limit, on all cores; a state's queries and hold-free searches
// start as soon as the state is discovered. Every answer is replayed through
// Game::play(), so a solution that is illegal under our rules is rejected
// (perfect-tetris may place the hold piece last, which needs a piece we do
// not have when the queue runs out).
// The result is exact if no query times out (reported on stderr).
// Needs the `pc` binary (see perfect_tetris.h).
//
// With DP_DUMP=<file> set, it also writes how many further PCs are possible
// from every boundary state reached; tools/fit_value.py fits the online value
// table to these.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "online.h"
#include "perfect_tetris.h"
#include "tetris.h"

using namespace tetris;

namespace {

constexpr int HEIGHTS[] = {2, 4};
constexpr double QUERY_TIMEOUT_SEC = 30;

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

// One PC question about a state: a height and what the PC leaves in hold.
// saveIndex >= 0 is a perfect-tetris query for SAVES[saveIndex] (Piece::None =
// any piece); saveIndex == NO_HOLD is the hold-free search of online.h.
struct Task {
    State state;
    int height;
    int saveIndex;  // position in SAVES, used to break ties deterministically
    Game game;
    PerfectTetris::Query query;
};

constexpr int NO_HOLD = -1;
constexpr Piece SAVES[] = {Piece::None, Piece::I, Piece::O, Piece::T, Piece::L, Piece::J, Piece::S, Piece::Z};

// Lemma 2.3: a PC from an empty board uses an even number of L, J and
// vertical T pieces. A T can always be turned flat or vertical, so this only
// rules out piece sets with no T and an odd number of L and J. The PC places
// every piece of `pieces` except the one at `skip` (pieces.size() = all of them).
bool parityOk(const std::vector<Piece>& pieces, size_t skip) {
    int t = 0, lj = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (i == skip) continue;
        t += pieces[i] == Piece::T;
        lj += pieces[i] == Piece::L || pieces[i] == Piece::J;
    }
    return t > 0 || lj % 2 == 0;
}

// `window` is the hold, current and next pieces the PC can draw from; it
// places all of them except `leftOver` (Piece::None = any one of them).
bool parityAllows(const std::vector<Piece>& window, Piece leftOver) {
    for (size_t i = 0; i < window.size(); ++i)
        if ((leftOver == Piece::None || window[i] == leftOver) && parityOk(window, i)) return true;
    return false;
}

// The pieces for an H-line PC from `game`: hold, current and next pieces,
// 5H/2 placements plus two. A PC of 5H/2 placements can draw at most one
// piece beyond them, but perfect-tetris reports SolutionTooLong when asked to
// save a hold piece with exactly that many, so it gets one spare. Either way
// the queue is too short for a taller PC, so the answer is H lines or none.
std::optional<PerfectTetris::Query> queryFor(const Game& game, int height) {
    int placements = 5 * height / 2;
    int hasHold = game.held() != Piece::None;
    if (hasHold + 1 + game.remaining() < placements) return std::nullopt;
    PerfectTetris::Query q{game.held(), {game.current()}};
    for (int k = 0; static_cast<int>(q.queue.size()) + hasHold < placements + 2 && game.peek(k) != Piece::None; ++k)
        q.queue.push_back(game.peek(k));
    return q;
}

// The pieces an H-line PC places when it never touches the hold slot: the
// current piece and the next 5H/2 - 1, in queue order. Only possible from a
// state whose hold slot is already empty, since hold never empties again.
std::optional<std::vector<Piece>> holdFreePieces(const Game& game, int height) {
    int placements = 5 * height / 2;
    if (game.held() != Piece::None || 1 + game.remaining() < placements) return std::nullopt;
    std::vector<Piece> pieces{game.current()};
    for (Piece p : game.preview(placements - 1)) pieces.push_back(p);
    return pieces;
}

}  // namespace

std::vector<Move> solve(const Game& start) {
    auto t0 = std::chrono::steady_clock::now();
    auto binary = PerfectTetris::findBinary();
    if (!binary) {
        std::cerr << "dp: perfect-tetris binary not found. Build it with Zig 0.14\n"
                     "    (cd perfect-tetris && zig build -Doptimize=ReleaseFast) or set PERFECT_TETRIS_PC.\n";
        return {};
    }
    PerfectTetris solver(*binary);

    std::mutex mu;
    std::condition_variable cv;
    std::deque<Task> work;
    std::map<State, bool> seen;
    std::map<std::pair<State, State>, std::pair<int, Edge>> found;  // (from, to) -> (priority, edge)
    int states = 0, queries = 0, searches = 0, solved = 0, pruned = 0, timedOut = 0, rejected = 0, active = 0;
    std::string error;

    // Queues every query for a newly reached state that passes the parity check.
    auto addState = [&](State s, const Game& game) {
        ++states;
        for (int height : HEIGHTS) {
            if (auto pieces = holdFreePieces(game, height)) {  // the PC places all of them, none left over
                if (parityOk(*pieces, pieces->size())) work.push_back({s, height, NO_HOLD, game, {}});
                else ++pruned;
            }
            if (!game.rules().hold) continue;  // then every PC is hold-free; nothing to ask
            auto q = queryFor(game, height);
            if (!q) continue;
            std::vector<Piece> window;  // the pieces the PC can draw from
            if (q->hold != Piece::None) window.push_back(q->hold);
            window.insert(window.end(), q->queue.begin(), q->queue.end() - 1);  // minus the spare
            for (int i = 0; i < 8; ++i) {
                if (parityAllows(window, SAVES[i])) work.push_back({s, height, i, game, *q});
                else ++pruned;
            }
        }
    };
    State origin{0, holdIndex(start.held())};
    seen[origin] = true;
    addState(origin, start);

    auto worker = [&] {
        std::unique_lock<std::mutex> lock(mu);
        for (;;) {
            cv.wait(lock, [&] { return !work.empty() || active == 0 || !error.empty(); });
            if (work.empty() || !error.empty()) return;
            Task task = std::move(work.front());
            work.pop_front();
            ++active;
            lock.unlock();

            Piece save = task.saveIndex == NO_HOLD ? Piece::None : SAVES[task.saveIndex];
            PerfectTetris::Answer answer;
            std::string failure;
            std::optional<std::vector<Move>> moves;
            Game next = task.game;
            if (task.saveIndex == NO_HOLD) {
                int placements = 5 * task.height / 2;
                online::PCAnswer a =
                    online::findPC(task.game.board(), Piece::None, task.game.current(),
                                   task.game.preview(placements - 1), task.height, {Piece::None}, 1, 0,
                                   task.game.rules().hold);
                if (!a.ends.empty()) {
                    answer.status = PerfectTetris::Status::Solved;
                    moves = std::move(a.ends[0].moves);
                }
            } else {
                try {
                    answer = solver.solve({task.query}, task.height, save, QUERY_TIMEOUT_SEC)[0];
                } catch (const std::exception& e) {
                    failure = e.what();
                }
                if (answer.status == PerfectTetris::Status::Solved) {
                    std::optional<Piece> want = save == Piece::None ? std::nullopt : std::optional<Piece>(save);
                    moves = replayPlacements(task.game, answer.placements, want);
                }
            }
            if (moves)  // every answer is checked against our own rules
                for (const Move& m : *moves)
                    if (!next.play(m)) {
                        moves.reset();
                        break;
                    }

            lock.lock();
            --active;
            ++(task.saveIndex == NO_HOLD ? searches : queries);
            if (!failure.empty()) error = failure;
            if (answer.status == PerfectTetris::Status::TimedOut) {
                ++timedOut;
                std::cerr << "dp: query timed out at piece " << task.state.placed << ": "
                          << fumen::quizComment(task.query.hold, task.query.queue) << " height " << task.height
                          << " save " << (save == Piece::None ? '-' : toChar(save)) << "\n";
            }
            if (answer.status == PerfectTetris::Status::Solved) {
                ++solved;
                if (!moves || !next.board().empty()) {
                    ++rejected;
                } else {
                    State to{next.placed(), holdIndex(next.held())};
                    int priority = task.height * 9 + task.saveIndex + 1;  // the hold-free edge first
                    auto key = std::make_pair(task.state, to);
                    auto it = found.find(key);
                    if (it == found.end() || priority < it->second.first)
                        found[key] = {priority, Edge{task.state, to, task.height, std::move(*moves)}};
                    if (!seen[to]) seen[to] = true, addState(to, next);
                }
            }
            cv.notify_all();
        }
    };
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    if (!error.empty()) {
        std::cerr << "dp: " << error << "\n";
        return {};
    }
    std::vector<Edge> edges;
    for (auto& [key, entry] : found) edges.push_back(std::move(entry.second));

    // best[s] = most PCs ending in s; via[s] = the edge that achieved it.
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (!(a.from == b.from)) return a.from < b.from;
        if (a.height != b.height) return a.height < b.height;
        return a.to < b.to;
    });
    std::map<State, int> best{{origin, 0}};
    std::map<State, const Edge*> via;
    for (const Edge& e : edges) {  // sorted by source, and edges only go forward
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
    std::cerr << "dp: " << best[end] << " perfect clears (" << two << " two-line, " << four << " four-line)\n"
              << "dp: " << states << " boundary states, " << edges.size() << " distinct PC transitions\n"
              << "dp: " << queries << " perfect-tetris queries and " << searches << " hold-free searches ("
              << pruned << " more skipped by the parity lemma), " << solved << " solved, " << rejected
              << " rejected by replay, " << threads << " threads, " << static_cast<long long>(ms) << " ms\n";
    if (timedOut) std::cerr << "dp: " << timedOut << " queries timed out; the result may not be optimal\n";

    // Optional: write the number of further PCs possible from every boundary
    // state reached, as training data for the online value table
    // (tools/fit_value.py). Set DP_DUMP to the file to write.
    if (const char* dump = std::getenv("DP_DUMP"); dump && *dump) {
        // Edges are sorted by source and only go forward, so one backward pass
        // gives further[s] = max over edges s -> t of 1 + further[t].
        std::map<State, int> further;
        for (auto it = edges.rbegin(); it != edges.rend(); ++it)
            further[it->from] = std::max(further[it->from], 1 + further[it->to]);
        std::ofstream out(dump);
        out << "{\"queue\":\"" << queueToString(start.queue()) << "\",\"optimum\":" << best[end]
            << ",\"timedOut\":" << timedOut << ",\"values\":[";
        bool first = true;
        for (auto& [s, pcs] : best) {  // every state reachable from the start
            (void)pcs;
            out << (first ? "" : ",") << "[" << s.placed + (s.hold != 0) << ",\""
                << (s.hold ? std::string(1, toChar(ALL_PIECES[s.hold - 1])) : "") << "\"," << further[s] << "]";
            first = false;
        }
        out << "]}\n";
        std::cerr << "dp: wrote " << best.size() << " state values to " << dump << "\n";
    }
    return moves;
}
