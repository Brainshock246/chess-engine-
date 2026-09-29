#include "position.h"
#include <cstdio>
#include <cstring>
#include <sstream>

namespace Aurora {

U64 ZobristPsq[PIECE_NB][SQUARE_NB];
U64 ZobristCastling[CASTLING_RIGHT_NB];
U64 ZobristEnPassant[FILE_NB];
U64 ZobristSide;

namespace {

struct Rng {
    U64 s;
    explicit Rng(U64 seed) : s(seed) {}
    U64 next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ULL; }
};

const char* CharToPiece = " PNBRQK  pnbrqk ";

} // namespace

void init_zobrist() {
    static bool done = false;
    if (done) return;
    done = true;
    Rng rng(0x1234567890ABCDEFULL);
    for (int p = 0; p < PIECE_NB; ++p)
        for (int s = 0; s < SQUARE_NB; ++s) ZobristPsq[p][s] = rng.next();
    for (int c = 0; c < CASTLING_RIGHT_NB; ++c) ZobristCastling[c] = rng.next();
    for (int f = 0; f < FILE_NB; ++f) ZobristEnPassant[f] = rng.next();
    ZobristSide = rng.next();
}

// ---------------------------------------------------------------------------
// board maintenance
// ---------------------------------------------------------------------------

void Position::put_piece(Piece pc, Square s) {
    board_[s] = pc;
    byTypeBB_[ALL_PIECES] |= Bitboards::square_bb(s);
    byTypeBB_[type_of(pc)] |= Bitboards::square_bb(s);
    byColorBB_[color_of(pc)] |= Bitboards::square_bb(s);
    piece_count_[pc]++;
    count_[s]++;
}

void Position::remove_piece(Square s) {
    Piece pc = board_[s];
    byTypeBB_[ALL_PIECES] ^= Bitboards::square_bb(s);
    byTypeBB_[type_of(pc)] ^= Bitboards::square_bb(s);
    byColorBB_[color_of(pc)] ^= Bitboards::square_bb(s);
    board_[s] = NO_PIECE;
    piece_count_[pc]--;
    count_[s]--;
}

void Position::move_piece(Square from, Square to) {
    Piece pc = board_[from];
    U64 fromTo = Bitboards::square_bb(from) | Bitboards::square_bb(to);
    byTypeBB_[ALL_PIECES] ^= fromTo;
    byTypeBB_[type_of(pc)] ^= fromTo;
    byColorBB_[color_of(pc)] ^= fromTo;
    board_[from] = NO_PIECE;
    board_[to] = pc;
    count_[from]--;
    count_[to]++;
}

int64_t Position::simple_material(Color c) const {
    int64_t v = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Piece pc = board_[s];
        if (pc != NO_PIECE && color_of(pc) == c) v += PieceValue[pc];
    }
    return v;
}

// ---------------------------------------------------------------------------
// FEN
// ---------------------------------------------------------------------------

void Position::set(const std::string& fen, StateInfo* si) {
    std::memset((void*)this, 0, sizeof(Position));
    st_ = si;
    si->previous = nullptr;
    si->capturedPiece = NO_PIECE;
    si->pliesFromNull = 0;
    si->rule50 = 0;
    si->castlingRights = NO_CASTLING;
    si->epSquare = SQ_NONE;
    gamePly_ = 0;

    std::istringstream ss(fen);
    std::string placement, stm, castling, ep, halfmoves, fullmoves;
    ss >> placement >> stm >> castling >> ep >> halfmoves >> fullmoves;

    // Refuse a position with no king for `us` before anything can ask for it.
    // king_square() is lsb(pieces(c, KING)), and lsb(0) is 64 - one past the
    // board - so every attacker lookup on it reads out of bounds and the
    // failure surfaces far away from the bad input, as a crash in perft or in
    // the search. Better to reject it here, where the mistake actually is.
    //
    // The side to move is parsed from the second field, which may be absent on a
    // bare placement string, so this check happens after the board loop below.

    int rank = 7, file = 0;
    for (char c : placement) {
        if (c == '/') { --rank; file = 0; continue; }
        if (c >= '1' && c <= '8') { file += c - '0'; continue; }
        const char* p = std::strchr(CharToPiece, c);
        put_piece(Piece(p - CharToPiece), make_square(File(file), Rank(rank)));
        ++file;
    }

    // A position with no king is not a position. Everything downstream assumes
    // one exists: king_square() is lsb(pieces(c, KING)) and lsb(0) is 64, one
    // past the board, so a single missing king turns every attacker lookup into
    // an out-of-bounds read - a crash that surfaces nowhere near the bad input.
    // Park the missing king on a free square of its own back rank so the
    // position stays in bounds, and say so.
    for (Color c = WHITE; c <= BLACK; ++c) {
        if (count(c, KING) == 1) continue;
        std::fprintf(stderr, "FEN has no %s king: %s\n",
                     c == WHITE ? "white" : "black", fen.c_str());
        for (Square s = SQ_E1; s <= SQ_A1 + 7; ++s) {
            const Square sq = relative_square(c, s);
            if (board_[sq] != NO_PIECE) continue;
            put_piece(make_piece(c, KING), sq);
            break;
        }
    }

    sideToMove_ = (stm == "b") ? BLACK : WHITE;
    if (!stm.empty() && !halfmoves.empty()) si->rule50 = std::stoi(halfmoves);
    if (!ep.empty() && ep.size() == 2 && ep[0] != '-')
        si->epSquare = make_square(File(ep[0] - 'a'), Rank(ep[1] - '1'));

    // Derive castling rights from king/rook placement (standard chess only).
    for (Color c = WHITE; c <= BLACK; ++c) {
        if (piece_on(relative_square(c, SQ_E1)) == make_piece(c, KING) && count(c, KING) == 1) {
            if (piece_on(relative_square(c, SQ_H1)) == make_piece(c, ROOK))
                si->castlingRights |= (c == WHITE) ? WHITE_OO : BLACK_OO;
            if (piece_on(relative_square(c, SQ_A1)) == make_piece(c, ROOK))
                si->castlingRights |= (c == WHITE) ? WHITE_OOO : BLACK_OOO;
        }
    }
    // Respect an explicit rights field that omits rights (e.g. "-")
    if (!castling.empty() && castling == "-") si->castlingRights = NO_CASTLING;

    for (int c = 0; c < COLOR_NB; ++c) si->nonPawnMaterial[c] = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Piece pc = board_[s];
        if (pc != NO_PIECE && type_of(pc) != PAWN && type_of(pc) != KING)
            si->nonPawnMaterial[color_of(pc)] += PieceValue[pc];
    }

    si->key = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        if (board_[s]) si->key ^= ZobristPsq[board_[s]][s];
    si->key ^= ZobristCastling[si->castlingRights];
    if (si->epSquare != SQ_NONE) si->key ^= ZobristEnPassant[file_of(si->epSquare)];
    if (sideToMove_ == BLACK) si->key ^= ZobristSide;

    set_check_info(si);
}

std::string Position::fen() const {
    std::ostringstream ss;
    for (int rank = 7; rank >= 0; --rank) {
        int run = 0;
        for (int file = 0; file < 8; ++file) {
            Piece pc = board_[make_square(File(file), Rank(rank))];
            if (pc == NO_PIECE) { ++run; continue; }
            if (run) { ss << run; run = 0; }
            ss << CharToPiece[pc];
        }
        if (run) ss << run;
        if (rank) ss << '/';
    }
    ss << (sideToMove_ == WHITE ? " w " : " b ");
    std::string c;
    if (can_castle(WHITE_OO))  c += 'K';
    if (can_castle(WHITE_OOO)) c += 'Q';
    if (can_castle(BLACK_OO))  c += 'k';
    if (can_castle(BLACK_OOO)) c += 'q';
    ss << (c.empty() ? "-" : c) << ' ';
    if (st_->epSquare == SQ_NONE) ss << '-';
    else ss << char('a' + file_of(st_->epSquare)) << char('1' + rank_of(st_->epSquare));
    ss << ' ' << st_->rule50 << " 1";
    return ss.str();
}

// ---------------------------------------------------------------------------
// check / pin detection
// ---------------------------------------------------------------------------

void Position::set_check_info(StateInfo* si) const {
    si->checkers = 0;
    si->blockersForKing[WHITE] = si->blockersForKing[BLACK] = 0;
    for (int i = 0; i < PIECE_TYPE_NB; ++i) si->checkSquares[i] = 0;

    U64 occ = pieces();
    for (Color c = WHITE; c <= BLACK; ++c) {
        Square ksq = king_square(c);
        Color them = ~c;
        Bitboard bishopCheckers = pieces(them, BISHOP, them, QUEEN) & Bitboards::bishop_attacks_bb(ksq, occ);
        Bitboard rookCheckers   = pieces(them, ROOK,   them, QUEEN) & Bitboards::rook_attacks_bb(ksq, occ);
        // Pawn checks: the squares an enemy pawn must *stand on* to attack ksq
        // are exactly the squares one of our own pawns on ksq would attack, so
        // the defender's colour is the right one here. (Verified by hand: a
        // black pawn on c5 checks a white king on b4, and
        // pawn_attacks_bb(WHITE, b4) = {a5, c5}.)
        Bitboard checks = bishopCheckers | rookCheckers
                        | (pieces(them, KNIGHT) & Bitboards::knight_attacks_bb(ksq))
                        | (pieces(them, KING) & Bitboards::king_attacks_bb(ksq))
                        | (pieces(them, PAWN) & Bitboards::pawn_attacks_bb(c, Bitboards::square_bb(ksq)));

        if (c == sideToMove_) si->checkers = checks;

        // Pinners: enemy sliders x-raying our king, blocked by at most one of our pieces.
        Bitboard snipers = (Bitboards::bishop_attacks_bb(ksq, 0) & (pieces(them, BISHOP) | pieces(them, QUEEN)))
                         | (Bitboards::rook_attacks_bb(ksq, 0) & (pieces(them, ROOK) | pieces(them, QUEEN)));
        while (snipers) {
            Square s = Bitboards::pop_lsb(snipers);
            U64 b = Bitboards::between_bb(ksq, s) & occ;
            if (b && !(U64(b & (b - 1))))   // exactly one piece in between -> pin
                si->blockersForKing[c] |= b;
        }
    }

    // Squares on which placing a piece of each type would check our king
    {
        Square ksq = king_square(sideToMove_);
        si->checkSquares[PAWN]   = Bitboards::pawn_attacks_bb(~sideToMove_, Bitboards::square_bb(ksq));
        si->checkSquares[KNIGHT] = Bitboards::knight_attacks_bb(ksq);
        si->checkSquares[BISHOP] = Bitboards::bishop_attacks_bb(ksq, occ) & ~Bitboards::square_bb(ksq);
        si->checkSquares[ROOK]   = Bitboards::rook_attacks_bb(ksq, occ) & ~Bitboards::square_bb(ksq);
        si->checkSquares[QUEEN]  = si->checkSquares[BISHOP] | si->checkSquares[ROOK];
        si->checkSquares[KING]   = Bitboards::king_attacks_bb(ksq);
    }
}

bool Position::attacked_by(Square s, U64 occ, Color by) const {
    const Color them = ~by;
    // Every term is filtered through `occ`, not just the sliders. `occ` is the
    // board *after* the move, so it already has the king lifted off its origin
    // square and the en passant victim removed. Reading the pawn/knight/king
    // sets from the live position instead makes an en passant capture look
    // illegal whenever the captured pawn was itself a defender of our king -
    // and it also fails to notice a discovered attack opened by the capture.
    const U64 theirs = occ & byColorBB_[by];
    return (Bitboards::pawn_attacks_bb(them, Bitboards::square_bb(s)) & theirs & byTypeBB_[PAWN])
        || (Bitboards::knight_attacks_bb(s) & theirs & byTypeBB_[KNIGHT])
        || (Bitboards::king_attacks_bb(s) & occ & byTypeBB_[KING])
        || (Bitboards::bishop_attacks_bb(s, occ) & theirs & (byTypeBB_[BISHOP] | byTypeBB_[QUEEN]))
        || (Bitboards::rook_attacks_bb(s, occ) & theirs & (byTypeBB_[ROOK] | byTypeBB_[QUEEN]));
}

// ---------------------------------------------------------------------------
// castling
// ---------------------------------------------------------------------------

bool Position::can_castle_now(Color c, bool kingSide) const {
    if (c != sideToMove_) return false;
    CastlingRights cr = (c == WHITE) ? (kingSide ? WHITE_OO : WHITE_OOO)
                                     : (kingSide ? BLACK_OO : BLACK_OOO);
    if (!can_castle(cr)) return false;
    if (checkers()) return false;

    Square kfrom = relative_square(c, SQ_E1);
    Square kto   = relative_square(c, kingSide ? SQ_G1 : SQ_C1);
    U64 occ = pieces();

    // Every square that must be empty: the two the king crosses, plus the one
    // the rook crosses. For kingside those coincide (f1), but for queenside
    // the rook passes b1 while the king never does - and forgetting b1 lets
    // the engine castle straight through its own knight.
    const Square rookFrom = castling_rook_square(cr);
    U64 mustBeEmpty = Bitboards::square_bb(kfrom);
    if (kingSide) {
        mustBeEmpty |= Bitboards::square_bb(relative_square(c, SQ_F1))
                     |  Bitboards::square_bb(relative_square(c, SQ_G1));
    } else {
        mustBeEmpty |= Bitboards::square_bb(relative_square(c, SQ_D1))
                     |  Bitboards::square_bb(relative_square(c, SQ_C1))
                     |  Bitboards::square_bb(relative_square(c, SQ_B1));
    }
    mustBeEmpty &= ~Bitboards::square_bb(kfrom);
    if (occ & mustBeEmpty) return false;

    // The rook must actually be home, or the right is stale.
    if (piece_on(rookFrom) != make_piece(c, ROOK)) return false;

    // The king may not pass through or land on an attacked square.
    U64 afterKing = (occ & ~Bitboards::square_bb(kfrom)) | Bitboards::square_bb(kto);
    Color them = ~c;
    if (attacked_by(kfrom, occ, them)) return false;
    if (kingSide) {
        if (attacked_by(relative_square(c, SQ_F1), afterKing, them)) return false;
    } else {
        if (attacked_by(relative_square(c, SQ_D1), afterKing, them)) return false;
    }
    if (attacked_by(kto, afterKing, them)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// make / unmake
// ---------------------------------------------------------------------------

template<bool Do>
void Position::do_move(Move m, StateInfo& newSt) {
    newSt.previous     = st_;
    newSt.capturedPiece = NO_PIECE;
    newSt.pliesFromNull = st_->pliesFromNull + 1;

    Color us = sideToMove_, them = ~us;
    Square from = from_sq(m), to = to_sq(m);
    Piece pc = board_[from];
    PieceType pt = type_of(pc);
    bool ep = is_ep(m);
    Piece captured = ep ? make_piece(them, PAWN) : board_[to];
    Square capSq = ep ? make_square(file_of(to), rank_of(from)) : to;
    bool castling = is_castling(m);

    newSt.rule50 = (captured != NO_PIECE || pt == PAWN) ? 0 : st_->rule50 + 1;
    newSt.capturedPiece = captured;

    U64 k = st_->key ^ ZobristSide;
    if (st_->epSquare != SQ_NONE) k ^= ZobristEnPassant[file_of(st_->epSquare)];
    k ^= ZobristCastling[st_->castlingRights];

    if (captured != NO_PIECE) {
        k ^= ZobristPsq[captured][capSq];
        if (Do) remove_piece(capSq);
    }

    Piece placed = pc;
    if (pt == PAWN && (rank_of(to) == RANK_1 || rank_of(to) == RANK_8))
        placed = make_piece(us, promo_type(m));

    if (Do) {
        move_piece(from, to);
        if (placed != pc) { remove_piece(to); put_piece(placed, to); }
    }
    k ^= ZobristPsq[pc][from] ^ ZobristPsq[placed][to];

    if (castling) {
        bool kingSide = file_of(to) > file_of(from);
        Square rfrom = relative_square(us, kingSide ? SQ_H1 : SQ_A1);
        Square rto   = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
        if (Do) move_piece(rfrom, rto);
        k ^= ZobristPsq[make_piece(us, ROOK)][rfrom] ^ ZobristPsq[make_piece(us, ROOK)][rto];
    }

    // Castling rights: recomputed from the board.  This is both robust and fast
    // because the right can only survive if the king and rook are still home.
    //
    // The result must be written into `newSt`, never into `st_`: the parent
    // state is still live and undo_move() restores *it*, so overwriting the
    // parent's rights loses them permanently.
    {
        int newRights = NO_CASTLING;
        for (Color c = WHITE; c <= BLACK; ++c) {
            if (piece_on(relative_square(c, SQ_E1)) != make_piece(c, KING) || count(c, KING) != 1) continue;
            if (piece_on(relative_square(c, SQ_H1)) == make_piece(c, ROOK) && (st_->castlingRights & (c == WHITE ? WHITE_OO : BLACK_OO)))
                newRights |= (c == WHITE) ? WHITE_OO : BLACK_OO;
            if (piece_on(relative_square(c, SQ_A1)) == make_piece(c, ROOK) && (st_->castlingRights & (c == WHITE ? WHITE_OOO : BLACK_OOO)))
                newRights |= (c == WHITE) ? WHITE_OOO : BLACK_OOO;
        }
        newSt.castlingRights = newRights;
        k ^= ZobristCastling[newRights];
    }

    // En passant square.
    //
    // Recorded after EVERY double push, unconditionally. Two things were wrong
    // before: the "is anyone able to capture" test asked whether *our* pawns
    // stood on the squares that *their* pawns attack (i.e. rank 7/2 instead of
    // rank 5/4), so it almost never fired and the ep square was effectively
    // never set - silently deleting every en passant capture. And gating it on
    // a capturer existing anyway makes the position differ from the reference
    // engines' FEN, which then makes perft comparisons and tree walks compare
    // two different positions.
    newSt.epSquare = SQ_NONE;
    if (pt == PAWN && ((from ^ to) == 16)) {
        newSt.epSquare = make_square(file_of(from),
                                     Rank((int(rank_of(from)) + int(rank_of(to))) / 2));
        k ^= ZobristEnPassant[file_of(newSt.epSquare)];
    }

    newSt.key = k;

    newSt.nonPawnMaterial[us]   = st_->nonPawnMaterial[us];
    newSt.nonPawnMaterial[them] = st_->nonPawnMaterial[them];
    auto isNP = [](Piece p) {
        PieceType t = type_of(p);
        return t != PAWN && t != KING && p != NO_PIECE;
    };
    if (isNP(pc))   newSt.nonPawnMaterial[us] -= PieceValue[pc];
    if (isNP(placed))newSt.nonPawnMaterial[us] += PieceValue[placed];
    if (isNP(captured)) newSt.nonPawnMaterial[us] += PieceValue[captured];

    if (Do) {
        st_ = &newSt;
        newSt.next = nullptr;
        sideToMove_ = them;
        ++gamePly_;
        set_check_info(&newSt);
    }
}

template void Position::do_move<true>(Move, StateInfo&);
template void Position::do_move<false>(Move, StateInfo&);

void Position::undo_move(Move m) {
    sideToMove_ = ~sideToMove_;
    Color us = sideToMove_;
    Square from = from_sq(m), to = to_sq(m);
    Piece pc = board_[to];
    PieceType pt = type_of(pc);

    // Undo a promotion. The move's own promotion field is the only reliable
    // test: after do_move the piece on `to` is a queen/rook/..., and after the
    // move the piece on `to` may equally be a king or rook that merely
    // happened to travel along rank 1 or 8. Testing the piece type or the
    // destination rank instead replaces those with pawns, which leaves the side
    // with no king and makes every later king_square() read out of bounds.
    if (promo_type(m) != NO_PT) {
        remove_piece(to);
        put_piece(make_piece(us, PAWN), to);
    }
    (void)pt;
    move_piece(to, from);

    if (is_castling(m)) {
        bool kingSide = file_of(to) > file_of(from);
        Square rfrom = relative_square(us, kingSide ? SQ_H1 : SQ_A1);
        Square rto   = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
        move_piece(rto, rfrom);
    }

    if (st_->capturedPiece != NO_PIECE) {
        Square capSq = is_ep(m) ? make_square(file_of(to), rank_of(from)) : to;
        put_piece(st_->capturedPiece, capSq);
    }

    st_ = st_->previous;
    --gamePly_;
}

void Position::do_null_move(StateInfo& newSt) {
    newSt.previous      = st_;
    newSt.capturedPiece = NO_PIECE;
    newSt.epSquare      = SQ_NONE;
    newSt.pliesFromNull = 0;
    newSt.rule50        = st_->rule50 + 1;
    newSt.castlingRights = st_->castlingRights;
    newSt.nonPawnMaterial[WHITE] = st_->nonPawnMaterial[WHITE];
    newSt.nonPawnMaterial[BLACK] = st_->nonPawnMaterial[BLACK];
    newSt.key = st_->key ^ ZobristSide
              ^ (st_->epSquare != SQ_NONE ? ZobristEnPassant[file_of(st_->epSquare)] : 0ULL);
    st_ = &newSt;
    sideToMove_ = ~sideToMove_;
    ++gamePly_;
    set_check_info(&newSt);
}

void Position::undo_null_move() {
    st_ = st_->previous;
    sideToMove_ = ~sideToMove_;
    --gamePly_;
}

// ---------------------------------------------------------------------------
// legality
// ---------------------------------------------------------------------------

bool Position::legal(Move m) const {
    Color us = sideToMove_, them = ~us;
    Square from = from_sq(m), to = to_sq(m);
    Square ksq = king_square(us);

    if (is_castling(m)) {
        bool kingSide = file_of(to) > file_of(from);
        return can_castle_now(us, kingSide);
    }

    if (type_of(board_[from]) == KING)
        return !attacked_by(to, (pieces() & ~Bitboards::square_bb(from)) | Bitboards::square_bb(to), them);

    if (is_ep(m)) {
        Square capSq = make_square(file_of(to), rank_of(from));
        U64 occ = (pieces() & ~Bitboards::square_bb(from) & ~Bitboards::square_bb(capSq))
                | Bitboards::square_bb(to);
        return !attacked_by(ksq, occ, them);
    }

    // If the moving piece is not a blocker for our king, the move is legal.
    if (!(blockers_for_king(us) & Bitboards::square_bb(from))) return true;
    return Bitboards::aligned(from, to, ksq);
}

bool Position::gives_check(Move m) const {
    Square from = from_sq(m), to = to_sq(m);
    Piece pc = board_[from];
    PieceType pt = type_of(pc);
    bool ep = is_ep(m);
    if (is_castling(m)) {
        Color us = sideToMove_;
        Square rto = relative_square(us, file_of(to) > file_of(from) ? SQ_F1 : SQ_D1);
        return Bitboards::rook_attacks_bb(rto, pieces() ^ Bitboards::square_bb(from))
             & pieces(~us, ROOK, ~us, QUEEN)
             || Bitboards::bishop_attacks_bb(rto, pieces() ^ Bitboards::square_bb(from))
             & pieces(~us, BISHOP, ~us, QUEEN);
    }

    U64 occ = (pieces() ^ Bitboards::square_bb(from)) | Bitboards::square_bb(to);
    if (ep) occ ^= Bitboards::square_bb(make_square(file_of(to), rank_of(from)));
    if (pt == PAWN && promo_type(m) != NO_PT) return true;  // conservative

    Square ksq = king_square(~sideToMove_);
    switch (pt) {
    case KNIGHT: return Bitboards::knight_attacks_bb(to) & Bitboards::square_bb(ksq);
    case BISHOP: return Bitboards::bishop_attacks_bb(to, occ) & Bitboards::square_bb(ksq);
    case ROOK:   return Bitboards::rook_attacks_bb(to, occ) & Bitboards::square_bb(ksq);
    case QUEEN:  return (Bitboards::queen_attacks_bb(to, occ)) & Bitboards::square_bb(ksq);
    default:     return false;
    }
}

} // namespace Aurora
