// Aurora - bitboard operations and attack tables (magic bitboards)
#pragma once
#include "types.h"
#include <bit>

namespace Bitboards {

void init();
// Populate only the tables that do not depend on magic numbers.
void init_baseline_tables();

inline int popcount(U64 b) { return std::popcount(b); }
inline int lsb(U64 b) { return std::countr_zero(b); }
inline int msb(U64 b) { return 63 ^ std::countl_zero(b); }

inline File lsb_file(U64 b) { return File(lsb(b) & 7); }
inline Rank lsb_rank(U64 b) { return Rank(lsb(b) >> 3); }
inline Square pop_lsb(U64& b) { Square s = Square(lsb(b)); b &= b - 1; return s; }

extern U64 SquareBB[64];
extern U64 FileBB[8];
extern U64 RankBB[8];
extern U64 BetweenBB[64][64];
extern U64 LineBB[64][64];

inline U64 square_bb(Square s) { return SquareBB[s]; }
inline U64 file_bb(File f) { return FileBB[f]; }
inline U64 rank_bb(Rank r) { return RankBB[r]; }
inline U64 between_bb(Square a, Square b) { return BetweenBB[a][b]; }
inline U64 line_bb(Square a, Square b) { return LineBB[a][b]; }

inline U64 shift_bb(int d, U64 b) {
    switch (d) {
    case 8:            return b << 8;
    case -8:           return b >> 8;
    case 16:           return b << 16;
    case -16:          return b >> 16;
    case 1:            return (b & ~FileBB[FILE_H]) << 1;
    case -1:           return (b & ~FileBB[FILE_A]) >> 1;
    case NORTH_EAST:   return (b & ~FileBB[FILE_H]) << 9;
    case NORTH_WEST:   return (b & ~FileBB[FILE_A]) << 7;
    case SOUTH_EAST:   return (b & ~FileBB[FILE_H]) >> 7;
    case SOUTH_WEST:   return (b & ~FileBB[FILE_A]) >> 9;
    default:           return 0;
    }
}

U64 pawn_attacks_bb(Color c, U64 b);

extern U64 KnightTable[64];
extern U64 KingTable[64];
inline U64 knight_attacks_bb(Square s) { return KnightTable[s]; }
inline U64 king_attacks_bb(Square s) { return KingTable[s]; }
inline U64 knight_attacks_bb(U64 b) { return KnightTable[lsb(b)]; }

U64 bishop_attacks_bb(Square s, U64 occ);
U64 rook_attacks_bb(Square s, U64 occ);
inline U64 queen_attacks_bb(Square s, U64 occ) { return bishop_attacks_bb(s, occ) | rook_attacks_bb(s, occ); }

extern U64 BishopTable[64][64];
extern U64 RookTable[64][64];
extern U64 BishopMaskA[64], BishopMaskB[64];
extern U64 RookMaskRank[64], RookMaskFile[64];
extern int  BishopStride[64];

// Exhaustively re-check every installed sliding-attack table against a
// ray-walking reference. Returns the number of mismatches (0 == healthy).
int verify_attack_tables();

template<PieceType Pt> inline U64 attacks_bb(Square s, U64 occ) {
    if constexpr (Pt == BISHOP) return bishop_attacks_bb(s, occ);
    else if constexpr (Pt == ROOK) return rook_attacks_bb(s, occ);
    else if constexpr (Pt == QUEEN) return queen_attacks_bb(s, occ);
    else return 0;
}
template<PieceType Pt> inline U64 attacks_bb(U64 b, U64 occ) {
    U64 r = 0;
    while (b) r |= attacks_bb<Pt>(pop_lsb(b), occ);
    return r;
}

inline bool aligned(Square a, Square b, Square c) { return (LineBB[a][b] & SquareBB[c]) != 0; }

inline int distance(Square a, Square b) {
    return std::max(std::abs(file_of(a) - file_of(b)), std::abs(rank_of(a) - rank_of(b)));
}
inline int file_distance(Square a, Square b) { return std::abs(file_of(a) - file_of(b)); }
inline int rank_distance(Square a, Rank r) { return std::abs(int(rank_of(a)) - int(r)); }

// Between/line for a "there and back" pair
inline U64 between_bb_sym(Square a, Square b) { return BetweenBB[a][b]; }

} // namespace Bitboards

// Bring the bitboard primitives into the global namespace: they are used
// unqualified throughout the engine.
using Bitboards::popcount;
using Bitboards::lsb;
using Bitboards::msb;
using Bitboards::pop_lsb;

// Nominal piece values (cp) - used for SEE, razoring, material counting.
extern const int PieceValue[PIECE_NB];

// Max distance from a square to any other (for distance tables)
inline int max_dist() { return 7; }

inline const char* PieceName(PieceType pt) {
    static const char* n[8] = {"", "P", "N", "B", "R", "Q", "K", ""};
    return n[pt & 7];
}
inline const char* SquareName(Square s) {
    static const char f[8] = {'a','b','c','d','e','f','g','h'};
    static char buf[3];
    if (s < 0 || s > 63) return "-";
    buf[0] = f[file_of(s)];
    buf[1] = char('1' + rank_of(s));
    return buf;
}
