// runner.cpp — compiles together with a strategy file, runs its solve() on a
// piece sequence, replays the returned moves on a fresh game to verify every
// move, and writes a JSON game log for the visualiser.
//
//   c++ -std=c++17 -O2 -Iinclude runner/runner.cpp my_strategy.cpp -o run
//   ./run --seed 42 --length 200 --cap 4 --out game.json
//   ./run --queue "IOTLJSZ..." --no-hold --out game.json

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "tetris.h"

using namespace tetris;

namespace {

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string pieceString(Piece p) { return p == Piece::None ? "" : std::string(1, toChar(p)); }

[[noreturn]] void usage(const char* msg) {
    std::cerr << "error: " << msg << "\n"
              << "usage: run [--seed N | --queue PIECES] [--length N] [--cap H] [--no-hold] [--out FILE]\n";
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t seed = 0;
    bool haveSeed = false;
    std::string queueArg, outPath = "game.json";
    int length = 200;
    Rules rules;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage(("missing value for " + a).c_str());
            return argv[++i];
        };
        if (a == "--seed") seed = std::strtoull(value().c_str(), nullptr, 10), haveSeed = true;
        else if (a == "--queue") queueArg = value();
        else if (a == "--length") length = std::atoi(value().c_str());
        else if (a == "--cap") rules.maxHeight = std::atoi(value().c_str());
        else if (a == "--no-hold") rules.hold = false;
        else if (a == "--out") outPath = value();
        else usage(("unknown argument " + a).c_str());
    }

    std::vector<Piece> queue = queueArg.empty() ? sevenBag(seed, length) : parseQueue(queueArg);
    if (!queueArg.empty() && static_cast<int>(queue.size()) > length) queue.resize(length);
    if (queue.empty()) usage("empty piece sequence");

    Game start(queue, rules);
    std::vector<Move> moves;
    std::string crash;
    auto t0 = std::chrono::steady_clock::now();
    try {
        moves = solve(start);
    } catch (const std::exception& e) {
        crash = std::string("solve() threw: ") + e.what();
    } catch (...) {
        crash = "solve() threw an unknown exception";
    }
    double solveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout.flush();
    std::cerr.flush();

    // Replay on a fresh game, stopping at the first illegal move.
    Game game(queue, rules);
    std::ostringstream movesJson, pcsJson;
    std::string error, endReason;
    int errorMove = -1, segmentStart = 0;
    for (size_t k = 0; k < moves.size(); ++k) {
        const Move& m = moves[k];
        std::string why;
        if (!game.play(m, &why)) {
            error = why, errorMove = static_cast<int>(k);
            break;
        }
        const Placement& p = m.placement;
        if (k) movesJson << ",";
        movesJson << "{\"hold\":" << (m.hold ? "true" : "false") << ",\"piece\":\"" << toChar(p.piece)
                  << "\",\"rot\":" << p.rot << ",\"x\":" << p.x << ",\"y\":" << p.y
                  << ",\"cleared\":" << game.lastCleared() << ",\"pc\":" << (game.lastWasPC() ? "true" : "false")
                  << "}";
        if (game.lastWasPC()) {
            int pieces = game.placed() - segmentStart;
            if (game.pcCount() > 1) pcsJson << ",";
            pcsJson << "{\"move\":" << k << ",\"pieces\":" << pieces << ",\"lines\":" << pieces * 2 / 5
                    << ",\"position\":" << game.placed() + (game.held() != Piece::None) << ",\"hold\":" << jsonString(pieceString(game.held()))
                    << "}";
            segmentStart = game.placed();
        }
    }
    if (!crash.empty()) endReason = "crashed";
    else if (errorMove >= 0) endReason = "illegal move";
    else if (game.toppedOut()) endReason = "topped out";
    else if (game.over()) endReason = "sequence finished";
    else endReason = "strategy stopped";

    std::ofstream out(outPath);
    if (!out) usage(("cannot write " + outPath).c_str());
    out << "{\"queue\":" << jsonString(queueToString(queue)) << ",\"seed\":";
    if (haveSeed && queueArg.empty()) out << seed;
    else out << "null";
    out << ",\"rules\":{\"hold\":" << (rules.hold ? "true" : "false") << ",\"cap\":" << rules.maxHeight << "}"
        << ",\"moves\":[" << movesJson.str() << "]"
        << ",\"pcs\":[" << pcsJson.str() << "]"
        << ",\"returned\":" << moves.size() << ",\"placed\":" << game.placed() << ",\"pcCount\":" << game.pcCount()
        << ",\"finalHold\":" << jsonString(pieceString(game.held())) << ",\"solveMs\":" << solveMs
        << ",\"endReason\":" << jsonString(endReason) << ",\"error\":";
    if (errorMove >= 0) out << "{\"move\":" << errorMove << ",\"message\":" << jsonString(error) << "}";
    else if (!crash.empty()) out << "{\"move\":-1,\"message\":" << jsonString(crash) << "}";
    else out << "null";
    out << "}\n";

    std::cerr << "[runner] " << game.pcCount() << " PCs, " << game.placed() << "/" << queue.size()
              << " pieces placed, solve " << static_cast<long long>(solveMs) << " ms (" << endReason << ")\n";
    return 0;
}
