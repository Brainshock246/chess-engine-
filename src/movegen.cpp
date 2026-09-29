#include "movegen.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace Aurora {

namespace {

constexpr int CAPTURE_BASE = 1 << 20;
constexpr int PROMO_BASE   = (1 << 21) + (1 << 20);

// `target` is the set of squares a pawn is allowed to land on. Captures are
// intersected with the *enemy* occupancy and pushes with the *empty*
// occupancy; without that intersection a pawn appears to "capture" onto an
// empty square diagonally, which silently inflates the move count.
//
// Captures and pushes are requested separately because the evasion stage needs
// captures only (a pawn push can never block a check), and because the
// CAPTURES stage must not contain pushes.
void gen_pawns(const Position& pos, MoveList& list, Color us, Bitboard target,
               bool wantCaps, bool wantPushes) {
    Bitboard pawns = pos.pieces(us, PAWN);
    if (!pawns) return;
    const U64 enemies  = pos.pieces(~us);
    const U64 empty    = ~pos.pieces();
    const int  sh      = push_shift(us);

    auto emit_promotions = [&](Square from, Square to, int base) {
        list.add(make_move(from, to, QUEEN),  base);
        list.add(make_move(from, to, ROOK),   base - 8);
        list.add(make_move(from, to, BISHOP), base - 24);
        list.add(make_move(from, to, KNIGHT), base - 40);
    };

    if (wantCaps) {
        const Bitboard capTarget = target & enemies;
        Bitboard b = pawns;
        while (b) {
            Square from = Bitboards::pop_lsb(b);
            Bitboard att = Bitboards::pawn_attacks_bb(us, Bitboards::square_bb(from)) & capTarget;
            while (att) {
                Square to = Bitboards::pop_lsb(att);
                bool promo = (us == WHITE) ? (rank_of(to) == RANK_8) : (rank_of(to) == RANK_1);
                if (promo) emit_promotions(from, to, PROMO_BASE);
                else       list.add(make_move(from, to), CAPTURE_BASE);
            }
        }
    }

    if (wantPushes) {
        const Bitboard pushTarget = target & empty;
        Bitboard b1 = Bitboards::shift_bb(Direction(sh), pawns) & pushTarget;
        while (b1) {
            Square to = Bitboards::pop_lsb(b1);
            Square from = Square(int(to) - sh);
            bool promo = (us == WHITE) ? (rank_of(to) == RANK_8) : (rank_of(to) == RANK_1);
            if (promo) emit_promotions(from, to, PROMO_BASE - 64);
            else       list.add(make_move(from, to), 0);
        }
        // Double pushes are only legal from the pawn's own starting rank. The
        // "intermediate square is empty" test alone is not enough: without the
        // rank test a pawn on a6 would be allowed to jump to a4, and a pawn on
        // a3 to reach a5. That single missing condition inflates perft(3) by
        // exactly the 32 nodes we were chasing.
        const Bitboard startRank = Bitboards::rank_bb(us == WHITE ? RANK_2 : RANK_7);
        Bitboard dblAll = Bitboards::shift_bb(Direction(2 * sh), pawns & startRank) & pushTarget
                        & Bitboards::shift_bb(Direction(sh), empty);
        while (dblAll) {
            Square to = Bitboards::pop_lsb(dblAll);
            list.add(make_move(Square(int(to) - 2 * sh), to), 0);
        }
    }
}

template<PieceType Pt>
void gen_piece(const Position& pos, MoveList& list, Color us, Bitboard target) {
    Bitboard bb = pos.pieces(us, Pt);
    if (!bb) return;
    U64 occ = pos.pieces();
    while (bb) {
        Square s = Bitboards::pop_lsb(bb);
        Bitboard att;
        if constexpr (Pt == KNIGHT)   att = Bitboards::knight_attacks_bb(s);
        else if constexpr (Pt == KING) att = Bitboards::king_attacks_bb(s);
        else if constexpr (Pt == BISHOP) att = Bitboards::bishop_attacks_bb(s, occ);
        else if constexpr (Pt == ROOK)   att = Bitboards::rook_attacks_bb(s, occ);
        // A queen is rook *and* bishop. Falling through to the rook case
        // silently deletes every diagonal move a queen can make.
        else att = Bitboards::queen_attacks_bb(s, occ);
        Bitboard b = att & target;
        while (b) list.add(make_move(s, Bitboards::pop_lsb(b)));
    }
}

void assign_capture_values(const Position& pos, MoveList& list) {
    for (int i = 0; i < list.size(); ++i) {
        Move m = list[i].move;
        if (!pos.capture_or_promotion(m)) { list[i].value = 0; continue; }
        PieceType to = type_of(pos.piece_on(to_sq(m)));
        if (to == NO_PT) to = PAWN;
        PieceType from = type_of(pos.piece_on(from_sq(m)));
        list[i].value = CAPTURE_BASE + PieceValue[to] * 16 - PieceValue[from];
    }
}

} // namespace

void generate(const Position& pos, MoveList& list, GenType type) {
    Color us = pos.side_to_move();
    Bitboard checkers = pos.checkers();
    list.clear();

    auto add_king_moves = [&](Bitboard b) {
        Square ksq = pos.king_square(us);
        while (b) list.add(make_move(ksq, pop_lsb(b)));
    };

    // En passant must be generated in the ordinary movegen too, not only when
    // evading a check. The capture square is empty, so it never shows up as a
    // normal pawn attack; omitting it silently drops every en passant move.
    auto add_ep_captures = [&]() {
        Square eps = pos.state()->epSquare;
        if (eps == SQ_NONE) return;
        // Where does our capturing pawn stand? For a white capture onto c6 that
        // is b5/d5, which is exactly what a *black* pawn on c6 attacks. So the
        // origin squares come from the opposite colour, and there must be no
        // extra guard: the intersection with our own pawns is the whole test.
        Bitboard ours = pos.pieces(us, PAWN)
                     & Bitboards::pawn_attacks_bb(~us, Bitboards::square_bb(eps));
        while (ours) list.add(make_ep(pop_lsb(ours), eps));
    };

    if (checkers) {
        // ---- evasions ----
        Square ksq = pos.king_square(us);
        // Generate every adjacent square we do not occupy and let legal() do
        // the real test. Filtering out the checker's ray here looks tempting but
        // is wrong: the ray *includes* the checker, and capturing the checker
        // with the king is the most common single evasion there is. The
        // occupancy that legal() uses already has the king lifted from its
        // square, so discovered attacks are handled correctly.
        Bitboard b = Bitboards::king_attacks_bb(ksq) & ~pos.pieces(us);
        add_king_moves(b);

        if (popcount(checkers) == 1) {
            Square csq = Square(lsb(checkers));
            Bitboard target = (Bitboards::between_bb(ksq, csq) | checkers) & ~pos.pieces(us);
            gen_piece<QUEEN>(pos, list, us, target);
            gen_piece<ROOK>(pos, list, us, target);
            gen_piece<BISHOP>(pos, list, us, target);
            gen_piece<KNIGHT>(pos, list, us, target);
            // Pawn moves can resolve a check two ways: capturing the checker,
            // and *pushing* onto a square between the king and the checker
            // (e.g. king e1, bishop h4, g2-g3). Generating only captures
            // silently loses the second kind, which is exactly the kind of
            // evasion kiwipete is built to expose.
            gen_pawns(pos, list, us, target, true, true);

            // En passant evasions are legal even when the ep square is not on
            // the checker's ray, so they need their own generation.
            add_ep_captures();
        }
        assign_capture_values(pos, list);
        if (type == LEGAL) {
            int n = 0;
            for (int i = 0; i < list.count; ++i)
                if (pos.legal(list[i].move)) list.moves[n++] = list.moves[i];
            list.count = n;
        }
        return;
    }

    // Not in check. Split the target by whether the move is a capture so that
    // the CAPTURES and QUIETS stages can be generated separately.
    const Bitboard enemy = pos.pieces(~us);
    const Bitboard emptySquares = ~pos.pieces();

    switch (type) {
    case CAPTURES:
        gen_piece<QUEEN>(pos, list, us, enemy);
        gen_piece<ROOK>(pos, list, us, enemy);
        gen_piece<BISHOP>(pos, list, us, enemy);
        gen_piece<KNIGHT>(pos, list, us, enemy);
        add_king_moves(Bitboards::king_attacks_bb(pos.king_square(us)) & enemy);
        gen_pawns(pos, list, us, enemy, true, false);
        add_ep_captures();
        break;

    case QUIETS:
        gen_piece<QUEEN>(pos, list, us, emptySquares);
        gen_piece<ROOK>(pos, list, us, emptySquares);
        gen_piece<BISHOP>(pos, list, us, emptySquares);
        gen_piece<KNIGHT>(pos, list, us, emptySquares);
        add_king_moves(Bitboards::king_attacks_bb(pos.king_square(us)) & emptySquares);
        gen_pawns(pos, list, us, emptySquares, false, true);
        break;

    default: {
        // Captures *and* quiets: the target is every square we do not occupy.
        // Targeting only empty squares here would silently drop every capture.
        const Bitboard all = ~pos.pieces(us);
        gen_piece<QUEEN>(pos, list, us, all);
        gen_piece<ROOK>(pos, list, us, all);
        gen_piece<BISHOP>(pos, list, us, all);
        gen_piece<KNIGHT>(pos, list, us, all);
        add_king_moves(Bitboards::king_attacks_bb(pos.king_square(us)) & all);
        gen_pawns(pos, list, us, all, true, true);
        add_ep_captures();
    }
        if (pos.can_castle_now(us, true))
            list.add(make_move(pos.king_square(us), relative_square(us, SQ_G1)));
        if (pos.can_castle_now(us, false))
            list.add(make_move(pos.king_square(us), relative_square(us, SQ_C1)));
        break;
    }

    assign_capture_values(pos, list);

    // `LEGAL` is the only mode that promises a fully legal list. Callers that
    // use EVASIONS / NON_EVASIONS filter as they search, which is much faster
    // than filtering the whole list up front.
    if (type == LEGAL) {
        int n = 0;
        for (int i = 0; i < list.count; ++i)
            if (pos.legal(list[i].move)) list.moves[n++] = list.moves[i];
        list.count = n;
    }
}

// ---------------------------------------------------------------------------
// MovePicker
// ---------------------------------------------------------------------------

MovePicker::MovePicker(const Position& pos, Move ttm, int depth, const int (&killers)[2], int thresh)
    : pos_(pos), ttMove_(ttm), depth_(depth), killer0_(killers[0]), killer1_(killers[1]), thresh_(thresh) {
    stage_ = 0;
    cur_ = end_ = moves_.begin();
}

void MovePicker::score_captures() {
    for (ExtMove* m = cur_; m < end_; ++m) m->value = mvv_lva(pos_, m->move);
}

void MovePicker::score_quiets() {
    for (ExtMove* m = cur_; m < end_; ++m) {
        Square to = to_sq(m->move);
        int v = (14 - std::min(Bitboards::file_distance(to, SQ_D4), 7))
              + (14 - std::min(Bitboards::rank_distance(to, RANK_4), 7));   // centralisation bonus
        if (type_of(pos_.piece_on(from_sq(m->move))) == PAWN) v += 8;
        m->value = v;
    }
}

ExtMove* MovePicker::pick_best() {
    ExtMove* best = cur_;
    for (ExtMove* p = cur_ + 1; p < end_; ++p)
        if (p->value > best->value) best = p;
    std::swap(*cur_, *best);
    return cur_++;
}

Move MovePicker::next_move(bool skipQuiets) {
    for (;;) {
        if (cur_ == end_ && stage_ < 4) {
            ++stage_;
            switch (stage_) {
            case 1:
                if (pos_.checkers()) generate(pos_, moves_, EVASIONS);
                else               generate(pos_, moves_, CAPTURES);
                score_captures();
                break;
            case 2:
                if (skipQuiets || pos_.checkers()) break;
                generate(pos_, moves_, QUIETS);
                score_quiets();
                break;
            case 3:
                break;
            case 4:
                return MOVE_NONE;
            }
            cur_ = moves_.begin();
            end_ = moves_.begin() + moves_.size();
            if (cur_ == end_) { if (stage_ >= 3) return MOVE_NONE; continue; }
        }
        if (cur_ == end_) return MOVE_NONE;

        if (ttMove_ != MOVE_NONE && stage_ == 0) {
            Move m = ttMove_;
            ttMove_ = MOVE_NONE;
            return m;
        }

        ExtMove* m = pick_best();
        if (m->move == MOVE_NONE) continue;
        if (m->move == killer0_ || m->move == killer1_) continue;
        if (pos_.legal(m->move)) return m->move;
    }
}

// ---------------------------------------------------------------------------
// Static Exchange Evaluation
// ---------------------------------------------------------------------------

namespace {

// Remove and return the least valuable attacker from `attackers`.
inline Square pop_least_valuable(Bitboard& attackers, const Position& pos) {
    Square s = SQ_NONE;
    int best = 1 << 30;
    Bitboard b = attackers;
    while (b) {
        Square t = Bitboards::pop_lsb(b);
        int v = PieceValue[pos.piece_on(t)];
        if (v < best) { best = v; s = t; }
    }
    if (s != SQ_NONE) attackers ^= Bitboards::square_bb(s);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Move <-> string
// ---------------------------------------------------------------------------

std::string to_string_move(Move m) {
    if (m == MOVE_NONE) return "(none)";
    if (m == MOVE_NULL) return "0000";
    Square from = from_sq(m), to = to_sq(m);
    std::string s;
    s += char('a' + file_of(from));
    s += char('1' + rank_of(from));
    s += char('a' + file_of(to));
    s += char('1' + rank_of(to));
    PieceType pt = promo_type(m);
    if (pt != NO_PT) s += "  nbrq"[pt];
    return s;
}

Move to_move(const Position& pos, const std::string& str) {
    if (str == "0000") return MOVE_NULL;
    if (str.size() < 4) return MOVE_NONE;
    Square from = make_square(File(str[0] - 'a'), Rank(str[1] - '1'));
    Square to   = make_square(File(str[2] - 'a'), Rank(str[3] - '1'));
    MoveList list;
    generate(pos, list, LEGAL);
    for (int i = 0; i < list.size(); ++i) {
        Move m = list[i].move;
        if (from_sq(m) == from && to_sq(m) == to) {
            if (str.size() >= 5) {
                const char* p = std::strchr(" nbrq", str[4]);
                if (!p) continue;
                PieceType pt = PieceType(p - " nbrq");
                if (promo_type(m) != pt) continue;
            }
            return m;
        }
    }
    return MOVE_NONE;
}

bool see_ge(const Position& pos, Move m, int threshold) {
    Square from = from_sq(m), to = to_sq(m);
    int capturedValue = is_ep(m) ? PieceValue[PAWN] : PieceValue[pos.piece_on(to)];
    if (capturedValue == 0) return threshold <= 0;  // quiet move

    int see = capturedValue;
    if (see - threshold < 0) return true;  // the capture alone already clears the bar

    U64 occ = (pos.pieces() ^ Bitboards::square_bb(from)) | Bitboards::square_bb(to);
    if (is_ep(m)) occ ^= Bitboards::square_bb(make_square(file_of(to), Rank(rank_of(from))));

    Color stm = pos.side_to_move();
    Square next = from;

    for (int i = 0; i < 30; ++i) {
        Color them = ~stm;
        Bitboard stmAttackers =
              (Bitboards::pawn_attacks_bb(them, Bitboards::square_bb(next)) & pos.pieces(stm, PAWN))
            | (Bitboards::knight_attacks_bb(next) & pos.pieces(stm, KNIGHT))
            | (Bitboards::bishop_attacks_bb(next, occ) & pos.pieces(stm, BISHOP, stm, QUEEN))
            | (Bitboards::rook_attacks_bb(next, occ) & pos.pieces(stm, ROOK, stm, QUEEN))
            | (Bitboards::king_attacks_bb(next) & pos.pieces(stm, KING));
        stmAttackers &= occ;
        if (!stmAttackers) return see >= threshold;   // no recapture available

        Square a = pop_least_valuable(stmAttackers, pos);
        if (a == SQ_NONE) return see >= threshold;

        see = PieceValue[pos.piece_on(a)] - see;
        occ ^= Bitboards::square_bb(a);
        next = a;
        stm = them;
        if (see < 0) {
            // Losing the exchange; only a further win can recover it.
            if (see < -400) return false;
        }
    }
    return see >= threshold;
}

} // namespace Aurora
