// online.h — the online setting: playing with limited vision.
//
// In the online setting a strategy sees only the board, the held piece, the
// current piece and the next few pieces (5 by default), as in ordinary play.
// This header provides
//
//   * View and playOnline(): a limited-vision game loop. The policy is handed
//     a View, never the Game, so it cannot look further ahead than allowed.
//   * findPC(): a PC search for arbitrary positions with a node budget, which
//     can be told which piece to leave in hold.
//   * Player: the two online algorithms of the report,
//
//       Mode::Myopic  maximises the chance of completing the next PC;
//       Mode::Value   maximises 1 + V(i', h') for the state (i', h') the next
//                     PC ends in, where V is the value table fitted to the
//                     offline optimum (value_table.h). It plans beyond the
//                     next PC.
//
// A strategy file only needs
//
//     #include "online.h"
//     std::vector<tetris::Move> solve(const tetris::Game& start) {
//         tetris::online::Options opt;
//         opt.mode = tetris::online::Mode::Myopic;
//         return tetris::online::run(start, opt);
//     }
//
// How the algorithms decide
//   * They remember every piece they have seen, so they know which pieces of
//     the current 7-bag are still to come.
//   * A 2-line PC places 5 pieces, so it is always completely visible, and is
//     found exactly.
//   * A 4-line PC places 10 pieces, so up to 4 of them are hidden when it
//     starts. The algorithm samples completions of the hidden pieces from
//     the 7-bag, solves a few sampled futures with full knowledge and takes
//     the first moves of those solutions as candidates. Each candidate is
//     scored on every sampled future (can the PC still be completed after
//     this move, and in which end state?), and the best average is played.
//     One piece is revealed per placement, so after about 4 placements the
//     rest of the PC is visible and is solved exactly.
//   * If no 4-line PC is possible any more, the algorithm aims for a 6-line
//     PC in the same way, then for the nearest PC of any height that fits in
//     6 rows, and otherwise places a piece with a stacking heuristic.
//
// Solving sampled futures with full knowledge is hindsight optimisation: the
// scores are upper bounds and are used only to rank moves.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tetris.h"
#include "value_table.h"

namespace tetris {
namespace online {

constexpr int MAX_ROWS = 6;  // findPC handles stacks of at most 6 rows

// ------------------------------------------------------------------ view ---

// Everything an online strategy is allowed to see.
struct View {
    Board board;
    Piece current = Piece::None, held = Piece::None;
    std::vector<Piece> preview;  // the next pieces, nearest first (at most `vision`)
    int taken = 0;               // pieces drawn so far, including current and held
    int placed = 0;
    int length = 0;              // total number of pieces in the game
    int pcCount = 0, linesSincePC = 0;
    Rules rules;

    // Every legal move, worked out from visible information only.
    std::vector<Move> legalMoves() const {
        std::vector<Move> out;
        if (current == Piece::None) return out;
        for (const Placement& p : legalPlacements(board, current, rules.maxHeight)) out.push_back({false, p});
        Piece alt = !rules.hold ? Piece::None : held != Piece::None ? held : preview.empty() ? Piece::None : preview[0];
        if (alt != Piece::None && alt != current)
            for (const Placement& p : legalPlacements(board, alt, rules.maxHeight)) out.push_back({true, p});
        return out;
    }
};

inline View makeView(const Game& game, int vision) {
    View v;
    v.board = game.board();
    v.current = game.current();
    v.held = game.held();
    v.preview = game.preview(vision);
    v.taken = game.taken();
    v.placed = game.placed();
    v.length = static_cast<int>(game.queue().size());
    v.pcCount = game.pcCount();
    v.linesSincePC = game.linesSincePC();
    v.rules = game.rules();
    return v;
}

// Plays a whole game with limited vision. `policy(view)` returns the next
// move, or std::nullopt to stop. The policy only ever receives Views.
template <class Policy>
std::vector<Move> playOnline(const Game& start, int vision, Policy&& policy) {
    declaredVision() = vision;
    Game game = start;
    while (!game.over()) {
        std::optional<Move> move = policy(makeView(game, vision));
        if (!move) break;
        std::string why;
        if (!game.play(*move, &why)) {
            std::cerr << "online: illegal move at piece " << game.placed() << ": " << why << "\n";
            break;
        }
    }
    return game.history();
}

// ------------------------------------------------------------- PC search ---

namespace detail {

inline uint64_t pack(const Board& b) {
    uint64_t v = 0;
    for (int y = 0; y < MAX_ROWS; ++y) v |= uint64_t(b.row(y)) << (10 * y);
    return v;
}

// Placements with their move-ordering key, cached by (board, piece, rows).
// The same boards come up again and again, within one search (reached with
// different pieces in hand) and across the sampled futures of one decision.
struct Option {
    Placement p;
    uint64_t mask;  // the cells it fills, as bits 10 * y + x
    int order;
};

inline Board unpack(uint64_t bits) {
    Board b;
    for (int y = 0; y < MAX_ROWS; ++y)
        for (int x = 0; x < Board::WIDTH; ++x)
            if (bits >> (10 * y + x) & 1) b.set(x, y);
    return b;
}

// Removes full rows from a packed board; the rows above move down.
inline uint64_t clearRows(uint64_t bits, int& cleared) {
    uint64_t out = 0;
    int kept = 0;
    cleared = 0;
    for (int y = 0; y < MAX_ROWS; ++y) {
        uint64_t row = (bits >> (10 * y)) & Board::FULL_ROW;
        if (row == Board::FULL_ROW) ++cleared;
        else out |= row << (10 * kept++);
    }
    return out;
}

class MoveCache {
   public:
    // The options for `piece` on the packed board `bits` under a cap of `rows`
    // rows, sorted by move-ordering key. The pointer stays valid until the
    // next call.
    const Option* get(uint64_t bits, Piece piece, int rows, int& n) {
        uint32_t tag = uint32_t(static_cast<int>(piece) | rows << 3) + 1;  // 0 marks an empty slot
        if (slots_.empty() || pool_.size() > 3'000'000) reset();
        size_t mask = slots_.size() - 1;
        size_t i = hash(bits, tag) & mask;
        while (slots_[i].tag) {
            if (slots_[i].tag == tag && slots_[i].bits == bits) {
                n = slots_[i].n;
                return pool_.data() + slots_[i].start;
            }
            i = (i + 1) & mask;
        }
        if ((used_ + 1) * 2 > slots_.size()) {
            grow();
            return get(bits, piece, rows, n);
        }
        uint32_t start = uint32_t(pool_.size());
        for (const Placement& p : legalPlacements(unpack(bits), piece, rows)) {
            uint64_t mask = 0;
            for (Cell c : p.cells()) mask |= 1ull << (10 * c.y + c.x);
            pool_.push_back({p, mask, orderKey(bits, mask, p)});
        }
        std::stable_sort(pool_.begin() + start, pool_.end(), [](const Option& a, const Option& b) { return a.order < b.order; });
        slots_[i] = {bits, tag, start, uint16_t(pool_.size() - start)};
        ++used_;
        n = slots_[i].n;
        return pool_.data() + start;
    }

    // Move ordering (a heuristic only; every option is still tried): leftmost
    // first, which closes column boundaries early, then placements that leave
    // no empty cell directly beneath them, then low ones.
    static int orderKey(uint64_t b, uint64_t own, const Placement& p) {
        int top = 0, sum = 0, minX = 99, holes = 0;
        for (Cell c : p.cells()) {
            top = std::max(top, c.y), minX = std::min(minX, c.x), sum += c.y;
            if (c.y > 0 && !((b | own) >> (10 * (c.y - 1) + c.x) & 1)) ++holes;
        }
        return minX * 65536 + holes * 4096 + top * 64 + sum;
    }

   private:
    struct Slot {
        uint64_t bits;
        uint32_t tag, start;
        uint16_t n;
    };
    static size_t hash(uint64_t bits, uint32_t tag) {
        uint64_t h = bits * 0x9E3779B97F4A7C15ull ^ (uint64_t(tag) * 0xC2B2AE3D27D4EB4Full);
        return size_t(h ^ (h >> 31));
    }
    void reset() {
        slots_.assign(1 << 14, Slot{0, 0, 0, 0});
        pool_.clear();
        used_ = 0;
    }
    void grow() {
        std::vector<Slot> old(slots_.size() * 2, Slot{0, 0, 0, 0});
        old.swap(slots_);
        size_t mask = slots_.size() - 1;
        for (const Slot& s : old) {
            if (!s.tag) continue;
            size_t i = hash(s.bits, s.tag) & mask;
            while (slots_[i].tag) i = (i + 1) & mask;
            slots_[i] = s;
        }
    }
    std::vector<Slot> slots_;
    std::vector<Option> pool_;
    size_t used_ = 0;
};

inline MoveCache& moveCache() {
    static thread_local MoveCache cache;
    return cache;
}

// A set of (board, state) keys with open addressing; much faster here than
// std::unordered_set.
class KeySet {
   public:
    bool contains(uint64_t board, uint32_t meta) const {
        if (slots_.empty()) return false;
        size_t mask = slots_.size() - 1;
        for (size_t i = hash(board, meta) & mask; slots_[i].meta != EMPTY; i = (i + 1) & mask)
            if (slots_[i].board == board && slots_[i].meta == meta) return true;
        return false;
    }
    void insert(uint64_t board, uint32_t meta) {
        if ((used_ + 1) * 2 > slots_.size()) grow();
        size_t mask = slots_.size() - 1;
        size_t i = hash(board, meta) & mask;
        while (slots_[i].meta != EMPTY) {
            if (slots_[i].board == board && slots_[i].meta == meta) return;
            i = (i + 1) & mask;
        }
        slots_[i] = {board, meta};
        ++used_;
    }

   private:
    static constexpr uint32_t EMPTY = 0xFFFFFFFFu;
    struct Slot {
        uint64_t board;
        uint32_t meta;
    };
    static size_t hash(uint64_t board, uint32_t meta) {
        uint64_t h = board * 0x9E3779B97F4A7C15ull ^ (uint64_t(meta) * 0xC2B2AE3D27D4EB4Full);
        return size_t(h ^ (h >> 29));
    }
    void grow() {
        std::vector<Slot> old(slots_.empty() ? 256 : slots_.size() * 2, Slot{0, EMPTY});
        old.swap(slots_);
        used_ = 0;
        for (const Slot& s : old)
            if (s.meta != EMPTY) insert(s.board, s.meta);
    }
    std::vector<Slot> slots_;
    size_t used_ = 0;
};

// Depth-first search for a PC that leaves one given piece in hold.
//
// Fixing the end hold fixes which pieces are placed, which makes two extra
// pruning rules available: no piece type may be placed more often than it
// occurs among those pieces, and the column-colour argument of the parity
// lemma can be applied to the pieces still to come.
struct EndSearch {
    const std::vector<Piece>& queue;  // the pieces after the current one
    bool holdOk;
    Piece target;      // piece to leave in hold (Piece::None: hold stays empty)
    long long budget;  // give up after this many nodes (0 = no limit)
    long long nodes = 0;
    bool aborted = false;
    int need[8] = {}, used[8] = {};
    std::vector<Move> path;

    KeySet failed;

    EndSearch(const std::vector<Piece>& q, bool hold, Piece end, long long limit)
        : queue(q), holdOk(hold), target(end), budget(limit) {}

    Piece at(int i) const { return i < static_cast<int>(queue.size()) ? queue[i] : Piece::None; }

    // Call the boundary between columns x and x+1 closed if no row has both
    // (x, y) and (x+1, y) empty. A tetromino is connected, so one that covers
    // cells on both sides must cover such a pair; hence no piece can cross a
    // closed boundary. Line clears only remove rows and placements only fill
    // cells, so a closed boundary stays closed, and the empty cells to its
    // left must be filled by whole pieces: a multiple of 4. (A completely
    // filled column is the special case of two closed boundaries.)
    static bool wallsOk(uint64_t b, int rows) {
        unsigned crossable = 0;
        int colEmpty[Board::WIDTH] = {};
        for (int y = 0; y < rows; ++y) {
            unsigned e = ~unsigned(b >> (10 * y)) & Board::FULL_ROW;
            crossable |= e & (e >> 1);
            for (int x = 0; x < Board::WIDTH; ++x) colEmpty[x] += (e >> x) & 1;
        }
        int left = 0;
        for (int x = 0; x + 1 < Board::WIDTH; ++x) {
            left += colEmpty[x];
            if (!((crossable >> x) & 1) && (left & 3)) return false;
        }
        return true;
    }

    // Column colouring of the parity lemma: over a whole PC, cells placed in
    // even columns equal cells placed in odd columns, because every cleared
    // row has 5 of each. So the board's current imbalance must be cancelled
    // by the pieces still to come: each L or J contributes +-2, each T 0
    // (flat) or +-2 (vertical), each I 0 (flat) or +-4 (vertical), the others
    // 0. A vertical T spans 3 rows and a vertical I spans 4, and the stack is
    // never taller than the rows still to be cleared, so with fewer than 3
    // rows left every T is flat, and with fewer than 4 every I is flat.
    bool colourOk(uint64_t b, int rows) const {
        constexpr uint64_t EVEN = 0x0555555555555555ull;
        int off = __builtin_popcountll(b & EVEN) - __builtin_popcountll(b & ~EVEN);
        auto left = [&](Piece p) { return need[static_cast<int>(p)] - used[static_cast<int>(p)]; };
        int lj = left(Piece::L) + left(Piece::J);
        int t = rows >= 3 ? left(Piece::T) : 0;
        int i = rows >= 4 ? left(Piece::I) : 0;
        if (std::abs(off) > 2 * lj + 2 * t + 4 * i) return false;
        if (t == 0 && (off / 2 + lj) % 2 != 0) return false;
        return true;
    }

    // Cheap necessary conditions for a PC that must still clear `rows` lines.
    bool feasible(uint64_t bits, int rows, int available) const {
        int cells = Board::WIDTH * rows - __builtin_popcountll(bits);
        return !(rows < MAX_ROWS && (bits >> (10 * rows))) && cells > 0 && cells % 4 == 0 &&
               cells / 4 <= available && wallsOk(bits, rows) && colourOk(bits, rows);
    }

    // `bits` is the packed board and `rows` the number of lines still to
    // clear. True if a PC ending with `target` in hold was found; `path` then
    // holds its moves.
    bool dfs(uint64_t bits, Piece cur, Piece hold, int next, int rows) {
        if (cur == Piece::None || rows <= 0) return false;
        int a = static_cast<int>(cur), h = static_cast<int>(hold);
        if (holdOk && hold != Piece::None && h < a) std::swap(a, h);  // (cur, hold) ~ (hold, cur)
        uint32_t meta = uint32_t(a | h << 3 | next << 6 | rows << 16);
        if (failed.contains(bits, meta)) return false;
        if (aborted || (budget > 0 && nodes >= budget)) {
            aborted = true;
            return false;
        }
        ++nodes;

        // The pieces that can be placed now: the current one, or (after a
        // hold) the held one, or the next one if the hold slot is empty.
        struct Choice {
            bool hold;
            Piece piece, newHold;
            int next;
        };
        Choice choices[2];
        int nChoices = 0;
        auto allowed = [&](Piece p) { return p != Piece::None && used[static_cast<int>(p)] < need[static_cast<int>(p)]; };
        if (allowed(cur)) choices[nChoices++] = {false, cur, hold, next};
        if (holdOk) {
            if (hold == Piece::None) {
                if (allowed(at(next))) choices[nChoices++] = {true, at(next), cur, next + 1};
            } else if (hold != cur && allowed(hold)) {
                choices[nChoices++] = {true, hold, cur, next};
            }
        }
        // Merge the two sorted option lists so the best-ordered moves go first.
        // (Copied out of the cache, which deeper calls may reallocate.)
        struct Cand {
            Option option;
            int choice;
        };
        Cand cands[256];
        int nCands = 0;
        for (int c = 0; c < nChoices; ++c) {
            int n;
            const Option* options = moveCache().get(bits, choices[c].piece, rows, n);
            for (int k = 0; k < n && nCands < 256; ++k) cands[nCands++] = {options[k], c};
        }
        if (nChoices == 2)
            std::stable_sort(cands, cands + nCands, [](const Cand& x, const Cand& y) { return x.option.order < y.option.order; });

        for (int k = 0; k < nCands; ++k) {
            const Choice& c = choices[cands[k].choice];
            const Placement& p = cands[k].option.p;
            int cleared;
            uint64_t nb = clearRows(bits | cands[k].option.mask, cleared);
            int left = rows - cleared;
            if (nb == 0) {
                if (left == 0 && c.newHold == target) {
                    path.push_back({c.hold, p});
                    return true;
                }
                continue;  // a shorter PC, or the wrong piece left in hold
            }
            if (left <= 0) continue;
            int idx = static_cast<int>(p.piece);
            ++used[idx];
            int available = 1 + static_cast<int>(queue.size()) - (c.next + 1) + (c.newHold != Piece::None);
            bool ok = false;
            if (feasible(nb, left, available)) {
                path.push_back({c.hold, p});
                ok = dfs(nb, at(c.next), c.newHold, c.next + 1, left);
                if (!ok) path.pop_back();
            }
            --used[idx];
            if (ok) return true;
            if (aborted) return false;
        }
        failed.insert(bits, meta);
        return false;
    }
};

}  // namespace detail

struct PCEnd {
    Piece endHold;
    std::vector<Move> moves;
};

struct PCAnswer {
    std::vector<PCEnd> ends;  // in the order requested
    bool proved = true;       // false if a node budget ran out, so absence is not proved
    long long nodes = 0;
};

// Searches for PCs from an arbitrary position: `board` (at most 6 rows), the
// held and current pieces, and the pieces after the current one. `rows` is
// the number of lines the PC must still clear, so the stack may not be taller
// than `rows`. The end holds in `order` are tried best first (Piece::None
// means "hold stays empty", which needs an empty hold that is never used),
// and the search stops after `limit` have been found. `budget` caps the nodes
// per end hold (0 = no limit). The queue must contain exactly the pieces the
// PC can draw: one more than it places when it ends with a piece in hold.
inline PCAnswer findPC(const Board& board, Piece hold, Piece cur, const std::vector<Piece>& queue, int rows,
                       const std::vector<Piece>& order, int limit = 1, long long budget = 0,
                       bool allowHold = true) {
    PCAnswer out;
    if (cur == Piece::None || rows <= 0 || rows > MAX_ROWS || board.height() > rows) return out;
    int cells = Board::WIDTH * rows - board.count();
    if (cells <= 0 || cells % 4) return out;
    int place = cells / 4;
    int total = 1 + static_cast<int>(queue.size()) + (hold != Piece::None);
    int count[8] = {};
    ++count[static_cast<int>(cur)];
    if (hold != Piece::None) ++count[static_cast<int>(hold)];
    for (Piece p : queue) ++count[static_cast<int>(p)];

    for (Piece end : order) {
        if (static_cast<int>(out.ends.size()) >= limit) break;
        bool useHold = end != Piece::None;
        if (useHold ? (!allowHold || !count[static_cast<int>(end)] || place != total - 1)
                    : (hold != Piece::None || place != total))
            continue;
        detail::EndSearch search(queue, useHold, end, budget);
        for (int k = 0; k < NUM_PIECES; ++k) search.need[k] = count[k];
        if (useHold) --search.need[static_cast<int>(end)];
        // Parity lemma: with no T among the pieces placed, a PC from an empty
        // board needs an even number of L and J pieces.
        if (board.empty() && search.need[static_cast<int>(Piece::T)] == 0 &&
            (search.need[static_cast<int>(Piece::L)] + search.need[static_cast<int>(Piece::J)]) % 2)
            continue;
        uint64_t bits = detail::pack(board);
        bool found = search.feasible(bits, rows, total) && search.dfs(bits, cur, hold, 0, rows);
        out.nodes += search.nodes;
        if (found) out.ends.push_back({end, std::move(search.path)});
        else if (search.aborted) out.proved = false;
    }
    return out;
}

// ----------------------------------------------------------- value table ---

// V(i, h) estimates how many further PCs the offline optimum makes from the
// boundary state (i, h): i pieces drawn, piece h in hold. The model is
//
//     V(i, h) = alpha * (pieces left) + theta(h, b),    b = (i - 1) mod 7,
//
// where b is the bag offset of the boundary (pieces placed so far, mod 7).
// alpha and theta come from value_table.h, which tools/fit_value.py
// regenerates from offline DP runs.
struct ValueTable {
    double alpha = VALUE_ALPHA;
    double theta[NUM_PIECES][7];
    double meanTheta = 0;

    ValueTable() {
        for (int p = 0; p < NUM_PIECES; ++p)
            for (int b = 0; b < 7; ++b) theta[p][b] = VALUE_THETA[p][b], meanTheta += VALUE_THETA[p][b] / (NUM_PIECES * 7);
    }
    // Value of the boundary where `i` of `length` pieces are drawn and `h` is held.
    double operator()(int i, Piece h, int length) const {
        if (h == Piece::None) return average(i, length);
        return alpha * std::max(length - i, 0) + theta[static_cast<int>(h)][((i - 1) % 7 + 7) % 7];
    }
    // Value of reaching position i with an unknown held piece.
    double average(int i, int length) const { return alpha * std::max(length - i, 0) + meanTheta; }

    // Reads a table file: alpha, then theta as 7 rows (held piece I, O, T, L,
    // J, S, Z) of 7 numbers (bag offset 0..6). '#' starts a comment.
    static ValueTable fromFile(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot open value table " + path);
        std::vector<double> xs;
        for (std::string line; std::getline(in, line);) {
            std::istringstream row(line.substr(0, line.find('#')));
            for (double x; row >> x;) xs.push_back(x);
        }
        if (xs.size() != 1 + NUM_PIECES * 7) throw std::runtime_error("value table " + path + ": expected 50 numbers");
        ValueTable t;
        t.alpha = xs[0];
        t.meanTheta = 0;
        for (int p = 0; p < NUM_PIECES; ++p)
            for (int b = 0; b < 7; ++b) t.theta[p][b] = xs[size_t(1 + 7 * p + b)], t.meanTheta += t.theta[p][b] / (NUM_PIECES * 7);
        return t;
    }
};

// ---------------------------------------------------------------- player ---

enum class Mode { Myopic, Value };

struct Options {
    Mode mode = Mode::Myopic;
    int vision = 5;            // preview pieces the algorithm may see
    int samples = 40;          // sampled futures per batch
    int maxSamples = 120;      // most futures used for one decision
    int candidates = 6;        // first moves scored per decision
    long long budget = 20000;  // search nodes per sampled query
    uint64_t seed = 1;         // for sampling futures (the game itself is not random)
};

class Player {
   public:
    struct Stats {
        int decisions = 0;       // decisions made by sampling
        int failedAttempts = 0;  // 4-line attempts that became impossible
        int recoveryPCs = 0;     // PCs found while recovering
        int twoLine = 0;         // 2-line PCs taken
    };

    explicit Player(Options options, ValueTable value = {})
        : opt_(options), value_(value), rng_(options.seed) {}

    const Stats& stats() const { return stats_; }

    std::optional<Move> choose(const View& v) {
        see(v);
        length_ = v.length;
        allowHold_ = v.rules.hold;
        cap_ = v.rules.maxHeight;
        if (v.board.empty()) target_ = 4;
        if (!plan_.empty()) {
            Move m = plan_.front();
            plan_.pop_front();
            return m;
        }
        while (target_) {
            if (auto m = attempt(v, target_)) return m;
            // The attempt is dead: aim for a 6-line PC, then fall back to recovery.
            if (target_ == 4 && !v.board.empty()) ++stats_.failedAttempts;
            target_ = target_ == 4 ? 6 : 0;
        }
        return recover(v);
    }

   private:
    using Pieces = std::vector<Piece>;

    // --- what the algorithm knows about the sequence -------------------------
    void see(const View& v) {
        size_t total = size_t(v.taken) + v.preview.size();
        if (known_.empty()) {
            known_.assign(size_t(std::max(v.taken - 1, 0)), Piece::None);  // unseen if we start mid-game
            known_.push_back(v.current);
        }
        for (size_t k = v.preview.size() - std::min(v.preview.size(), total - known_.size()); known_.size() < total; ++k)
            known_.push_back(v.preview[k]);
    }
    // Pieces of the current bag that have not been seen yet.
    Pieces bagRest() const {
        size_t j0 = known_.size();
        Pieces rest;
        for (Piece p : ALL_PIECES)
            if (std::find(known_.begin() + long(7 * (j0 / 7)), known_.end(), p) == known_.end()) rest.push_back(p);
        return rest;
    }
    // Every possible next `u` unseen pieces, all equally likely (u <= 2).
    std::vector<Pieces> allFutures(int u) const {
        Pieces rest = bagRest();
        std::vector<Pieces> out;
        auto arrangements = [](Pieces from, int k) {  // ordered selections of k pieces
            std::vector<Pieces> res;
            std::sort(from.begin(), from.end());
            do {
                Pieces head(from.begin(), from.begin() + k);
                if (res.empty() || res.back() != head) res.push_back(head);
            } while (std::next_permutation(from.begin(), from.end()));
            return res;
        };
        int fromBag = std::min<int>(u, static_cast<int>(rest.size()));
        for (const Pieces& head : arrangements(rest, fromBag)) {
            if (fromBag == u) {
                out.push_back(head);
                continue;
            }
            for (const Pieces& tail : arrangements(Pieces(std::begin(ALL_PIECES), std::end(ALL_PIECES)), u - fromBag)) {
                Pieces f = head;
                f.insert(f.end(), tail.begin(), tail.end());
                out.push_back(f);
            }
        }
        return out;
    }
    // k random completions of the next `u` unseen pieces, drawn from the 7-bag.
    std::vector<Pieces> sampleFutures(int u, int k) {
        Pieces rest = bagRest();
        std::vector<Pieces> out;
        for (int s = 0; s < k; ++s) {
            Pieces f = rest;
            std::shuffle(f.begin(), f.end(), rng_);
            while (static_cast<int>(f.size()) < u) {
                Pieces bag(std::begin(ALL_PIECES), std::end(ALL_PIECES));
                std::shuffle(bag.begin(), bag.end(), rng_);
                f.insert(f.end(), bag.begin(), bag.end());
            }
            f.resize(size_t(u));
            out.push_back(f);
        }
        return out;
    }

    // --- what an outcome is worth ----------------------------------------------
    // End holds to try, best first.
    Pieces orderFor(int iEnd) {
        if (!allowHold_) return {Piece::None};
        Pieces order(std::begin(ALL_PIECES), std::end(ALL_PIECES));
        if (opt_.mode == Mode::Value)
            std::stable_sort(order.begin(), order.end(),
                             [&](Piece a, Piece b) { return value_(iEnd, a, length_) > value_(iEnd, b, length_); });
        else
            std::shuffle(order.begin(), order.end(), rng_);  // myopic: no preference between ends
        return order;
    }
    double reward(int iEnd, Piece end) const {
        return opt_.mode == Mode::Value ? 1.0 + value_(iEnd, end, length_) : 1.0;
    }
    // A failed attempt is valued as reaching a boundary about 5 pieces later
    // than planned, with no PC scored.
    double failValue(int iEnd) const { return opt_.mode == Mode::Value ? value_.average(iEnd + 5, length_) : 0.0; }

    // --- the decision ----------------------------------------------------------
    struct Window {
        int place;  // pieces still to place
        int qlen;   // pieces needed after the current one
    };
    // The pieces a PC needs when `rows` lines are still to be cleared.
    std::optional<Window> window(const Board& board, Piece held, int rows) const {
        if (rows <= 0 || rows > MAX_ROWS || board.height() > rows) return std::nullopt;
        if (cap_ > 0 && rows > cap_) return std::nullopt;
        int cells = Board::WIDTH * rows - board.count();
        if (cells <= 0 || cells % 4) return std::nullopt;
        int place = cells / 4;
        return Window{place, held != Piece::None || !allowHold_ ? place - 1 : place};
    }

    Move commit(const std::vector<Move>& moves) {
        plan_.assign(moves.begin() + 1, moves.end());
        return moves.front();
    }

    static Pieces firstN(const Pieces& v, int n) { return Pieces(v.begin(), v.begin() + std::min<long>(n, long(v.size()))); }

    // Next move towards a PC of `lines` lines, or nullopt if it is out of reach.
    std::optional<Move> attempt(const View& v, int lines) {
        const Pieces& pv = v.preview;
        int iNow = v.taken - 1, done = v.linesSincePC;
        std::optional<std::pair<double, std::vector<Move>>> best;

        // A PC that is completely visible: 2 lines.
        if (lines == 4) {
            auto w = window(v.board, v.held, 2 - done);
            if (w && w->qlen <= static_cast<int>(pv.size())) {
                int iEnd = iNow + 1 + w->qlen;
                PCAnswer ans = findPC(v.board, v.held, v.current, firstN(pv, w->qlen), 2 - done, orderFor(iEnd), 1, 0,
                                      allowHold_);
                if (!ans.ends.empty()) {
                    best = {reward(iEnd, ans.ends[0].endHold), ans.ends[0].moves};
                    if (opt_.mode == Mode::Myopic) {
                        ++stats_.twoLine;
                        return commit(best->second);
                    }
                }
            }
        }

        // A 4-line (or, when recovering, 6-line) PC, possibly with hidden pieces.
        auto w = window(v.board, v.held, lines - done);
        if (w && iNow + 1 + w->qlen <= v.length) {
            int iEnd = iNow + 1 + w->qlen;
            int hidden = w->qlen - static_cast<int>(pv.size());
            if (hidden <= 0) {
                PCAnswer ans = findPC(v.board, v.held, v.current, firstN(pv, w->qlen), lines - done, orderFor(iEnd), 1,
                                      lines == 4 ? 0 : 50 * opt_.budget, allowHold_);
                if (!ans.ends.empty()) {
                    double score = reward(iEnd, ans.ends[0].endHold);
                    if (!best || score > best->first) best = {score, ans.ends[0].moves};
                }
            } else if (auto res = hindsight(v.board, v.held, v.current, pv, hidden, lines - done, iEnd)) {
                if (!best || res->first > best->first) best = {res->first, {res->second}};
            }
        }
        if (!best) return std::nullopt;
        if (lines == 6 && best->second.size() > 1) ++stats_.recoveryPCs;
        if (lines == 4 && best->second.size() == 5 && done == 0 && v.board.empty()) ++stats_.twoLine;
        return commit(best->second);
    }

    // Best first move of a PC that must still clear `rows` lines when `hidden`
    // of its pieces are not visible yet. Returns (score, move), or nullopt if
    // no sampled future has a PC.
    std::optional<std::pair<double, Move>> hindsight(const Board& board, Piece held, Piece cur, const Pieces& pv,
                                                     int hidden, int rows, int iEnd) {
        bool exact = hidden <= 2;
        std::vector<Pieces> futures = exact ? allFutures(hidden) : sampleFutures(hidden, opt_.samples);
        Pieces order = orderFor(iEnd);
        ++stats_.decisions;
        auto join = [&](const Pieces& f) {
            Pieces q = pv;
            q.insert(q.end(), f.begin(), f.end());
            return q;
        };

        // Candidate first moves: the first moves of solutions to a few futures.
        struct Cand {
            Move move;
            int votes;
            double total;
        };
        std::vector<Cand> cands;
        size_t step = std::max<size_t>(1, futures.size() / 12);
        for (size_t k = 0; k < futures.size(); k += step) {
            PCAnswer ans = findPC(board, held, cur, join(futures[k]), rows, order, 2, opt_.budget, allowHold_);
            for (const PCEnd& e : ans.ends) {
                const Move& m = e.moves.front();
                auto it = std::find_if(cands.begin(), cands.end(), [&](const Cand& c) {
                    return c.move.hold == m.hold && c.move.placement.piece == m.placement.piece &&
                           c.move.placement.sameCells(m.placement);
                });
                if (it == cands.end()) cands.push_back({m, 1, 0.0});
                else ++it->votes;
            }
        }
        if (cands.empty()) return std::nullopt;
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.votes > b.votes; });
        if (static_cast<int>(cands.size()) > opt_.candidates) cands.resize(size_t(opt_.candidates));

        // Total reward of playing `m` now, summed over the futures in `batch`.
        auto score = [&](const Move& m, const std::vector<Pieces>& batch) {
            Board after = board;
            int left = rows - after.place(m.placement);
            double total = 0;
            for (const Pieces& f : batch) {
                Pieces q = join(f);
                Piece held2 = m.hold ? cur : held;
                size_t skip = m.hold && held == Piece::None ? 1 : 0;  // holding into an empty slot draws a piece
                if (after.empty() || q.size() <= skip) {  // the move itself ends the PC, or nothing follows
                    total += after.empty() && left == 0 ? reward(iEnd, held2) : failValue(iEnd);
                    continue;
                }
                Piece cur2 = q[skip];
                Pieces rest(q.begin() + long(skip) + 1, q.end());
                PCAnswer ans = findPC(after, held2, cur2, rest, left, order, 1, opt_.budget, allowHold_);
                total += ans.ends.empty() ? failValue(iEnd) : reward(iEnd, ans.ends[0].endHold);
            }
            return total;
        };

        // Score the candidates on the same futures. While several are close to
        // the best, draw more futures and score only those again.
        std::vector<size_t> alive(cands.size());
        for (size_t k = 0; k < alive.size(); ++k) alive[k] = k;
        int used = 0;
        for (;;) {
            for (size_t k : alive) cands[k].total += score(cands[k].move, futures);
            used += static_cast<int>(futures.size());
            double top = -1e18;
            for (size_t k : alive) top = std::max(top, cands[k].total / used);
            std::vector<size_t> keep;
            for (size_t k : alive)
                if (cands[k].total / used >= top - 0.03) keep.push_back(k);
            alive = keep;
            if (exact || alive.size() == 1 || used >= opt_.maxSamples) break;
            futures = sampleFutures(hidden, opt_.samples);
        }
        size_t bestK = alive[0];
        for (size_t k : alive)
            if (cands[k].total > cands[bestK].total) bestK = k;  // ties: more votes (earlier in the list)
        return std::make_pair(cands[bestK].total / used, cands[bestK].move);
    }

    // --- recovery after a failed attempt -----------------------------------------
    // Aim for the nearest PC of any height up to 6 rows; otherwise stack.
    std::optional<Move> recover(const View& v) {
        const Pieces& pv = v.preview;
        int iNow = v.taken - 1;
        int height = v.board.height();
        std::optional<std::pair<double, Move>> best;
        if (height > 0 && height <= MAX_ROWS) {
            int tried = 0;
            for (int rows = height; rows <= MAX_ROWS; ++rows) {
                auto w = window(v.board, v.held, rows);
                if (!w) continue;
                int iEnd = iNow + 1 + w->qlen;
                if (iEnd > v.length) break;
                int hidden = w->qlen - static_cast<int>(pv.size());
                if (hidden <= 0) {  // completely visible: solve it exactly
                    PCAnswer ans = findPC(v.board, v.held, v.current, firstN(pv, w->qlen), rows, orderFor(iEnd), 1,
                                          10 * opt_.budget, allowHold_);
                    if (!ans.ends.empty()) {
                        ++stats_.recoveryPCs;
                        return commit(ans.ends[0].moves);
                    }
                } else if (tried < 2) {  // hidden pieces: sample, with a smaller budget
                    ++tried;
                    Options saved = opt_;
                    opt_.samples = opt_.maxSamples = 16, opt_.candidates = 4;
                    auto res = hindsight(v.board, v.held, v.current, pv, hidden, rows, iEnd);
                    opt_ = saved;
                    if (res && (!best || res->first > best->first)) best = res;
                }
            }
        }
        if (best) return best->second;
        return stack(v);
    }

    // Low, flat, no holes: a plain stacking move for when no PC is in sight.
    static std::optional<Move> stack(const View& v) {
        std::optional<Move> best;
        double bestCost = 0;
        for (const Move& m : v.legalMoves()) {
            Board b = v.board;
            int cleared = b.place(m.placement);
            int bumps = 0;
            for (int x = 0; x + 1 < Board::WIDTH; ++x) bumps += std::abs(b.columnHeight(x) - b.columnHeight(x + 1));
            double cost = 10 * b.holes() + 2 * b.height() + bumps - 4 * cleared + (m.hold ? 0.1 : 0.0);
            if (!best || cost < bestCost) best = m, bestCost = cost;
        }
        return best;
    }

    Options opt_;
    ValueTable value_;
    std::mt19937_64 rng_;
    Stats stats_;
    Pieces known_;           // every piece seen so far, in sequence order
    std::deque<Move> plan_;  // moves already decided (a fully visible PC)
    int target_ = 4;         // lines the current attempt aims for (0 = recovering)
    int length_ = 0, cap_ = 0;
    bool allowHold_ = true;
};

// Plays the whole game with one of the online algorithms and reports on stderr.
// Environment: ONLINE_SEED sets the sampling seed, and ONLINE_TABLE names a
// value table file to use in place of value_table.h (see ValueTable::fromFile).
inline std::vector<Move> run(const Game& start, Options options) {
    if (const char* s = std::getenv("ONLINE_SEED"); s && *s) options.seed = std::strtoull(s, nullptr, 10);
    ValueTable table;
    if (const char* s = std::getenv("ONLINE_TABLE"); s && *s) {
        table = ValueTable::fromFile(s);
        std::cerr << "online: value table from " << s << "\n";
    }
    Player player(options, table);
    std::vector<Move> moves = playOnline(start, options.vision, [&](const View& v) { return player.choose(v); });
    Game game = start;
    int two = 0, four = 0, longer = 0, segment = 0;
    for (const Move& m : moves) {
        game.playUnchecked(m);
        if (game.lastWasPC()) {
            int lines = (game.placed() - segment) * 2 / 5;
            (lines == 2 ? two : lines == 4 ? four : longer)++;
            segment = game.placed();
        }
    }
    const Player::Stats& s = player.stats();
    std::cerr << "online (" << (options.mode == Mode::Value ? "value-guided" : "myopic") << ", vision "
              << options.vision << "): " << game.pcCount() << " perfect clears (" << two << " two-line, " << four
              << " four-line, " << longer << " longer)\n"
              << "online: " << s.failedAttempts << " failed 4-line attempts, " << s.decisions
              << " decisions made by sampling\n";
    return moves;
}

}  // namespace online
}  // namespace tetris
