// dp_builtin.cpp — the offline DP from dp.cpp, using tetris.h's own PC search
// (perfectClearOptions) instead of perfect-tetris. Kept as a cross-check.
//
// After every PC the board is empty, so the whole game state is
// (pieces placed, piece in hold); by Corollary 2.6 there are at most 321 such
// states for 200 pieces. From each reachable state, perfectClearOptions()
// lists every 2- and 4-line PC by the hold piece it ends with; each is an
// edge to a later state. Edges always move forward in the sequence, so
//
//     best(s') = max over edges s -> s' of best(s) + 1
//
// can be filled in order of pieces placed, and the state with the largest
// best() gives the maximum number of PCs. Pieces after the final PC are not
// placed.
//
// A state's edges do not depend on its best() value, only on whether it is
// reachable, so the PC searches run in parallel as states are discovered and
// the DP runs afterwards. The result is exact provided every search
// completes; a search that hits the node limit is reported on stderr.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "tetris.h"

using namespace tetris;

namespace {

constexpr int HEIGHTS[] = {2, 4};
constexpr long long NODE_LIMIT = 20'000'000;

// Hold slot index: 0 = empty, k + 1 = piece k.
int holdIndex(Piece p) { return p == Piece::None ? 0 : static_cast<int>(p) + 1; }

struct State {
    int placed, hold;
    bool operator<(const State& o) const { return placed != o.placed ? placed < o.placed : hold < o.hold; }
};

struct Edge {
    State from, to;
    int height;
    std::vector<Move> moves;
};

// Finds every reachable boundary state and the PC edges out of it, searching
// states on all cores as soon as they are discovered.
std::vector<Edge> discover(const Game& start, int& incomplete, long long& nodes) {
    std::mutex mu;
    std::condition_variable cv;
    std::map<State, bool> seen;
    std::vector<std::pair<State, Game>> work{{{0, holdIndex(start.held())}, start}};
    std::vector<Edge> edges;
    int active = 0;
    seen[work[0].first] = true;

    auto worker = [&] {
        std::unique_lock<std::mutex> lock(mu);
        for (;;) {
            cv.wait(lock, [&] { return !work.empty() || active == 0; });
            if (work.empty()) return;
            auto [state, game] = std::move(work.back());
            work.pop_back();
            ++active;
            lock.unlock();

            std::vector<Edge> found;
            int cut = 0;
            long long n = 0;
            for (int height : HEIGHTS) {
                PCOptions opts = perfectClearOptions(game, height, NODE_LIMIT);
                n += opts.nodes;
                if (!opts.complete) ++cut;
                for (PCOption& opt : opts.options) {
                    Game next = game;
                    for (const Move& m : opt.moves) next.playUnchecked(m);
                    found.push_back({state, {next.placed(), holdIndex(next.held())}, height, std::move(opt.moves)});
                }
            }

            lock.lock();
            incomplete += cut;
            nodes += n;
            for (Edge& e : found) {
                if (!seen[e.to]) {
                    seen[e.to] = true;
                    Game next = game;
                    for (const Move& m : e.moves) next.playUnchecked(m);
                    work.push_back({e.to, std::move(next)});
                }
                edges.push_back(std::move(e));
            }
            --active;
            cv.notify_all();
        }
    };

    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    return edges;
}

}  // namespace

std::vector<Move> solve(const Game& start) {
    auto t0 = std::chrono::steady_clock::now();
    int incomplete = 0;
    long long nodes = 0;
    std::vector<Edge> edges = discover(start, incomplete, nodes);
    // Fixed order, so ties break the same way on every run.
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.from.placed != b.from.placed || a.from.hold != b.from.hold) return a.from < b.from;
        if (a.height != b.height) return a.height < b.height;
        return a.to < b.to;
    });

    // best[s] = most PCs ending in s; via[s] = the edge that achieved it.
    State origin{0, holdIndex(start.held())};
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
              << "dp: " << best.size() << " boundary states, " << edges.size() << " PC options, " << nodes
              << " search nodes, " << std::thread::hardware_concurrency() << " threads, "
              << static_cast<long long>(ms) << " ms\n";
    if (incomplete) std::cerr << "dp: " << incomplete << " searches hit the node limit; result may not be optimal\n";
    return moves;
}
