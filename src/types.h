// Aurora - core types
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <algorithm>
#include <iostream>

using U64 = uint64_t;
using U32 = uint32_t;
using U16 = uint16_t;
using U8  = uint8_t;
using i16 = int16_t;
using i32 = int32_t;
using Bitboard = U64;

enum Color : int { WHITE, BLACK, COLOR_NB = 2 };

enum PieceType : int {
    NO_PT, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    ALL_PIECES = 0, PIECE_TYPE_NB = 8
};

enum Piece : int {
    NO_PIECE,
    W_PAWN = 1, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN = 9, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    PIECE_NB = 16
};

constexpr Piece make_piece(Color c, PieceType pt) { return Piece((c << 3) | pt); }
constexpr PieceType type_of(Piece p) { return PieceType(p & 7); }
constexpr Color color_of(Piece p) { return Color(p >> 3); }
constexpr bool is_ok(Piece p) { return (p & 0x88) == 0; }

// Squares: A1=0 .. H8=63
enum Square : int {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE, SQUARE_NB = 64
};

enum File : int { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H, FILE_NB };
enum Rank : int { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8, RANK_NB };

constexpr File file_of(Square s) { return File(int(s) & 7); }
constexpr Rank rank_of(Square s) { return Rank(int(s) >> 3); }
constexpr Square make_square(File f, Rank r) { return Square((int(r) << 3) | int(f)); }
constexpr Square from_sq(int i) { return Square(i); }

constexpr bool is_ok(Square s) { return s >= SQ_A1 && s <= SQ_H8; }

// Increment operators for the strongly-typed enumerations.
//
// BOTH forms must exist. With only `operator++(T) -> T` (the postfix shape),
// a prefix `++x` still binds `x` to a *copy*, increments the temporary and
// discards it - so the loop variable never advances and the loop spins
// forever. Defining the prefix form as `T& operator++(T&)` makes the intent
// explicit and cannot be mis-bound.
constexpr Square& operator++(Square& s) { return s = Square(int(s) + 1); }
constexpr Color&  operator++(Color& c)  { return c = Color(int(c) + 1); }
constexpr File&   operator++(File& f)   { return f = File(int(f) + 1); }
constexpr Rank&   operator++(Rank& r)   { return r = Rank(int(r) + 1); }
constexpr PieceType& operator++(PieceType& p) { return p = PieceType(int(p) + 1); }

constexpr Square operator++(Square s, int) { return Square(int(s) + 1); }
constexpr Color  operator++(Color c, int)  { return Color(int(c) + 1); }
constexpr File   operator++(File f, int)   { return File(int(f) + 1); }
constexpr Rank   operator++(Rank r, int)   { return Rank(int(r) + 1); }
constexpr PieceType operator++(PieceType p, int) { return PieceType(int(p) + 1); }

constexpr File   operator+(File f, int i)   { return File(int(f) + i); }
constexpr Rank   operator+(Rank r, int i)   { return Rank(int(r) + i); }
constexpr Square operator+(Square s, int i) { return Square(int(s) + i); }
constexpr int    operator-(File a, File b) { return int(a) - int(b); }
constexpr int    operator-(Rank a, Rank b) { return int(a) - int(b); }
constexpr int    operator-(Square a, Square b) { return int(a) - int(b); }

// Direction is the signed number of ranks (x8) a move travels.  All shifts are
// expressed as additions to a square index, so EAST/WEST are +/-1.
enum Direction : int {
    NORTH = 8, EAST = 1, SOUTH = -8, WEST = -1,
    NORTH_EAST = NORTH + EAST, SOUTH_EAST = SOUTH + EAST,
    SOUTH_WEST = SOUTH + WEST, NORTH_WEST = NORTH + WEST
};

constexpr int push_shift(Color c) { return c == WHITE ? int(NORTH) : int(SOUTH); }
constexpr Direction pawn_push(Color c) { return c == WHITE ? NORTH : SOUTH; }
constexpr int pawn_double_push(Color c) { return 2 * push_shift(c); }
constexpr Direction pawn_attack_dir(Color c) { return c == WHITE ? NORTH_EAST : SOUTH_WEST; }

constexpr Color opposite(Color c) { return Color(c ^ BLACK); }
constexpr Color operator~(Color c) { return Color(c ^ BLACK); }

// ---- Move encoding (16 bit) ----
// bits 0-5 from, 6-11 to, 12-14 promotion type (0=none), 15 = en-passant
enum Move : int { MOVE_NONE = 0, MOVE_NULL = 65 };

constexpr Move make_move(Square from, Square to) { return Move(from | (to << 6)); }
constexpr Move make_move(Square from, Square to, PieceType pt) { return Move(from | (to << 6) | (pt << 12)); }
constexpr Move make_ep(Square from, Square to) { return Move(from | (to << 6) | (1 << 15)); }
inline Square from_sq(Move m) { return Square(m & 0x3F); }
inline Square to_sq(Move m) { return Square((m >> 6) & 0x3F); }
inline PieceType promo_type(Move m) { return PieceType((m >> 12) & 7); }
inline bool is_ep(Move m) { return bool(m & (1 << 15)); }
inline bool is_ok(Move m) { return m != MOVE_NONE && m != MOVE_NULL; }

// ---- Value ----
// Scores are plain centipawn integers. A typedef (rather than a wrapper type)
// keeps every comparison and arithmetic site unambiguous and lets the code
// read the way a chess engine normally does.
using Value = int;

constexpr Value VALUE_ZERO     = 0;
constexpr Value VALUE_DRAW     = 0;
constexpr Value VALUE_MATE     = 32000;
constexpr Value VALUE_INFINITE = 32001;
constexpr Value VALUE_NONE     = 32002;

constexpr Value VALUE_TB_WIN         = VALUE_MATE - 1;
constexpr Value VALUE_MATE_IN_MAX_PLY  = VALUE_MATE - 2;
constexpr Value VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY;
// Beyond this magnitude the position is treated as decided for scaling.
constexpr Value VALUE_KNOWN_WIN = 10000;

// Bound types
// Castling rights bit indices
enum CastlingRights : int {
    NO_CASTLING = 0, WHITE_OO = 1, WHITE_OOO = 2,
    BLACK_OO = 4, BLACK_OOO = 8,
    WHITE_CASTLING = WHITE_OO | WHITE_OOO,
    BLACK_CASTLING = BLACK_OO | BLACK_OOO,
    ANY_CASTLING = WHITE_CASTLING | BLACK_CASTLING,
    CASTLING_RIGHT_NB = 16
};

constexpr Square relative_square(Color c, Square s) { return Square(int(s) ^ (c * 56)); }
constexpr Rank relative_rank(Color c, Rank r) { return Rank(int(r) ^ (c * 7)); }
constexpr Rank relative_rank(Color c, Square s) { return relative_rank(c, rank_of(s)); }
constexpr File relative_file(Color c, File f) { return File(int(f) ^ (c * 7)); }
constexpr Square pawn_push_sq(Color c, Square s) { return Square(int(s) + push_shift(c)); }
