// Aurora - position representation
#pragma once
#include "types.h"
#include "bitboard.h"
#include <vector>

namespace Aurora {

struct StateInfo {
    Piece    capturedPiece = NO_PIECE;
    int      castlingRights = 0;
    Square   epSquare = SQ_NONE;
    int      rule50 = 0;
    int      pliesFromNull = 0;
    U64      key = 0;
    Bitboard checkers = 0;
    Bitboard blockersForKing[COLOR_NB] = {};
    Bitboard checkSquares[PIECE_TYPE_NB] = {};
    StateInfo* previous = nullptr;
    StateInfo* next = nullptr;
    int64_t  nonPawnMaterial[COLOR_NB] = {};
};

// A pre-allocated state array.  `do_move` is always called with an explicit
// StateInfo so that no allocation ever happens inside the search.
using StateStack = std::vector<StateInfo>;

class Position {
public:
    Position() = default;

    void set(const std::string& fen, StateInfo* si);
    std::string fen() const;

    // --- piece access ---
    Piece piece_on(Square s) const { return board_[s]; }
    bool empty(Square s) const { return board_[s] == NO_PIECE; }
    int count(PieceType pt) const { return piece_count_[make_piece(WHITE, pt)] + piece_count_[make_piece(BLACK, pt)]; }
    int count(Color c, PieceType pt) const { return piece_count_[make_piece(c, pt)]; }

    template<PieceType Pt> Square square(Color c) const { return Square(lsb(pieces(c, Pt))); }
    Square king_square(Color c) const { return square<KING>(c); }

    Bitboard pieces() const { return byTypeBB_[ALL_PIECES]; }
    Bitboard pieces(PieceType pt) const { return byTypeBB_[pt]; }
    Bitboard pieces(PieceType a, PieceType b) const { return byTypeBB_[a] | byTypeBB_[b]; }
    template<PieceType... Pts> Bitboard pieces() const { return (byTypeBB_[Pts] | ...); }
    Bitboard pieces(Color c) const { return byColorBB_[c]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColorBB_[c] & byTypeBB_[pt]; }
    template<PieceType... Pts> Bitboard pieces(Color c) const { return byColorBB_[c] & (byTypeBB_[Pts] | ...); }
    Bitboard pieces(Color a, PieceType pa, Color b, PieceType pb) const {
        return (byColorBB_[a] & byTypeBB_[pa]) | (byColorBB_[b] & byTypeBB_[pb]);
    }

    // --- game state ---
    Color side_to_move() const { return sideToMove_; }
    int game_ply() const { return gamePly_; }
    int rule50_count() const { return st_->rule50; }
    int plies_from_null() const { return st_->pliesFromNull; }

    // --- checks ---
    Bitboard checkers() const { return st_->checkers; }
    Bitboard blockers_for_king(Color c) const { return st_->blockersForKing[c]; }
    Bitboard check_squares(PieceType pt) const { return st_->checkSquares[pt]; }
    bool in_check() const { return st_->checkers; }
    // True if pieces of colour `by` attack square `s` given occupancy `occ`.
    // The colour matters: counting our own pieces as attackers makes every
    // king move toward one of our own pieces look illegal.
    bool attacked_by(Square s, U64 occ, Color by) const;

    // --- castling ---
    int castling_rights() const { return st_->castlingRights; }
    bool can_castle(CastlingRights cr) const { return (st_->castlingRights & cr) != 0; }
    Square castling_rook_square(CastlingRights cr) const {
        Color c = (cr & WHITE_CASTLING) ? WHITE : BLACK;
        bool kside = (cr & (WHITE_OO | BLACK_OO)) != 0;
        return relative_square(c, kside ? SQ_H1 : SQ_A1);
    }
    bool is_castling(Move m) const {
        return type_of(board_[from_sq(m)]) == KING
            && std::abs(int(file_of(from_sq(m))) - int(file_of(to_sq(m)))) == 2;
    }
    // Can the given side castle in this position (rights + clear path + not in check)?
    bool can_castle_now(Color c, bool kingSide) const;

    // --- move legality ---
    bool legal(Move m) const;
    bool gives_check(Move m) const;
    bool capture(Move m) const { return is_ep(m) || !empty(to_sq(m)); }
    bool capture_or_promotion(Move m) const { return capture(m) || promo_type(m) != NO_PT; }
    Piece moved_piece(Move m) const { return board_[from_sq(m)]; }

    template<bool Do>
    void do_move(Move m, StateInfo& newSt);
    void do_move(Move m, StateInfo& newSt) { do_move<true>(m, newSt); }
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    U64 key() const { return st_->key; }
    int64_t non_pawn_material(Color c) const { return st_->nonPawnMaterial[c]; }
    int64_t non_pawn_material() const { return st_->nonPawnMaterial[WHITE] + st_->nonPawnMaterial[BLACK]; }
    StateInfo* state() const { return st_; }
    void set_state(StateInfo* si) { st_ = si; }
    int64_t simple_material(Color c) const;

private:
    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);
    void set_check_info(StateInfo* si) const;
    int compute_castling_rights() const;

    Piece board_[64] = {};
    Bitboard byTypeBB_[PIECE_TYPE_NB] = {};
    Bitboard byColorBB_[COLOR_NB] = {};
    int piece_count_[PIECE_NB] = {};
    int count_[SQUARE_NB] = {};
    int gamePly_ = 0;
    Color sideToMove_ = WHITE;
    StateInfo* st_ = nullptr;
};

extern U64 ZobristPsq[PIECE_NB][SQUARE_NB];
extern U64 ZobristCastling[CASTLING_RIGHT_NB];
extern U64 ZobristEnPassant[FILE_NB];
extern U64 ZobristSide;
void init_zobrist();

} // namespace Aurora
