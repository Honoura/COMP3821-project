// dp_search.cpp — the offline DP from dp.cpp, with the exhaustive PC search of
// online.h (findPC) as the PC checker instead of perfect-tetris.
//
// It needs no external solver, so it runs anywhere, and it gives an
// independent check of dp.cpp: the search has no time or node limit, so every
// "no PC" answer is a proof and the result is always exact. It is slower than
// dp.cpp on a many-core machine (a few minutes per 200-piece sequence).
//
// With DP_DUMP=<file> set, it also writes how many further PCs are possible
// from every boundary state reached, in the same format as dp.cpp;
// tools/fit_value.py fits the online value table to these. DP_THREADS sets
// the number of threads (default: all cores).
//
// After every PC the board is empty, so the whole game state is
// (pieces placed, piece in hold); by Corollary 2.6 there are at most 321 such
// states for 200 pieces. From each reachable state, pcOptions()
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
// the DP runs afterwards.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "online.h"
#include "tetris.h"

using namespace tetris;

namespace {

constexpr int HEIGHTS[] = {2, 4};

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

// Every PC of exactly `height` lines from an empty board, one for each piece
// it can leave in hold (Piece::None = the hold slot stays empty).
online::PCAnswer pcOptions(const Game& game, int height) {
    online::PCAnswer out;
    if (game.over() || !game.board().empty()) return out;
    if (game.rules().maxHeight > 0 && height > game.rules().maxHeight) return out;
    int place = 5 * height / 2;  // pieces a PC of this height places (Lemma 2.1)
    bool held = game.held() != Piece::None;
    auto search = [&](int draws, const std::vector<Piece>& ends) {  // draws = pieces taken after the current one
        if (draws > game.remaining()) return;
        online::PCAnswer a = online::findPC(game.board(), game.held(), game.current(), game.preview(draws), height,
                                            ends, static_cast<int>(ends.size()), 0, game.rules().hold);
        out.nodes += a.nodes;
        for (online::PCEnd& e : a.ends) out.ends.push_back(std::move(e));
    };
    // The hold slot is never used: exactly `place` pieces, the current one included.
    if (!held) search(place - 1, {Piece::None});
    // A piece is left in hold: one more piece than is placed.
    if (game.rules().hold)
        search(held ? place - 1 : place, std::vector<Piece>(std::begin(ALL_PIECES), std::end(ALL_PIECES)));
    return out;
}

// Finds every reachable boundary state and the PC edges out of it, searching
// states on all cores as soon as they are discovered.
std::vector<Edge> discover(const Game& start, long long& nodes) {
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
            long long n = 0;
            for (int height : HEIGHTS) {
                online::PCAnswer opts = pcOptions(game, height);
                n += opts.nodes;
                for (online::PCEnd& opt : opts.ends) {
                    Game next = game;
                    for (const Move& m : opt.moves) next.playUnchecked(m);
                    found.push_back({state, {next.placed(), holdIndex(next.held())}, height, std::move(opt.moves)});
                }
            }

            lock.lock();
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
    if (const char* s = std::getenv("DP_THREADS"); s && *s) threads = static_cast<unsigned>(std::max(1, std::atoi(s)));
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    return edges;
}

}  // namespace

std::vector<Move> solve(const Game& start) {
    auto t0 = std::chrono::steady_clock::now();
    long long nodes = 0;
    std::vector<Edge> edges = discover(start, nodes);
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
              << " search nodes, " << static_cast<long long>(ms) << " ms, every search exhaustive\n";

    // Optional: write the number of further PCs possible from every boundary
    // state reached (see the top of the file).
    if (const char* dump = std::getenv("DP_DUMP"); dump && *dump) {
        std::map<State, int> further;  // edges are sorted by source and only go forward
        for (auto it = edges.rbegin(); it != edges.rend(); ++it)
            further[it->from] = std::max(further[it->from], 1 + further[it->to]);
        std::ofstream out(dump);
        out << "{\"queue\":\"" << queueToString(start.queue()) << "\",\"optimum\":" << best[end]
            << ",\"timedOut\":0,\"values\":[";
        bool first = true;
        for (auto& [s, pcs] : best) {
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
