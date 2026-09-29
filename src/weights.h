// Aurora - tunable evaluation weights.
//
// Every constant that a tuner is allowed to modify lives in this one array so
// that tools/tune.py can perturb exactly one weight at a time and so that the
// search/eval code contains no magic numbers.  The names are consumed by the
// tuner and by the `eval` UCI command.

#pragma once
#include "types.h"

namespace Aurora {

struct WeightTable {
    // ---- material (tapered: mg, eg) ----
    int mg_value[PIECE_NB] = {
        0,
        82, 337, 365, 477, 1025, 0,
        0,
        0,
        82, 337, 365, 477, 1025, 0,
        0
    };
    int eg_value[PIECE_NB] = {
        0,
        94, 281, 297, 512, 936, 0,
        0,
        0,
        94, 281, 297, 512, 936, 0,
        0
    };

    // ---- piece-square tables (mg, eg) ----
    //
    // Indexed by PieceType and mirrored by square ^ 56 for Black - deliberately
    // NOT one table per colour. These were originally [PIECE_NB] with only eight
    // rows supplied, so indices 9..14 (every Black piece) were zero-initialised:
    // White was scored with its piece-square table and Black with none, which
    // showed up as the start position evaluating ~830cp against the mover.
    // A single mirrored table cannot be half-written.
    int mg_psqt[PIECE_TYPE_NB][SQUARE_NB] = {
        {0},
        {  0,   0,   0,   0,   0,   0,   0,   0,
           98, 134,  61,  95,  68, 126,  34, -11,
           -6,   7,  26,  31,  65,  56,  25, -20,
          -14,  13,   6,  21,  23,  12,  17, -23,
          -27,  -2,  -5,  12,  17,   6,  10, -25,
          -26,  -4,  -4, -10,   3,   3,  33, -12,
          -35,  -1, -20, -23, -15,  24,  38, -22},
        { -29, -51, -23, -15, -22, -18, -50, -64,
          -42, -20, -10,  -5,  -2, -20, -23, -44,
          -23,  -9,  12,  10,  19,  17,  25, -16,
          -13,   4,  16,  13,  28,  19,  21,  -8,
           -9,  17,  19,  53,  37,  69,  18, -47,
          -13,   4,  16,  13,  12,  17,  17, -23,
          -27, -71, -56, -26,  24, -13, -43, -51},
        { -24, -13, -7,  -2,  4,  -2, -13, -24,
          -16,  -1,  5,  -3,  1,   6,  -1, -17,
          -10,  10,  2,  14,   4,  -6,  -8, -13,
            4,   4,  13,   1,   6,   2,   0,  -6,
           -8,  10,   9,  17,  14,  15,  10,  -9,
          -11,  -2,   3,  -2,   8,   3,  -2,  -9,
          -12, -11, -20, -13, -20, -11, -12, -13},
        { -10,  11,  11,  13,  13,  10,   7,  -3,
            2,  17,  17,  21,  19,  17,  16,   2,
           -4,  16,  15,  20,  18,  20,  16,   4,
           -8,   4,  15,  12,  22,  17,  10,  -4,
          -4,   3,  13,   5,  19,   9,   2,  -4,
           -7,   2,  10,   6,   7,   5,   0,  -9,
           -5,   3,  -1,  -1,   2,  -1,   2,  -6},
        { -33, -28, -22, -43,  -5, -32, -20, -41,
          -22, -23, -30, -16, -16, -23, -36, -32,
          -18,  27,  19,  47,  25,  30,   8,   0,
           -9,  24,  39,  45,  57,  40,  57,  36,
          -19,   6,  21,  26,  40,  38,  26,  18,
          -45, -37, -36, -30, -17, -25, -28, -43,
          -36, -27, -21, -25, -22, -20, -30, -37},
        { -24, -54, -21, -11,  28,  14,  24,  -3,
          -13, -17,   7,   8,  29,  56,  47,  -6,
          -27, -27, -16, -16,  -1,  17,  -2,  -8,
           -9, -26,  -9, -10,  -2,  -4,   3,  -3,
          -18,  -4,  21,  24,  27,  23,   9, -11,
           -8, -22, -24,  -1,  16,  -6,  -6, -17,
          -43, -31, -40, -14, -30, -13, -32, -22},
        {-105, -21, -58, -33, -17, -28, -19, -23,
           -27, -11,   4,  13,  14,   4,  -5, -17,
          -26,  -8, -10,   2,   4,   4, -10, -13,
           -9,  10,   2,  -7,  -9, -10,   3,  -2,
           15,  13,   7,   7,  -5,  -6,  -3,  -9,
          -12, -12, -15,  -9, -10, -12, -15, -17,
          -30, -32, -43, -48, -58, -38, -51, -30}
    };

    int eg_psqt[PIECE_TYPE_NB][SQUARE_NB] = {
        {0},
        {  0,   0,   0,   0,   0,   0,   0,   0,
         178, 173, 158, 134, 147, 132, 165, 187,
          94, 100,  85,  67,  56,  53,  82,  84,
          32,  24,  13,   5,  -2,   4,  17,  17,
          13,   9,  -3,  -7,  -7,  -8,   3,  -1,
           4,   7,  -6,   1,   0,  -5,  -1,  -8,
          13,   8,   8,  10,  13,   0,   2,  -7},
        {-50, -40, -30, -20, -10, -26, -38, -50,
         -35, -25, -14,  -8,  -5,  -6, -16, -25,
         -18,  -6,   0,   4,  12,  15,  22,  -8,
         -10,   8,  18,  22,  32,  28,  32,   0,
          -8,  12,  22,  26,  36,  40,  38,  12,
         -22,  10,  16,  24,  34,  36,  26,   6,
         -50, -38, -28, -20, -14, -18, -30, -46},
        {-16, -12,  -8,  -4,   0,   0,  -8, -16,
          -8,  -4,   0,   4,   4,   0,  -4,  -8,
          -4,   0,   4,   8,   8,   4,   0,  -4,
          -4,   0,   4,   8,   8,   8,   0,  -4,
          -4,   4,   4,   8,  12,   8,   4,  -4,
          -8,   4,   0,   4,   4,   0,  -4,  -8,
         -16, -12,  -8,  -4,   0,   0,  -4, -12},
        { -2,   2,   6,   8,  10,  12,  10,   2,
           10,  14,  16,  20,  22,  18,  16,   8,
           -2,   0,   6,   8,  10,   6,   4,  -2,
           -4,   0,   2,   4,   6,   6,   2,  -4,
           -6,  -2,   0,   2,   4,   4,   2,  -6,
           -4,  -2,   0,   4,   4,   2,   0,  -4,
           -8,  -2,   0,   4,   4,   2,  -2,  -6},
        {-12, -10,  -2,   6,  10,  12,   6,  -8,
           -8,   0,   8,  14,  18,  16,   8,  -4,
            0,  12,  24,  30,  34,  30,  18,  -2,
            14,  20,  30,  36,  40,  38,  24,  10,
            10,  18,  26,  34,  36,  32,  20,   4,
           -6,   2,  12,  16,  20,  16,   8,  -6,
          -12, -10,   0,   8,  10,   4,  -4, -12},
        { -4,  -4,   0,   4,   4,   0,  -4,  -4,
            0,   4,   8,  12,  12,   8,   4,   0,
           -4,   0,   4,   8,   8,   4,   0,  -4,
           -4,   0,   4,   8,   8,   4,   0,  -4,
           -4,   0,   4,   8,   8,   4,   0,  -4,
           -4,   0,   4,   8,   8,   4,   0,  -4,
           -8,  -8,   0,   4,   4,   0,  -8,  -8},
        {-53, -34, -21, -11, -28, -14, -24, -43,
          -27, -11,   4,  13,  14,   4,  -5, -17,
          -26,  -8, -10,   2,   4,   4, -10, -13,
           -9,  10,   2,  -7,  -9, -10,   3,  -2,
           15,  13,   7,   7,  -5,  -6,  -3,  -9,
          -12, -12, -15,  -9, -10, -12, -15, -17,
          -30, -32, -43, -48, -58, -38, -51, -30}
    };

    // ---- positional terms ----
    int mg_knight_mobility = 4;   int eg_knight_mobility = 4;
    int mg_bishop_mobility = 4;   int eg_bishop_mobility = 5;
    int mg_rook_mobility   = 2;   int eg_rook_mobility   = 6;
    int mg_queen_mobility  = 1;   int eg_queen_mobility  = 3;
    int mg_king_mobility   = -8;  int eg_king_mobility   = -12;
    int mg_pawn_mobility   = 0;   int eg_pawn_mobility   = 0;

    int mg_bishop_pair      = 30;  int eg_bishop_pair      = 55;
    int mg_rook_open_file    = 28;  int eg_rook_open_file    = 12;
    int mg_rook_semi_open    = 13;  int eg_rook_semi_open    = 7;
    int mg_rook_on_passed    = 22;  int eg_rook_on_passed    = 8;
    int mg_rook_on_7th       = 12;  int eg_rook_on_7th       = 28;
    int mg_knight_outpost    = 20;  int eg_knight_outpost    = 6;
    int mg_bishop_outpost    = 8;   int eg_bishop_outpost    = 2;
    int mg_strapped_knight   = -30; int eg_strapped_knight   = -12;
    int mg_minor_behind_pawn  = 8;   int eg_minor_behind_pawn  = 4;
    int mg_pawn_on_7th        = 8;   int eg_pawn_on_7th        = 60;

    int mg_isolated_pawn    = -6;  int eg_isolated_pawn    = -12;
    int mg_doubled_pawn     = -8;  int eg_doubled_pawn     = -22;
    int mg_backward_pawn    = -8;  int eg_backward_pawn    = -10;
    int mg_connected_pawn   = 6;   int eg_connected_pawn   = 4;
    int mg_bishop_king_prox  = 5;   int eg_bishop_king_prox  = 0;
    int mg_rook_king_prox    = 4;   int eg_rook_king_prox    = 2;
    int mg_queen_king_prox   = 2;   int eg_queen_king_prox   = 2;
    int mg_shelter_bonus     = 6;   int eg_shelter_bonus     = 0;
    int mg_storm_penalty     = -18; int eg_storm_penalty     = -6;
    int mg_flank_attacks     = 4;   int eg_flank_attacks     = 0;
    int mg_king_protector     = 8;   int eg_king_protector     = 0;
    int mg_passed_pawn_bonus[SQUARE_NB] = {
        0,   2,   6,  14,  30,  62, 100, 0,
        0,   4,  10,  22,  46,  92, 148, 0,
        0,   6,  16,  34,  66, 126, 196, 0,
        0,  10,  26,  56, 100, 178, 258, 0,
        0,  16,  40,  84, 148, 258, 360, 0,
        0,  24,  58, 120, 204, 336, 460, 0,
        0,  34,  82, 164, 272, 420, 560, 0,
        0,   0,   0,   0,   0,   0,   0, 0
    };
    int eg_passed_pawn_bonus[SQUARE_NB] = {
        0,  10,  16,  30,  58, 100, 150, 0,
        0,  14,  26,  46,  84, 136, 196, 0,
        0,  20,  38,  66, 112, 176, 244, 0,
        0,  28,  54,  92, 148, 226, 300, 0,
        0,  40,  76, 124, 192, 288, 372, 0,
        0,  56, 104, 166, 250, 366, 460, 0,
        0,  76, 140, 220, 320, 460, 560, 0,
        0,   0,   0,   0,   0,   0,   0, 0
    };
    int mg_doubled_pawn_penalty = 0; int eg_doubled_pawn_penalty = 0;

    int mg_contempt = 18;
    int mg_threat_by_safe_pawn = 90;
    int mg_threat_by_rook = 12;
    int mg_threat_by_knight = 20;
    int mg_threat_by_bishop = 20;
    int mg_threat_by_queen = 4;
    int mg_weak_queen = 82;
    int mg_weak_rook = 30;
    int mg_fortress = 22;

    // ---- king safety ----
    //
    // These are *small* on purpose. The attack pressure is a weighted count of
    // enemy pieces bearing on our king ring, and the danger term is quadratic
    // in that count, so the weights only need to be comparable to each other -
    // the shape of the curve supplies the rest. The first version of this
    // evaluator used weights in the 500-1000 range and then squared the sum,
    // which reached 12000cp in ordinary middlegame positions and saturated the
    // score clamp at +/-9999.
    int king_attacker_knight = 20;   // per enemy knight bearing on the ring
    int king_attacker_bishop = 18;   // per enemy bishop
    int king_attacker_rook   = 28;   // per enemy rook
    int king_attacker_queen  = 35;   // per enemy queen, doubled when adjacent
    int king_danger_scale    = 100;  // danger = units^2 * scale / 10000
    int king_danger_max      = 300;  // hard ceiling on the attack penalty

    // ---- mobility centring ----
    // Mobility is a raw count, so each piece needs a baseline to subtract or
    // every position inherits a large constant offset from its piece count.
    int mg_mobility_base[PIECE_TYPE_NB] = {0, 0, 6, 6, 8, 9, 0};
    int eg_mobility_base[PIECE_TYPE_NB] = {0, 0, 6, 6, 8, 9, 0};

    // ---- Aurora-specific ----
    int noise_min = 2;    // depth at which the deterministic noise term activates
    int noise_max = 3;    // noise magnitude in centipawns (must stay tiny)
    int spark_mg   = 4;   // SPARK: middlegame weight per unit of created pressure
    int spark_eg   = 2;   // SPARK: endgame weight
    int spark_storm = 8;  // SPARK: extra weight when we are behind on material
    int tempo_bonus = 14;

    int mg_scale = 100;   // global eval scale (%)
    int eg_scale = 100;
};

// Flat, name-addressable view over WeightTable for the tuner.
int  num_tunable();
const char* tunable_name(int idx);
int* tunable_ref(int idx);

} // namespace Aurora
