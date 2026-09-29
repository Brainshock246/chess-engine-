// Minimal correctness harness: attack tables, FEN round-trip, move generation.
#include "bitboard.h"
#include "position.h"
#include "movegen.h"
#include <cstdio>
#include <vector>
#include <chrono>

using namespace Aurora;

// Optional: pass a FEN on the command line to diagnose move generation there.
int main(int argc, char** argv) {
    if (argc > 3 && std::string(argv[1]) == "--after") {
        // --after "<fen>" <move>  ->  prints the FEN after playing <move>
        Bitboards::init();
        init_zobrist();
        std::vector<StateInfo> st(256);
        Position p;
        p.set(argv[2], st.data());
        MoveList ml;
        generate(p, ml, LEGAL);
        std::string want = argv[3];
        for (int i = 0; i < ml.size(); ++i) {
            if (to_string_move(ml[i].move) == want) {
                p.do_move(ml[i].move, st[p.game_ply() + 1]);
                std::fprintf(stderr, "AFTER %s\n", p.fen().c_str());
                return 0;
            }
        }
        std::fprintf(stderr, "AFTER <no such move %s>\n", want.c_str());
        return 1;
    }

    if (argc > 2 && std::string(argv[1]) == "--makeunmake") {
        // --makeunmake "<fen>"  ->  do/undo every legal move, reporting progress
        Bitboards::init();
        init_zobrist();
        std::vector<StateInfo> st(256);
        Position p;
        p.set(argv[2], st.data());
        MoveList ml;
        generate(p, ml, LEGAL);
        std::fprintf(stderr, "makeunmake: %d moves in %s\n", ml.size(), argv[2]);
        for (int i = 0; i < ml.size(); ++i) {
            std::fprintf(stderr, "  [%2d/%2d] %s ", i, ml.size(),
                         to_string_move(ml[i].move).c_str());
            std::fflush(stderr);
            std::string before = p.fen();
            p.do_move(ml[i].move, st[p.game_ply() + 1]);
            p.undo_move(ml[i].move);
            std::string after = p.fen();
            if (before != after)
                std::fprintf(stderr, " UNMAKE MISMATCH\n    before %s\n    after  %s\n",
                             before.c_str(), after.c_str());
            else
                std::fprintf(stderr, " ok\n");
        }
        std::fprintf(stderr, "makeunmake: completed\n");
        return 0;
    }

    if (argc > 1) {
        Bitboards::init();
        init_zobrist();
        std::vector<StateInfo> st2(256);
        Position p;
        p.set(argv[1], st2.data());
        std::fprintf(stderr, "FEN in : %s\n", argv[1]);
        std::fprintf(stderr, "FEN out: %s\n", p.fen().c_str());
        std::fprintf(stderr, "stm=%s  key=%016llx\n",
                     p.side_to_move() == WHITE ? "w" : "b",
                     (unsigned long long)p.key());
        std::fprintf(stderr, "checkers=%d  blockers(w)=%d blockers(b)=%d\n",
                     popcount(p.checkers()),
                     popcount(p.blockers_for_king(WHITE)),
                     popcount(p.blockers_for_king(BLACK)));
        Bitboard ck = p.checkers();
        while (ck) {
            Square s = pop_lsb(ck);
            // %c, not %s: " PNBRQK"[...] is a char, and handing a char to %s
            // makes printf read from that value as an address.
            std::fprintf(stderr, "  checker on %c%c (%c)\n", 'a' + int(file_of(s)),
                         '1' + int(rank_of(s)), " PNBRQK"[type_of(p.piece_on(s))]);
        }
        MoveList legal, pseudo;
        if (popcount(p.checkers()) == 1) {
            Square ksq = p.king_square(p.side_to_move());
            Square csq = Square(lsb(p.checkers()));
            U64 bt = Bitboards::between_bb(ksq, csq);
            U64 tg = (bt | p.checkers()) & ~p.pieces(p.side_to_move());
            auto dump = [&](const char* tag, U64 bb) {
                std::fprintf(stderr, "  %s:", tag);
                while (bb) { Square q = pop_lsb(bb);
                    std::fprintf(stderr, " %c%c", 'a' + int(file_of(q)), '1' + int(rank_of(q))); }
                std::fprintf(stderr, "\n");
            };
            dump("ksq/csq", Bitboards::square_bb(ksq) | Bitboards::square_bb(csq));
            dump("between", bt);
            dump("target ", tg);
        }
        generate(p, legal, LEGAL);
        generate(p, pseudo, NON_EVASIONS);
        std::fprintf(stderr, "legal=%d pseudo=%d\n", legal.size(), pseudo.size());
        for (int i = 0; i < pseudo.size(); ++i)
            if (!p.legal(pseudo[i].move))
                std::fprintf(stderr, "  rejected: %s\n", to_string_move(pseudo[i].move).c_str());
        for (int i = 0; i < legal.size(); ++i)
            std::fprintf(stderr, "  %s\n", to_string_move(legal[i].move).c_str());
        return 0;
    }
    Bitboards::init();
    std::fprintf(stderr, "[1] init ok\n");

    int bad = Bitboards::verify_attack_tables();
    std::fprintf(stderr, "[2] attack tables: %d mismatches\n", bad);
    if (bad) return 1;

    init_zobrist();
    std::vector<StateInfo> states(1024);
    Position pos;
    pos.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", states.data());
    std::fprintf(stderr, "[3] fen: %s\n", pos.fen().c_str());

    MoveList ml;
    generate(pos, ml, LEGAL);
    std::fprintf(stderr, "[4] startpos legal moves: %d (expect 20)\n", ml.size());
    for (int i = 0; i < ml.size(); ++i) std::fprintf(stderr, "     %s\n", to_string_move(ml[i].move).c_str());

    // Kiwipete: the standard hard case (castling, pins, en passant, promotions)
    Position p2;
    p2.set("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", states.data());
    MoveList m2;
    generate(p2, m2, LEGAL);
    std::fprintf(stderr, "[5] kiwipete legal moves: %d (expect 48)\n", m2.size());
    for (int i = 0; i < m2.size(); ++i) {
        Move m = m2[i].move;
        std::fprintf(stderr, "  %s  %s\n", to_string_move(m).c_str(),
                     p2.legal(m) ? "ok" : "ILLEGAL");
    }

    return 0;
}
