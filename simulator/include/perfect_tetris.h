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

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "tetris.h"

extern char** environ;

namespace tetris {
namespace fumen {

constexpr const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr const char* COMMENT_TABLE =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
constexpr int FIELD_TOP = 23, FIELD_BLOCKS = (FIELD_TOP + 1) * Board::WIDTH;

// Fumen piece ids: 1 I, 2 L, 3 O, 4 Z, 5 T, 6 J, 7 S.
inline Piece fromId(int id) {
    constexpr Piece map[8] = {Piece::None, Piece::I, Piece::L, Piece::O, Piece::Z, Piece::T, Piece::J, Piece::S};
    return id > 0 && id < 8 ? map[id] : Piece::None;
}
// Fumen stores rotation as 0 reverse, 1 right, 2 spawn, 3 left; this converts both ways.
constexpr int ROT_MAP[4] = {2, 1, 0, 3};

// Fumen anchors O, I, S and Z differently from their rotation centre.
inline Cell legacyShift(Piece p, int rot) {
    static const Cell O[4] = {{0, -1}, {0, 0}, {1, 0}, {1, -1}}, I[4] = {{0, 0}, {0, 0}, {1, 0}, {0, -1}},
                      S[4] = {{0, -1}, {-1, 0}, {0, 0}, {0, 0}}, Z[4] = {{0, -1}, {0, 0}, {0, 0}, {1, 0}};
    switch (p) {
        case Piece::O: return O[rot];
        case Piece::I: return I[rot];
        case Piece::S: return S[rot];
        case Piece::Z: return Z[rot];
        default: return {0, 0};
    }
}

inline void push(std::string& out, long long v, int n) {
    for (int i = 0; i < n; ++i, v /= 64) out += B64[v % 64];
}

// The quiz comment perfect-tetris reads its queue from.
inline std::string quizComment(Piece hold, const std::vector<Piece>& queue) {
    std::string s = "#Q=[";
    if (hold != Piece::None) s += toChar(hold);
    s += "](";
    if (!queue.empty()) s += toChar(queue[0]);
    s += ")";
    for (size_t i = 1; i < queue.size(); ++i) s += toChar(queue[i]);
    return s;
}

// A one-page fumen: empty field, no piece, the given comment.
inline std::string encodeEmptyPage(const std::string& comment) {
    std::string esc;  // JavaScript escape()
    for (unsigned char c : comment) {
        if (std::isalnum(c) || std::string("@*_+-./").find(char(c)) != std::string::npos) {
            esc += char(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            esc += buf;
        }
    }
    std::string out = "v115@";
    push(out, 8LL * FIELD_BLOCKS + FIELD_BLOCKS - 1, 2);  // field unchanged
    push(out, 0, 1);                                      // repeat count
    long long v = 0;                                      // lock
    v = v * 2 + 1;                                        // has comment
    v = v * 2 + 1;                                        // colour (first page)
    v = v * 4 * FIELD_BLOCKS * 4 * 8;                     // mirror, rise, position, rotation, piece = 0
    push(out, v, 3);
    push(out, static_cast<long long>(esc.size()), 2);
    for (size_t i = 0; i < esc.size(); i += 4) {
        long long c = 0;
        for (int k = 3; k >= 0; --k) {
            char ch = i + k < esc.size() ? esc[i + k] : ' ';
            const char* at = std::strchr(COMMENT_TABLE, ch);
            c = c * 96 + (at ? at - COMMENT_TABLE : 0);
        }
        push(out, c, 5);
    }
    return out;
}

// The piece placed on each page of a fumen, in order. Returns std::nullopt if
// the string is not a valid v115 fumen.
inline std::optional<std::vector<Placement>> decodePlacements(const std::string& text) {
    size_t at = text.find("115@");
    if (at == std::string::npos) return std::nullopt;
    std::string data;
    for (size_t i = at + 4; i < text.size(); ++i)
        if (std::strchr(B64, text[i]) && text[i] != '\0') data += text[i];
        else if (text[i] != '?') break;
    size_t pos = 0;
    bool ok = true;
    auto poll = [&](int n) {
        long long v = 0, mul = 1;
        for (int i = 0; i < n; ++i, mul *= 64) {
            if (pos >= data.size()) return ok = false, 0LL;
            v += (std::strchr(B64, data[pos++]) - B64) * mul;
        }
        return v;
    };

    std::vector<Placement> out;
    int repeat = 0;
    while (ok && pos < data.size()) {
        if (repeat > 0) {
            --repeat;
        } else {
            for (int i = 0; i < FIELD_BLOCKS && ok;) {
                long long run = poll(2);
                int len = static_cast<int>(run % FIELD_BLOCKS) + 1;
                if (run / FIELD_BLOCKS == 8 && len == FIELD_BLOCKS) repeat = static_cast<int>(poll(1));
                i += len;
            }
        }
        long long v = poll(3);
        int type = static_cast<int>(v % 8);
        v /= 8;
        int rot = ROT_MAP[v % 4];
        v /= 4;
        int loc = static_cast<int>(v % FIELD_BLOCKS);
        v /= FIELD_BLOCKS * 8;  // position, rise, mirror, colour
        bool hasComment = v % 2;
        if (Piece p = fromId(type); p != Piece::None) {
            Cell s = legacyShift(p, rot);
            out.push_back({p, rot, loc % Board::WIDTH + s.x, FIELD_TOP - loc / Board::WIDTH - 1 + s.y});
        }
        if (hasComment) {
            long long len = poll(2);
            for (long long i = 0; i < len; i += 4) poll(5);
        }
    }
    if (!ok) return std::nullopt;
    return out;
}

}  // namespace fumen

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

        auto [text, finished] = run(args, timeoutSec);
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
    // Runs the binary and returns (stdout + stderr, finished before timeout).
    static std::pair<std::string, bool> run(const std::vector<std::string>& args, double timeoutSec) {
        // Create the pipe and spawn under a lock with close-on-exec set, so a
        // process started by another thread never inherits this pipe (which
        // would hold it open and delay EOF until that process exits).
        static std::mutex spawnLock;
        int fds[2];
        pid_t pid;
        {
            std::lock_guard<std::mutex> lock(spawnLock);
            if (pipe(fds) != 0) throw std::runtime_error("perfect-tetris: pipe failed");
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
            posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
            std::vector<char*> argv;
            for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            int err = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            close(fds[1]);
            if (err != 0) {
                close(fds[0]);
                throw std::runtime_error("perfect-tetris: cannot run " + args[0] + ": " + std::strerror(err));
            }
        }

        std::string text;
        bool finished = true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSec);
        char buf[4096];
        for (;;) {
            int wait = -1;
            if (timeoutSec > 0) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) {
                    finished = false;
                    break;
                }
                wait = static_cast<int>(left.count());
            }
            pollfd p{fds[0], POLLIN, 0};
            if (::poll(&p, 1, wait) == 0) continue;  // re-check the deadline
            ssize_t n = read(fds[0], buf, sizeof buf);
            if (n <= 0) break;
            text.append(buf, static_cast<size_t>(n));
        }
        if (!finished) kill(pid, SIGKILL);
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (finished && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            throw std::runtime_error("perfect-tetris exited abnormally: " + text.substr(0, 200));
        return {text, finished};
    }

    std::string binary_, kicks_;
};

}  // namespace tetris
