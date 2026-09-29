// Aurora - evaluation
//
// Design notes
// ------------
// The evaluation is a *tapered* additive evaluator with a learned/tunable
// weight set (see weights.h).  Two ideas are specific to this engine:
//
//   1. NOISE  - a deterministic, position-hash-derived dither is added to
//               shallow scores.  It is *not* random: it is a function of the
//               position key, so the same position always evaluates the same.
//               This breaks the "eval is a staircase" artifact at shallow
//               depths, which lets alpha-beta distinguish near-equal
//               positions and produces genuinely different, more diverse
//               move choices among equally-valued alternatives.
//
//   2. SPARK - a "tension" term measuring how many of *our* attackers bear on
//               *enemy* defended pieces that we cannot profitably capture
//               (poor SEE).  This is not a mobility term; it rewards creating
//               pressure that outlives the current search, and it scales with
//               who is winning.  It is the main *new* evaluation concept here.
//
// Everything is expressed as a sum of small terms in centipawns and blended
// between a middlegame and an endgame table using a game-phase value.

#pragma once
#include "position.h"
#include "bitboard.h"

namespace Aurora {

struct EvalInfo {
    // per-side accumulators
    int score[COLOR_NB] = {0, 0};
    // cached tables
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB] = {};
    Bitboard attackedBy2[COLOR_NB][PIECE_TYPE_NB] = {};
    Bitboard attackedByAll[COLOR_NB] = {};
    Bitboard mobilityArea[COLOR_NB] = {};
    int kingAttackersCount[COLOR_NB] = {};
    int kingAttacksCount[COLOR_NB] = {};
    int kingAttackersWeight[COLOR_NB] = {};
    int kingAttackersUnit[COLOR_NB] = {};
    Bitboard kingRing[COLOR_NB] = {};
    Bitboard pawnAttacks[COLOR_NB] = {};
    Bitboard pawnAttacksAll[COLOR_NB] = {};
    int gamePhase = 0;
    int nonPawnMaterial[COLOR_NB] = {};
};

namespace Eval {
    Value evaluate(const Position& pos);
    // Scale a centipawn score into a "draw-ish" range as the endgame approaches
    int scale_factor(int cp);
    bool worth_printing(const Position& pos);
    void trace(const Position& pos);
    // When set, evaluate() dumps the per-side accumulators to stderr. Needed
    // because a colour-asymmetry test that compares "side to move" against
    // "other side" is blind, by construction, to any term that depends on the
    // mover rather than the colour - which is a different and equally fatal bug.
    extern bool trace_enabled;
    // Read/write weights (for tuning)
    void set_weight(int idx, int v);
    int  get_weight(int idx);
    int  weight_count();
    const char* weight_name(int idx);
}

} // namespace Aurora
