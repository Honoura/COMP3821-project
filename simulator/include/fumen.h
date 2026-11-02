// fumen.h — encode and decode fumen (v115) strings, the format used by
// fumen.zui.jp, solution-finder and perfect-tetris.
#pragma once

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "tetris.h"

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

// One decoded page: the field shown (before this page's piece locks; row
// y = 0 is the bottom, cells hold fumen ids 0 empty, 1 I, 2 L, 3 O, 4 Z,
// 5 T, 6 J, 7 S, 8 garbage), the page's piece if any, and its comment.
struct Page {
    std::array<std::array<uint8_t, Board::WIDTH>, FIELD_TOP> field{};
    std::optional<Placement> piece;
    std::string comment;  // as stored, i.e. still %-escaped
};

// Decodes a v115 fumen (a URL around it is fine). Returns std::nullopt if it
// is not valid.
inline std::optional<std::vector<Page>> decode(const std::string& text) {
    size_t at = text.find("115@");
    if (at == std::string::npos) return std::nullopt;
    std::string data;
    for (size_t i = at + 4; i < text.size(); ++i)
        if (text[i] != '\0' && std::strchr(B64, text[i])) data += text[i];
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

    std::vector<Page> pages;
    std::array<int, FIELD_BLOCKS> prev{};  // field after the previous page, top row first
    std::string comment;
    int repeat = 0;
    while (ok && pos < data.size()) {
        std::array<int, FIELD_BLOCKS> field = prev;
        if (repeat > 0) {
            --repeat;
        } else {
            for (int i = 0; i < FIELD_BLOCKS && ok;) {
                long long run = poll(2);
                int diff = static_cast<int>(run / FIELD_BLOCKS) - 8, len = static_cast<int>(run % FIELD_BLOCKS) + 1;
                if (diff == 0 && len == FIELD_BLOCKS) repeat = static_cast<int>(poll(1));
                for (int k = 0; k < len && i < FIELD_BLOCKS; ++k, ++i) field[i] += diff;
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
        bool lock = (v / 2) % 2 == 0;

        Page page;
        for (int y = 0; y < FIELD_TOP; ++y)
            for (int x = 0; x < Board::WIDTH; ++x)
                page.field[y][x] = static_cast<uint8_t>(field[(FIELD_TOP - 1 - y) * Board::WIDTH + x]);
        if (Piece p = fromId(type); p != Piece::None) {
            Cell s = legacyShift(p, rot);
            page.piece = Placement{p, rot, loc % Board::WIDTH + s.x, FIELD_TOP - loc / Board::WIDTH - 1 + s.y};
        }
        if (hasComment) {
            long long len = poll(2);
            comment.clear();
            for (long long i = 0; i < len; i += 4) {
                long long c = poll(5);
                for (int k = 0; k < 4 && i + k < len; ++k, c /= 96) comment += COMMENT_TABLE[c % 96];
            }
        }
        page.comment = comment;

        // Lock the piece and clear lines to get the next page's starting field.
        if (lock) {
            if (page.piece)
                for (Cell c : page.piece->cells())
                    if (c.x >= 0 && c.x < Board::WIDTH && c.y >= 0 && c.y < FIELD_TOP)
                        field[(FIELD_TOP - 1 - c.y) * Board::WIDTH + c.x] = type;
            int w = FIELD_TOP - 1;
            std::array<int, FIELD_BLOCKS> cleared{};
            for (int r = Board::WIDTH * (FIELD_TOP + 1) - Board::WIDTH; r < FIELD_BLOCKS; ++r) cleared[r] = field[r];
            for (int y = FIELD_TOP - 1; y >= 0; --y) {  // row index from the top
                bool full = true;
                for (int x = 0; x < Board::WIDTH; ++x) full &= field[y * Board::WIDTH + x] != 0;
                if (full) continue;
                for (int x = 0; x < Board::WIDTH; ++x) cleared[w * Board::WIDTH + x] = field[y * Board::WIDTH + x];
                --w;
            }
            field = cleared;
        }
        prev = field;
        pages.push_back(std::move(page));
    }
    if (!ok) return std::nullopt;
    return pages;
}

// The piece placed on each page of a fumen, in order.
inline std::optional<std::vector<Placement>> decodePlacements(const std::string& text) {
    auto pages = decode(text);
    if (!pages) return std::nullopt;
    std::vector<Placement> out;
    for (const Page& p : *pages)
        if (p.piece) out.push_back(*p.piece);
    return out;
}

}  // namespace fumen

}  // namespace tetris
