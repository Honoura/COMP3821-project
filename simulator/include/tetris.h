// tetris.h — helper library for COMP3821 perfect-clear strategies.
//
// A strategy is a single .cpp file that includes this header and defines
//
//     std::vector<tetris::Move> solve(const tetris::Game& start);
//
// `start` is a fresh game over the full piece sequence (offline setting: the
// whole queue is visible through Game::queue()). Return the moves to play in
// order. The runner replays them on its own copy of the game, so an illegal
// move ends the game there; returning fewer moves than pieces simply stops
// early. Anything printed to stdout/stderr shows up in the visualiser console.
//
// Conventions
//   * Board is 10 wide, x = 0..9 left to right, y = 0 is the bottom row.
//   * A placement is (piece, rotation, x, y) where (x, y) is the piece's
//     rotation centre, using the same convention as tetris-fumen / fumen.zui.jp.
//   * Rotation: SPAWN = 0, RIGHT = 1 (clockwise), REVERSE = 2, LEFT = 3.
//   * Movement is SRS with standard wall kicks (no 180 rotation) and gravity
//     disabled: any position reachable by shifts, rotations and soft drops.
//   * A move may hold first. If the hold slot is empty, holding puts the
//     current piece in hold and the next piece becomes current; otherwise the
//     current and held pieces swap. The (new) current piece is then placed.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace tetris {

// ---------------------------------------------------------------- pieces ---

enum class Piece : uint8_t { I, O, T, L, J, S, Z, None };

constexpr int NUM_PIECES = 7;
constexpr Piece ALL_PIECES[NUM_PIECES] = {Piece::I, Piece::O, Piece::T, Piece::L,
                                          Piece::J, Piece::S, Piece::Z};

enum Rotation : int { SPAWN = 0, RIGHT = 1, REVERSE = 2, LEFT = 3 };

inline int rotateCW(int rot) { return (rot + 1) & 3; }
inline int rotateCCW(int rot) { return (rot + 3) & 3; }

inline char toChar(Piece p) { return "IOTLJSZ."[static_cast<int>(p)]; }

inline Piece fromChar(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'I': return Piece::I;
        case 'O': return Piece::O;
        case 'T': return Piece::T;
        case 'L': return Piece::L;
        case 'J': return Piece::J;
        case 'S': return Piece::S;
        case 'Z': return Piece::Z;
        default: return Piece::None;
    }
}

// "IOTLJSZ..." -> pieces. Characters that are not piece letters are ignored.
inline std::vector<Piece> parseQueue(const std::string& s) {
    std::vector<Piece> out;
    for (char c : s)
        if (fromChar(c) != Piece::None) out.push_back(fromChar(c));
    return out;
}

inline std::string queueToString(const std::vector<Piece>& q) {
    std::string s;
    for (Piece p : q) s += toChar(p);
    return s;
}

// Seeded 7-bag randomiser: every block of 7 pieces is a permutation of IOTLJSZ.
inline std::vector<Piece> sevenBag(uint64_t seed, int length) {
    std::mt19937_64 rng(seed);
    std::vector<Piece> out;
    while (static_cast<int>(out.size()) < length) {
        std::array<Piece, NUM_PIECES> bag;
        std::copy(std::begin(ALL_PIECES), std::end(ALL_PIECES), bag.begin());
        for (int i = NUM_PIECES - 1; i > 0; --i)
            std::swap(bag[i], bag[std::uniform_int_distribution<int>(0, i)(rng)]);
        for (Piece p : bag)
            if (static_cast<int>(out.size()) < length) out.push_back(p);
    }
    return out;
}

// Position within the current 7-bag after `taken` pieces have been drawn.
inline int bagPosition(int taken) { return taken % 7; }

struct Cell {
    int x, y;
};

namespace detail {

// Spawn-orientation cells relative to the rotation centre (y up).
constexpr int BASE[NUM_PIECES][4][2] = {
    {{0, 0}, {-1, 0}, {1, 0}, {2, 0}},   // I
    {{0, 0}, {1, 0}, {0, 1}, {1, 1}},    // O
    {{0, 0}, {-1, 0}, {1, 0}, {0, 1}},   // T
    {{0, 0}, {-1, 0}, {1, 0}, {1, 1}},   // L
    {{0, 0}, {-1, 0}, {1, 0}, {-1, 1}},  // J
    {{0, 0}, {-1, 0}, {0, 1}, {1, 1}},   // S
    {{0, 0}, {1, 0}, {0, 1}, {-1, 1}},   // Z
};

using ShapeTable = std::array<std::array<std::array<Cell, 4>, 4>, NUM_PIECES>;

inline const ShapeTable& shapes() {
    static const ShapeTable table = [] {
        ShapeTable t{};
        for (int p = 0; p < NUM_PIECES; ++p)
            for (int i = 0; i < 4; ++i) {
                int x = BASE[p][i][0], y = BASE[p][i][1];
                for (int r = 0; r < 4; ++r) {
                    t[p][r][i] = {x, y};
                    int nx = y, ny = -x;  // rotate clockwise
                    x = nx, y = ny;
                }
            }
        return t;
    }();
    return table;
}

// SRS offset tables. The kick for test i when rotating a -> b is
// OFFSET[a][i] - OFFSET[b][i].
constexpr int JLSTZ_OFFSETS[4][5][2] = {
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}},
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}},
};
constexpr int I_OFFSETS[4][5][2] = {
    {{0, 0}, {-1, 0}, {2, 0}, {-1, 0}, {2, 0}},
    {{-1, 0}, {0, 0}, {0, 0}, {0, 1}, {0, -2}},
    {{-1, 1}, {1, 1}, {-2, 1}, {1, 0}, {-2, 0}},
    {{0, 1}, {0, 1}, {0, 1}, {0, -1}, {0, 2}},
};
constexpr int O_OFFSETS[4][2] = {{0, 0}, {0, -1}, {-1, -1}, {-1, 0}};

}  // namespace detail

// The four cells of `piece` in rotation `rot`, relative to its centre.
inline const std::array<Cell, 4>& shape(Piece piece, int rot) {
    return detail::shapes()[static_cast<int>(piece)][rot & 3];
}

// Number of SRS kick tests for a piece (O has a single, fixed offset).
inline int kickCount(Piece piece) { return piece == Piece::O ? 1 : 5; }

// Translation for kick test `test` when rotating `from` -> `to`.
inline Cell kick(Piece piece, int from, int to, int test) {
    if (piece == Piece::O)
        return {detail::O_OFFSETS[from][0] - detail::O_OFFSETS[to][0],
                detail::O_OFFSETS[from][1] - detail::O_OFFSETS[to][1]};
    const auto& t = piece == Piece::I ? detail::I_OFFSETS : detail::JLSTZ_OFFSETS;
    return {t[from][test][0] - t[to][test][0], t[from][test][1] - t[to][test][1]};
}

struct Placement {
    Piece piece = Piece::None;
    int rot = SPAWN;
    int x = 0, y = 0;

    std::array<Cell, 4> cells() const {
        std::array<Cell, 4> out = shape(piece, rot);
        for (Cell& c : out) c.x += x, c.y += y;
        return out;
    }
    int minY() const {
        int m = 1 << 30;
        for (Cell c : cells()) m = std::min(m, c.y);
        return m;
    }
    int maxY() const {
        int m = -(1 << 30);
        for (Cell c : cells()) m = std::max(m, c.y);
        return m;
    }
    // Identifies the set of cells covered, so placements that look the same
    // (e.g. an S in SPAWN vs REVERSE one row apart) compare equal.
    uint64_t key() const {
        int lo = minY();
        uint64_t bits = 0;
        for (Cell c : cells()) bits |= uint64_t(1) << ((c.y - lo) * 10 + c.x);
        return (uint64_t(lo + 64) << 40) | bits;
    }
    bool sameCells(const Placement& o) const { return key() == o.key(); }
};

// Colour offset from Lemma 2.3: cells in even (black) columns minus cells in
// odd (white) columns. 0 for O, S, Z, flat T, flat I; ±2 for L, J, vertical T;
// ±4 for vertical I. Over any PC from an empty board the offsets sum to 0.
inline int parityOffset(const Placement& p) {
    int off = 0;
    for (Cell c : p.cells()) off += (c.x % 2 == 0) ? 1 : -1;
    return off;
}

// ----------------------------------------------------------------- board ---

class Board {
   public:
    static constexpr int WIDTH = 10;
    static constexpr int HEIGHT = 40;
    static constexpr uint16_t FULL_ROW = (1u << WIDTH) - 1;

    bool filled(int x, int y) const {
        if (x < 0 || x >= WIDTH || y < 0) return true;
        if (y >= HEIGHT) return false;
        return (rows_[y] >> x) & 1;
    }
    void set(int x, int y, bool on = true) {
        if (on) rows_[y] |= uint16_t(1u << x);
        else rows_[y] &= uint16_t(~(1u << x));
    }
    uint16_t row(int y) const { return rows_[y]; }

    bool fits(const Placement& p) const {
        for (Cell c : p.cells())
            if (c.x < 0 || c.x >= WIDTH || c.y < 0 || c.y >= HEIGHT || filled(c.x, c.y))
                return false;
        return true;
    }

    // Locks the piece (assumed to fit) and clears full lines. Returns lines cleared.
    int place(const Placement& p) {
        for (Cell c : p.cells()) set(c.x, c.y);
        return clearLines();
    }

    int clearLines() {
        int w = 0;
        for (int r = 0; r < HEIGHT; ++r)
            if (rows_[r] != FULL_ROW) rows_[w++] = rows_[r];
        int cleared = HEIGHT - w;
        while (w < HEIGHT) rows_[w++] = 0;
        return cleared;
    }

    bool empty() const {
        for (uint16_t r : rows_)
            if (r) return false;
        return true;
    }
    // Index of the highest filled row + 1 (0 for an empty board).
    int height() const {
        for (int y = HEIGHT - 1; y >= 0; --y)
            if (rows_[y]) return y + 1;
        return 0;
    }
    int columnHeight(int x) const {
        for (int y = HEIGHT - 1; y >= 0; --y)
            if (filled(x, y)) return y + 1;
        return 0;
    }
    int count() const {
        int n = 0;
        for (uint16_t r : rows_) n += __builtin_popcount(r);
        return n;
    }
    // Empty cells with a filled cell somewhere above them in the same column.
    int holes() const {
        int n = 0, h = height();
        for (int x = 0; x < WIDTH; ++x) {
            bool covered = false;
            for (int y = h - 1; y >= 0; --y) {
                if (filled(x, y)) covered = true;
                else if (covered) ++n;
            }
        }
        return n;
    }

    // ASCII picture of the bottom `rows` rows ('#' filled, '.' empty).
    std::string toString(int rows = -1) const {
        if (rows < 0) rows = std::max(height(), 1);
        std::string s;
        for (int y = rows - 1; y >= 0; --y) {
            for (int x = 0; x < WIDTH; ++x) s += filled(x, y) ? '#' : '.';
            s += '\n';
        }
        return s;
    }

    bool operator==(const Board& o) const { return rows_ == o.rows_; }

   private:
    std::array<uint16_t, HEIGHT> rows_{};
};

// Where pieces appear: centre (4, 20), just above the 20-row visible field.
inline Placement spawnPlacement(Piece piece) { return {piece, SPAWN, 4, 20}; }

// Tries an SRS rotation (dir = +1 clockwise, -1 anticlockwise) with wall kicks.
inline std::optional<Placement> rotate(const Board& board, const Placement& p, int dir) {
    int to = dir > 0 ? rotateCW(p.rot) : rotateCCW(p.rot);
    for (int t = 0; t < kickCount(p.piece); ++t) {
        Cell k = kick(p.piece, p.rot, to, t);
        Placement q{p.piece, to, p.x + k.x, p.y + k.y};
        if (board.fits(q)) return q;
    }
    return std::nullopt;
}

// Every distinct resting position of `piece` reachable from spawn by shifts,
// SRS rotations and soft drops. Positions covering the same cells are only
// listed once. If maxHeight > 0, placements with any cell at y >= maxHeight
// are excluded (the "height cap").
inline std::vector<Placement> legalPlacements(const Board& board, Piece piece, int maxHeight = 0) {
    std::vector<Placement> out;
    if (piece == Piece::None) return out;
    Placement spawn = spawnPlacement(piece);
    if (!board.fits(spawn)) return out;

    constexpr int XO = 3, YO = 3, XS = Board::WIDTH + 6, YS = Board::HEIGHT + 6;
    std::vector<uint8_t> seen(4 * XS * YS, 0);
    auto index = [&](const Placement& p) { return (p.rot * YS + (p.y + YO)) * XS + (p.x + XO); };
    std::vector<Placement> stack;
    auto push = [&](const Placement& p) {
        if (p.x + XO < 0 || p.x + XO >= XS || p.y + YO < 0 || p.y + YO >= YS) return;
        uint8_t& s = seen[index(p)];
        if (!s && board.fits(p)) s = 1, stack.push_back(p);
    };

    // Above the stack every (rotation, column) is reachable directly from
    // spawn, so seed the search there instead of walking down from row 20.
    int top = board.height();
    if (top <= 16) {
        for (int r = 0; r < 4; ++r) {
            Placement p{piece, r, 0, 0};
            int lo = 0;
            for (Cell c : shape(piece, r)) lo = std::min(lo, c.y);
            p.y = top - lo;
            for (p.x = -2; p.x < Board::WIDTH + 2; ++p.x) push(p);
        }
    } else {
        push(spawn);
    }

    std::unordered_set<uint64_t> keys;
    while (!stack.empty()) {
        Placement p = stack.back();
        stack.pop_back();
        push({p.piece, p.rot, p.x - 1, p.y});
        push({p.piece, p.rot, p.x + 1, p.y});
        Placement down{p.piece, p.rot, p.x, p.y - 1};
        if (board.fits(down)) push(down);
        else if ((maxHeight <= 0 || p.maxY() < maxHeight) && keys.insert(p.key()).second)
            out.push_back(p);
        for (int dir : {1, -1})
            if (auto q = rotate(board, p, dir)) push(*q);
    }
    std::sort(out.begin(), out.end(), [](const Placement& a, const Placement& b) {
        return a.rot != b.rot ? a.rot < b.rot : a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    return out;
}

// ------------------------------------------------------------------ game ---

struct Rules {
    bool hold = true;   // allow the hold slot
    int maxHeight = 0;  // height cap; 0 = no cap
};

struct Move {
    bool hold = false;    // press hold before placing
    Placement placement;  // where the (post-hold) current piece goes
};

class Game {
   public:
    Game() = default;
    explicit Game(std::vector<Piece> queue, Rules rules = {})
        : queue_(std::make_shared<const std::vector<Piece>>(std::move(queue))), rules_(rules) {
        current_ = draw();
        if (current_ != Piece::None && !board_.fits(spawnPlacement(current_))) toppedOut_ = true;
    }

    // --- state --------------------------------------------------------------
    const Board& board() const { return board_; }
    const Rules& rules() const { return rules_; }
    Piece current() const { return toppedOut_ ? Piece::None : current_; }
    Piece held() const { return hold_; }
    // The full piece sequence (offline setting: everything is visible).
    const std::vector<Piece>& queue() const { return *queue_; }
    // Pieces taken from the sequence so far, including the current and held
    // pieces (the `i` of Lemma 2.5).
    int taken() const { return next_; }
    // Upcoming pieces after the current one; peek(0) is the next piece.
    Piece peek(int k) const {
        int i = next_ + k;
        return i >= 0 && i < static_cast<int>(queue_->size()) ? (*queue_)[i] : Piece::None;
    }
    std::vector<Piece> preview(int n) const {
        std::vector<Piece> out;
        for (int k = 0; k < n && peek(k) != Piece::None; ++k) out.push_back(peek(k));
        return out;
    }
    int remaining() const { return static_cast<int>(queue_->size()) - next_; }
    int placed() const { return placed_; }
    int pcCount() const { return pcs_; }
    int linesSincePC() const { return linesSincePC_; }
    bool lastWasPC() const { return lastWasPC_; }
    int lastCleared() const { return lastCleared_; }
    bool toppedOut() const { return toppedOut_; }
    bool over() const { return current() == Piece::None; }
    const std::vector<Move>& history() const { return history_; }

    bool canHold() const {
        return rules_.hold && !over() && (hold_ != Piece::None || next_ < static_cast<int>(queue_->size()));
    }
    // The piece that would be placed after (optionally) holding.
    Piece pieceAfter(bool hold) const {
        if (!hold) return current();
        if (!canHold()) return Piece::None;
        return hold_ != Piece::None ? hold_ : (*queue_)[next_];
    }

    // All legal moves from here: every placement of the current piece, plus
    // every placement of the hold piece if holding is allowed and differs.
    std::vector<Move> legalMoves() const {
        std::vector<Move> out;
        if (over()) return out;
        for (const Placement& p : legalPlacements(board_, current_, rules_.maxHeight)) out.push_back({false, p});
        Piece alt = pieceAfter(true);
        if (canHold() && alt != current_)
            for (const Placement& p : legalPlacements(board_, alt, rules_.maxHeight)) out.push_back({true, p});
        return out;
    }

    bool isLegal(const Move& m, std::string* why = nullptr) const {
        auto fail = [&](const std::string& msg) {
            if (why) *why = msg;
            return false;
        };
        if (over()) return fail(toppedOut_ ? "game over (topped out)" : "no pieces left");
        if (m.hold && !rules_.hold) return fail("hold is disabled");
        if (m.hold && !canHold()) return fail("cannot hold: no piece to swap in");
        Piece expect = pieceAfter(m.hold);
        if (m.placement.piece != expect)
            return fail(std::string("expected piece ") + toChar(expect) + ", move places " +
                        toChar(m.placement.piece));
        if (!board_.fits(m.placement)) return fail("placement overlaps the stack or the walls");
        if (rules_.maxHeight > 0 && m.placement.maxY() >= rules_.maxHeight)
            return fail("placement exceeds the height cap of " + std::to_string(rules_.maxHeight));
        uint64_t k = m.placement.key();
        for (const Placement& p : legalPlacements(board_, expect, rules_.maxHeight))
            if (p.key() == k) return true;
        return fail("placement is not reachable from spawn (or is not resting on the stack)");
    }

    // Plays a move. Returns false (leaving the game unchanged) if it is illegal.
    bool play(const Move& m, std::string* why = nullptr) {
        if (!isLegal(m, why)) return false;
        playUnchecked(m);
        return true;
    }

    // Plays a move without the reachability check. Faster for searches that
    // already generated the move with legalMoves()/legalPlacements().
    void playUnchecked(const Move& m) {
        if (m.hold) {
            if (hold_ == Piece::None) hold_ = current_, current_ = draw();
            else std::swap(hold_, current_);
        }
        lastCleared_ = board_.place(m.placement);
        linesSincePC_ += lastCleared_;
        lastWasPC_ = board_.empty();
        if (lastWasPC_) ++pcs_, linesSincePC_ = 0;
        ++placed_;
        history_.push_back(m);
        current_ = draw();
        if (current_ != Piece::None && !board_.fits(spawnPlacement(current_))) toppedOut_ = true;
    }

   private:
    Piece draw() { return next_ < static_cast<int>(queue_->size()) ? (*queue_)[next_++] : Piece::None; }

    std::shared_ptr<const std::vector<Piece>> queue_ = std::make_shared<const std::vector<Piece>>();
    Rules rules_;
    Board board_;
    Piece current_ = Piece::None, hold_ = Piece::None;
    int next_ = 0, placed_ = 0, pcs_ = 0, linesSincePC_ = 0, lastCleared_ = 0;
    bool lastWasPC_ = false, toppedOut_ = false;
    std::vector<Move> history_;
};

// ------------------------------------------------------- PC search helper ---

namespace detail {

struct PCSearch {
    const std::vector<Piece>& queue;
    bool allowHold;
    long long nodeLimit;
    long long nodes = 0;
    std::vector<Move> path = {};

    struct Key {
        std::array<uint16_t, 8> rows;
        int16_t next;
        uint8_t cur, hold, height;
        bool operator==(const Key& o) const {
            return rows == o.rows && next == o.next && cur == o.cur && hold == o.hold && height == o.height;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = 1469598103934665603ull;
            for (uint16_t r : k.rows) h = (h ^ r) * 1099511628211ull;
            h = (h ^ uint64_t(k.next)) * 1099511628211ull;
            h = (h ^ (uint64_t(k.cur) << 16 | uint64_t(k.hold) << 8 | k.height)) * 1099511628211ull;
            return h;
        }
    };
    std::unordered_set<Key, KeyHash> failed;

    Piece at(int i) const { return i < static_cast<int>(queue.size()) ? queue[i] : Piece::None; }

    PCSearch(const std::vector<Piece>& q, bool hold, long long limit)
        : queue(q), allowHold(hold), nodeLimit(limit) {}

    // Necessary condition: every connected region of empty cells below the
    // PC line must be filled by whole pieces, so its size is a multiple of 4.
    static bool regionsOk(const Board& b, int height) {
        uint64_t open = 0;
        for (int y = 0; y < height; ++y)
            open |= uint64_t(~b.row(y) & Board::FULL_ROW) << (y * Board::WIDTH);
        while (open) {
            uint64_t region = open & (~open + 1), prev = 0;
            while (region != prev) {
                prev = region;
                uint64_t grow = region | (region << Board::WIDTH) | (region >> Board::WIDTH);
                grow |= ((region << 1) & ~COL0) | ((region >> 1) & ~COL9);
                region = grow & open;
            }
            if (__builtin_popcountll(region) % 4) return false;
            open &= ~region;
        }
        return true;
    }
    static constexpr uint64_t COL0 = 0x0004010040100401ull;  // x = 0 in rows 0..5
    static constexpr uint64_t COL9 = COL0 << 9;

    bool dfs(const Board& board, Piece cur, Piece hold, int next, int height) {
        if (cur == Piece::None || height <= 0) return false;
        if (++nodes > nodeLimit) return false;
        Key key{{}, int16_t(next), uint8_t(cur), uint8_t(hold), uint8_t(height)};
        for (int y = 0; y < height && y < 8; ++y) key.rows[y] = board.row(y);
        if (failed.count(key)) return false;

        for (int useHold = 0; useHold < (allowHold ? 2 : 1); ++useHold) {
            Piece piece = cur, newHold = hold;
            int n = next;
            if (useHold) {
                if (hold == Piece::None) {
                    piece = at(n++);
                    newHold = cur;
                } else {
                    piece = hold;
                    newHold = cur;
                }
                if (piece == Piece::None || piece == cur) continue;
            }
            Piece newCur = at(n);
            auto options = legalPlacements(board, piece, height);
            // Try low placements first: they tend to lead to solutions sooner.
            std::sort(options.begin(), options.end(), [](const Placement& a, const Placement& b) {
                return a.maxY() != b.maxY() ? a.maxY() < b.maxY() : a.minY() < b.minY();
            });
            for (const Placement& p : options) {
                Board nb = board;
                int cleared = nb.place(p);
                int nh = height - cleared;
                if (!regionsOk(nb, nh)) continue;
                path.push_back({bool(useHold), p});
                if (nb.empty() || dfs(nb, newCur, newHold, n + 1, nh)) return true;
                path.pop_back();
                if (nodes > nodeLimit) return false;
            }
        }
        failed.insert(key);
        return false;
    }
};

}  // namespace detail

// Searches for a sequence of moves from the current state of `game` that
// ends with an empty board after clearing exactly `height` lines (counting
// the cells already on the board). Respects the game's hold rule. Returns
// std::nullopt if there is none, or if the search exceeds `nodeLimit`
// search nodes. Heights 1..6 are supported. The moves can be fed straight
// into Game::play().
inline std::optional<std::vector<Move>> findPerfectClear(const Game& game, int height,
                                                         long long nodeLimit = 2'000'000) {
    const Board& b = game.board();
    if (game.over() || height <= 0 || height > 6 || b.height() > height) return std::nullopt;
    if (game.rules().maxHeight > 0 && height > game.rules().maxHeight) return std::nullopt;
    int emptyCells = Board::WIDTH * height - b.count();
    if (emptyCells <= 0 || emptyCells % 4) return std::nullopt;
    int needed = emptyCells / 4;
    int available = 1 + game.remaining() + (game.held() != Piece::None);
    if (needed > available) return std::nullopt;

    detail::PCSearch search(game.queue(), game.rules().hold, nodeLimit);
    if (!search.dfs(b, game.current(), game.held(), game.taken(), height)) return std::nullopt;
    return search.path;
}

}  // namespace tetris

// The function every strategy file must define.
std::vector<tetris::Move> solve(const tetris::Game& start);
