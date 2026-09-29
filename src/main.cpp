// Aurora - main entry point
//
// Sub-commands:
//   aurora                 - play a UCI game on stdin/stdout
//   aurora perft <n>       - perft from the initial position
//   aurora perft <fen> <n> - perft from an arbitrary position
//   aurora bench [depth]   - fixed-depth benchmark over a built-in suite
//   aurora dumpfen         - print the start position FEN (sanity check)

#include "position.h"
#include "movegen.h"
#include "search.h"
#include "uci.h"
#include "bitboard.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <string>

using namespace Aurora;
using namespace std::chrono;

namespace {

using States = std::vector<StateInfo>;

// Cross-check for the fast legal() filter. The fast path reasons about pins and
// blockers; this verifies the result the blunt way - actually make the move and
// ask whether the mover's own king is attacked. Any divergence is a bug in
// legal(), and perft is the cheapest place to find it.
static bool slow_legal(const Position& pos, Move m) {
    if (m == MOVE_NONE) return false;
    Square ksq = pos.king_square(pos.side_to_move());
    Position copy = pos;
    StateInfo si;
    copy.do_move(m, si);
    bool ok = !copy.attacked_by(ksq, copy.pieces(), ~pos.side_to_move());
    copy.undo_move(m);
    return ok;
}

uint64_t perft(Position& pos, States& states, int depth, bool check = false) {
    if (depth == 0) return 1;
    MoveList moves;
    generate(pos, moves, LEGAL);
    if (depth == 1) return uint64_t(moves.size());
    uint64_t nodes = 0;
    for (int i = 0; i < moves.size(); ++i) {
        if (check) {
            MoveList pseudo;
            generate(pos, pseudo, NON_EVASIONS);
            bool inPseudo = false;
            for (int j = 0; j < pseudo.size(); ++j)
                if (pseudo[j].move == moves[i].move) { inPseudo = true; break; }
            if (!inPseudo || !slow_legal(pos, moves[i].move)) {
                std::cout << "PERFT BUG at " << pos.fen() << " move "
                          << to_string_move(moves[i].move) << std::endl;
                return 0;
            }
        }
        pos.do_move(moves[i].move, states[pos.game_ply() + 1]);
        nodes += perft(pos, states, depth - 1, check);
        pos.undo_move(moves[i].move);
    }
    return nodes;
}

void divide(Position& pos, States& states, int depth) {
    MoveList moves;
    generate(pos, moves, LEGAL);
    uint64_t total = 0;
    for (int i = 0; i < moves.size(); ++i) {
        pos.do_move(moves[i].move, states[pos.game_ply() + 1]);
        uint64_t n = (depth <= 1) ? 1 : perft(pos, states, depth - 1);
        pos.undo_move(moves[i].move);
        std::cout << to_string_move(moves[i].move) << ": " << n << std::endl;
        total += n;
    }
    std::cout << "\nNodes searched: " << total << std::endl;
}

// Built-in benchmark suite: 30 varied positions (openings, middlegames, endgames)
const char* BenchFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "2rq1rk1/pp1bppbp/3p1np1/8/3NP3/1BN1BP2/PPPQ2PP/2KR3R w - - 0 1",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
    "r1b1r1k1/1pp1n1pp/1p1p4/4p2Q/4Pp2/1BNP4/PPP2PPP/3R1RK1 w - - 2 15",
    "3r1rk1/p1q2ppp/1pn1pn2/2bp4/8/PP1BPN2/1PQ2PPP/R3R1K1 w - - 0 16",
    "2r3k1/pp1bppbp/3p1np1/q7/3P4/2P2N2/PP1B1PPP/R2Q1RK1 b - - 0 1",
    "r1bq2rk/pp3pbp/2p1p1pQ/7P/3P4/2PB1N2/PP3PPR/2KR4 w - - 0 1",
    "8/8/3P1k2/p1P5/8/8/8/4K3 w - - 0 1",
    "8/2k5/8/1P6/8/8/5K2/8 w - - 0 1",
    "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",
    "8/8/4k3/8/2p5/8/B7/4K3 w - - 0 1",
    "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1",
    "8/8/1k6/2b5/2pP4/8/5K2/8 b - - 0 1",
    "5k2/8/8/8/8/8/8/4K2R w K - 0 1",
    "3k4/8/8/8/8/8/8/R3K3 w Q - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
    "r1bq1rk1/pp2ppbp/2np1np1/8/2BNP3/2N1B3/PPP2PPP/R2QK2R w KQ - 0 8",
    "2kr3r/pp1q1ppp/5n2/1Nb5/2Pp1B2/7Q/PP3PPP/R3K2R w KQ - 0 1",
    "r1b1k2r/ppppqppp/2n2n2/2b5/2B1P3/2N2N2/PPPP1PPP/R1BQK2R b KQkq - 0 1",
    "2r3k1/pp1q1pp1/2n1p1p1/2pPp3/2P1P3/2N1BP2/PP2Q1PP/R4RK w - - 0 1",
    "8/8/4k3/8/8/4K3/4P3/8 w - - 0 1",
    "1k6/1b6/8/8/7Q/8/8/4K3 w - - 0 1",
    "3k4/3q4/8/8/8/3Q4/3K4/8 w - - 0 1",
    "8/1k6/8/2Pp4/8/8/8/4KQ2 w - d6 0 1"
};
constexpr int BENCH_COUNT = sizeof(BenchFens) / sizeof(BenchFens[0]);

int run_bench(int depth) {
    if (depth <= 0) depth = 8;
    States states(MAX_PLY + 32);
    Search search;
    search.set_hash_size(16);

    uint64_t totalNodes = 0;
    int64_t totalTime = 0;
    int bestDepth = 0;

    for (int i = 0; i < BENCH_COUNT; ++i) {
        Position pos;
        pos.set(BenchFens[i], states.data());
        LimitsType limits;
        limits.depth = depth;
        limits.startTime = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

        std::ostringstream sink;
        int64_t t0 = limits.startTime;
        search.set_node_limit(0);
        search.think(pos, states.data(), limits, sink);
        int64_t el = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() - t0;
        totalNodes += search.nodes();
        totalTime += el;
        if (search.completed_depth() > bestDepth) bestDepth = search.completed_depth();
        std::cout << "." << std::flush;
    }
    std::cout << "\n";
    double nps = totalTime > 0 ? double(totalNodes) / double(totalTime) * 1000.0 : 0.0;
    std::cout << "\n===========================\n"
              << "Total time (ms) : " << totalTime << "\n"
              << "Nodes searched  : " << totalNodes << "\n"
              << "Nodes/second    : " << uint64_t(nps) << "\n"
              << "Best depth      : " << bestDepth << "\n"
              << "===========================" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Bitboards::init();
    Aurora::init_zobrist();

    if (argc < 2) {
        UCI uci(*(new Search()));
        uci.loop(argc, argv);
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "after" && argc >= 4) {
        // aurora after "<fen>" <move>  ->  prints the FEN after playing <move>
        // Used by tools/narrow.py to walk a tree. Keeping this in the engine
        // rather than in a side tool removes the risk of a stale helper binary
        // disagreeing with the engine it is supposed to describe.
        States states(64);
        Position pos;
        pos.set(argv[2], states.data());
        MoveList ml;
        generate(pos, ml, LEGAL);
        std::string want = argv[3];
        for (int i = 0; i < ml.size(); ++i) {
            if (to_string_move(ml[i].move) == want) {
                pos.do_move(ml[i].move, states[pos.game_ply() + 1]);
                std::cout << "AFTER " << pos.fen() << std::endl;
                return 0;
            }
        }
        std::cout << "AFTER <no such move " << want << ">" << std::endl;
        return 1;
    }

    if (cmd == "setw" && argc >= 3) {
        // setw <index> <value>   /   setw <name> <value>
        // Lets a driver bisect the evaluation by zeroing one weight at a time
        // in a single process, instead of rebuilding for every hypothesis.
        const int n = Eval::weight_count();
        int idx = -1;
        if (std::isdigit((unsigned char)argv[2][0])) {
            idx = std::atoi(argv[2]);
        } else {
            for (int i = 0; i < n; ++i)
                if (argv[2] == Eval::weight_name(i)) { idx = i; break; }
        }
        if (idx < 0 || idx >= n) {
            std::cout << "no such weight: " << argv[2] << "\n";
            return 1;
        }
        Eval::set_weight(idx, std::atoi(argv[3]));
        std::cout << "set " << Eval::weight_name(idx) << " = " << std::atoi(argv[3]) << "\n";
        return 0;
    }

    if (cmd == "weights") {
        const int n = Eval::weight_count();
        for (int i = 0; i < n; ++i)
            std::cout << i << "\t" << Eval::weight_name(i) << "\t" << Eval::get_weight(i) << "\n";
        return 0;
    }

    if (cmd == "evaldump" && argc >= 3) {
        // Per-side accumulator dump. Use this, not the end score, to find an
        // asymmetry: the final score hides which side is being mis-scored.
        Eval::trace_enabled = true;
        States states(64);
        Position pos;
        pos.set(argv[2], states.data());
        std::cout << "fen: " << pos.fen() << "\n";
        const Value v = Eval::evaluate(pos);
        Eval::trace_enabled = false;
        std::cout << "score: " << int(v) << "\n";
        return 0;
    }

    if (cmd == "terminal" && argc >= 2) {
        // aurora terminal [moves...]   ->   ok | mate | stalemate
        //
        // Replays the move list from the start position and reports whether the
        // side to move has any legal move. tools/match.py needs this to tell
        // checkmate from stalemate: both engines signal "no legal move", and the
        // two are opposite results. The match runner cannot work it out from the
        // protocol alone, so the engine answers it directly rather than the
        // runner re-implementing position tracking - which is exactly the
        // duplication that has bitten this project before.
        States states(1024);
        Position pos;
        pos.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", states.data());
        for (int i = 2; i < argc; ++i) {
            MoveList ml;
            generate(pos, ml, LEGAL);
            const std::string want = argv[i];
            bool played = false;
            for (int k = 0; k < ml.size(); ++k) {
                if (to_string_move(ml[k].move) == want) {
                    pos.do_move(ml[k].move, states[pos.game_ply() + 1]);
                    played = true;
                    break;
                }
            }
            if (!played) {
                std::cout << "illegal " << want << "\n";
                return 1;
            }
        }
        MoveList legal;
        generate(pos, legal, LEGAL);
        if (legal.size() == 0)
            std::cout << (pos.checkers() != 0 ? "mate" : "stalemate") << "\n";
        else
            std::cout << "ok\n";
        return 0;
    }

    if (cmd == "eval" && argc >= 3) {
        // Static evaluation of a position, from the side to move's point of view.
        States states(64);
        Position pos;
        pos.set(argv[2], states.data());
        std::cout << int(Eval::evaluate(pos)) << "\n";
        return 0;
    }

    if (cmd == "info" && argc >= 3) {
        // Diagnostic dump: FEN round-trip, key, checkers, pin blockers, and the
        // legality verdict for every pseudo-legal move.
        States states(64);
        Position pos;
        pos.set(argv[2], states.data());
        std::cout << "fen   : " << pos.fen() << "\n";
        std::cout << "key   : " << std::hex << pos.key() << std::dec << "\n";
        std::cout << "stm   : " << (pos.side_to_move() == WHITE ? "w" : "b") << "\n";
        std::cout << "checkers: " << popcount(pos.checkers()) << "\n";
        Bitboard cb = pos.checkers();
        while (cb) { Square s = pop_lsb(cb);
            std::cout << "   checker " << char('a' + file_of(s)) << char('1' + rank_of(s)) << "\n"; }
        Bitboard w = pos.blockers_for_king(WHITE), b = pos.blockers_for_king(BLACK);
        std::cout << "blockers w/b: " << popcount(w) << "/" << popcount(b) << "\n";
        auto dump = [&](const char* tag, Bitboard bb) {
            std::cout << "   " << tag << ":";
            while (bb) { Square s = pop_lsb(bb);
                std::cout << " " << char('a' + file_of(s)) << char('1' + rank_of(s)); }
            std::cout << "\n";
        };
        dump("white", w);
        dump("black", b);
        MoveList pseudo, legal;
        generate(pos, pseudo, NON_EVASIONS);
        generate(pos, legal, LEGAL);
        std::cout << "pseudo: " << pseudo.size() << "  legal: " << legal.size() << "\n";
        for (int i = 0; i < pseudo.size(); ++i)
            if (!pos.legal(pseudo[i].move)) {
                Move m = pseudo[i].move;
                std::cout << "  REJECTED " << to_string_move(m)
                          << "  from=" << char('a' + file_of(from_sq(m))) << char('1' + rank_of(from_sq(m)))
                          << " to=" << char('a' + file_of(to_sq(m))) << char('1' + rank_of(to_sq(m)))
                          << " blocker_hit=" << ((pos.blockers_for_king(pos.side_to_move()) & Bitboards::square_bb(from_sq(m))) ? "yes" : "no")
                          << " aligned=" << (Bitboards::aligned(from_sq(m), to_sq(m), pos.king_square(pos.side_to_move())) ? "yes" : "no")
                          << "\n";
            }
        return 0;
    }

    if (cmd == "moves" && argc >= 3) {
        // aurora moves "<fen>"  ->  the legal move list, one per line
        States states(64);
        Position pos;
        pos.set(argv[2], states.data());
        MoveList ml;
        generate(pos, ml, LEGAL);
        for (int i = 0; i < ml.size(); ++i)
            std::cout << to_string_move(ml[i].move) << "\n";
        return 0;
    }

    if (cmd == "perft" || cmd == "perftcheck") {
        const bool check = (cmd == "perftcheck");
        States states(1024);
        Position pos;
        std::string fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        int depth = 6;
        if (argc >= 4) { fen = argv[2]; depth = std::stoi(argv[3]); }
        else if (argc == 3) { depth = std::stoi(argv[2]); }
        pos.set(fen, states.data());
        if (depth == 0) std::cout << "1" << std::endl;
        else if (check) std::cout << "Nodes searched: " << perft(pos, states, depth, true) << std::endl;
        else divide(pos, states, depth);
        return 0;
    }

    if (cmd == "bench") {
        int depth = (argc > 2) ? std::stoi(argv[2]) : 8;
        return run_bench(depth);
    }

    if (cmd == "weights") {
        int n = Eval::weight_count();
        for (int i = 0; i < n; ++i)
            std::cout << Eval::weight_name(i) << " " << Eval::get_weight(i) << "\n";
        return 0;
    }

    if (cmd == "dumpfen") {
        std::vector<StateInfo> states(16);
        Position pos;
        pos.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", states.data());
        std::cout << pos.fen() << std::endl;
        return 0;
    }

    // default: UCI loop
    Search search;
    UCI uci(search);
    uci.loop(argc, argv);
    return 0;
}
