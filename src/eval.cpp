// Aurora - evaluation
//
// A tapered, additive classical evaluator.
//
// The single most important structural rule here: every positional term is
// computed for BOTH colours and differenced. The first version of this file
// differenced material but only ever *added* the side-to-move's mobility,
// threats, pawn structure and king safety, which made "being to move" worth
// several hundred centipawns and had the start position evaluating at +538.
// So the evaluation of a colour is a self-contained function of that colour, and
// the final score is its value minus its opponent's.
//
// Scales are deliberately conservative. An earlier king-safety term multiplied
// an attacker weight by a distance *percentage* and then squared the sum,
// reaching 12000cp in ordinary middlegames and saturating the score clamp.
//
// Two terms are this engine's own, both isolated at the end of the file and both
// measurable on their own:
//   * NOISE  - a deterministic, position-hash-derived dither
//   * SPARK  - a "pressure we have created" term
//
// Everything tunable lives in weights.h.

#include "eval.h"
#include "weights.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Aurora {

static WeightTable W;

bool Eval::trace_enabled = false;

// ---------------------------------------------------------------------------
// tunable weight registry (flat, name-addressable view for the tuner)
// ---------------------------------------------------------------------------

namespace {

inline std::string pt_name(PieceType p) { return std::string(1, " PNBRQK"[p & 7]); }

struct Entry { std::string name; int* ptr; };

const std::vector<Entry>& registry() {
    static std::vector<Entry> r;
    if (!r.empty()) return r;

    for (int c = 0; c < COLOR_NB; ++c)
        for (int p = PAWN; p <= KING; ++p) {
            if (p == KING) continue;   // king PSQT is not a search term
            Piece pc = make_piece(Color(c), PieceType(p));
            std::string side = (c == WHITE) ? "w" : "b";
            r.push_back({std::string("mg_value_") + side + pt_name(PieceType(p)), &W.mg_value[pc]});
            r.push_back({std::string("eg_value_") + side + pt_name(PieceType(p)), &W.eg_value[pc]});
        }

    // PSQT is shared between the colours and mirrored at lookup, so it is
    // registered once per type rather than once per colour.
    for (int p = PAWN; p <= QUEEN; ++p)
        for (int s = SQ_A1; s <= SQ_H8; ++s) {
            std::string sq = SquareName(Square(s));
            std::string n = pt_name(PieceType(p)) + "_" + sq;
            r.push_back({std::string("mg_psqt_") + n, &W.mg_psqt[p][s]});
            r.push_back({std::string("eg_psqt_") + n, &W.eg_psqt[p][s]});
        }

    struct { const char* n; int* p; } scalars[] = {
        {"mg_knight_mobility", &W.mg_knight_mobility},   {"eg_knight_mobility", &W.eg_knight_mobility},
        {"mg_bishop_mobility", &W.mg_bishop_mobility},   {"eg_bishop_mobility", &W.eg_bishop_mobility},
        {"mg_rook_mobility",   &W.mg_rook_mobility},     {"eg_rook_mobility",   &W.eg_rook_mobility},
        {"mg_queen_mobility",  &W.mg_queen_mobility},    {"eg_queen_mobility",  &W.eg_queen_mobility},
        {"mg_king_mobility",   &W.mg_king_mobility},     {"eg_king_mobility",   &W.eg_king_mobility},
        {"mg_bishop_pair",     &W.mg_bishop_pair},       {"eg_bishop_pair",     &W.eg_bishop_pair},
        {"mg_rook_open_file",  &W.mg_rook_open_file},    {"eg_rook_open_file",  &W.eg_rook_open_file},
        {"mg_rook_semi_open",  &W.mg_rook_semi_open},    {"eg_rook_semi_open",  &W.eg_rook_semi_open},
        {"mg_rook_on_7th",     &W.mg_rook_on_7th},       {"eg_rook_on_7th",     &W.eg_rook_on_7th},
        {"mg_knight_outpost",  &W.mg_knight_outpost},    {"eg_knight_outpost",  &W.eg_knight_outpost},
        {"mg_bishop_outpost",  &W.mg_bishop_outpost},    {"eg_bishop_outpost",  &W.eg_bishop_outpost},
        {"mg_strapped_knight", &W.mg_strapped_knight},   {"eg_strapped_knight", &W.eg_strapped_knight},
        {"mg_minor_behind_pawn", &W.mg_minor_behind_pawn}, {"eg_minor_behind_pawn", &W.eg_minor_behind_pawn},
        {"mg_pawn_on_7th",     &W.mg_pawn_on_7th},       {"eg_pawn_on_7th",     &W.eg_pawn_on_7th},
        {"mg_isolated_pawn",   &W.mg_isolated_pawn},     {"eg_isolated_pawn",   &W.eg_isolated_pawn},
        {"mg_doubled_pawn",    &W.mg_doubled_pawn},      {"eg_doubled_pawn",    &W.eg_doubled_pawn},
        {"mg_connected_pawn",  &W.mg_connected_pawn},    {"eg_connected_pawn",  &W.eg_connected_pawn},
        {"mg_bishop_king_prox",&W.mg_bishop_king_prox},  {"eg_bishop_king_prox",&W.eg_bishop_king_prox},
        {"mg_rook_king_prox",  &W.mg_rook_king_prox},    {"eg_rook_king_prox",  &W.eg_rook_king_prox},
        {"mg_queen_king_prox", &W.mg_queen_king_prox},   {"eg_queen_king_prox", &W.eg_queen_king_prox},
        {"mg_shelter_bonus",   &W.mg_shelter_bonus},     {"eg_shelter_bonus",   &W.eg_shelter_bonus},
        {"mg_storm_penalty",   &W.mg_storm_penalty},     {"eg_storm_penalty",   &W.eg_storm_penalty},
        {"mg_threat_by_safe_pawn", &W.mg_threat_by_safe_pawn},
        {"mg_threat_by_knight",   &W.mg_threat_by_knight},
        {"mg_threat_by_bishop",   &W.mg_threat_by_bishop},
        {"mg_threat_by_rook",     &W.mg_threat_by_rook},
        {"mg_threat_by_queen",    &W.mg_threat_by_queen},
        {"mg_weak_queen",         &W.mg_weak_queen},
        {"king_attacker_knight",  &W.king_attacker_knight},
        {"king_attacker_bishop",  &W.king_attacker_bishop},
        {"king_attacker_rook",    &W.king_attacker_rook},
        {"king_attacker_queen",   &W.king_attacker_queen},
        {"king_danger_scale",     &W.king_danger_scale},
        {"king_danger_max",       &W.king_danger_max},
        {"noise_max",  &W.noise_max},
        {"spark_mg",   &W.spark_mg},
        {"spark_eg",   &W.spark_eg},
        {"spark_storm",&W.spark_storm},
        {"tempo_bonus",&W.tempo_bonus},
        {"mg_scale",   &W.mg_scale},
    };
    for (auto& s : scalars) r.push_back({s.n, s.p});

    for (int s = SQ_A1; s <= SQ_H8; ++s) {
        std::string sq = SquareName(Square(s));
        r.push_back({std::string("mg_pp_") + sq, &W.mg_passed_pawn_bonus[s]});
        r.push_back({std::string("eg_pp_") + sq, &W.eg_passed_pawn_bonus[s]});
    }
    for (int pt = KNIGHT; pt <= QUEEN; ++pt) {
        std::string n = pt_name(PieceType(pt));
        r.push_back({std::string("mg_mob_base_") + n, &W.mg_mobility_base[pt]});
        r.push_back({std::string("eg_mob_base_") + n, &W.eg_mobility_base[pt]});
    }
    return r;
}

} // namespace

int  num_tunable() { return int(registry().size()); }
const char* tunable_name(int i) { return registry()[i].name.c_str(); }
int* tunable_ref(int i) { return registry()[i].ptr; }
void Eval::set_weight(int idx, int v) { *registry()[idx].ptr = v; }
int  Eval::get_weight(int idx) { return *registry()[idx].ptr; }
int  Eval::weight_count() { return num_tunable(); }
const char* Eval::weight_name(int idx) { return tunable_name(idx); }

// ---------------------------------------------------------------------------
// attack tables
// ---------------------------------------------------------------------------

namespace {

// Game-phase increments, scaled so a full board sums to exactly 24 - the
// divisor used by the taper below. With the natural 1/1/2/4 a full board scored
// 12, so the middlegame tables were applied at half weight in the opening and
// the engine began every game playing as though the queens were off.
constexpr int PHASE_MAX = 24;
constexpr int mg_phase(PieceType pt) {
    switch (pt) {
    case KNIGHT: case BISHOP: return 2;
    case ROOK:   return 4;
    case QUEEN:  return 8;
    default:     return 0;
    }
}

constexpr Square flip_sq(Square s) { return Square(int(s) ^ 56); }

struct Tables {
    Bitboard pawnAttacks[COLOR_NB] = {};
    // attackedBy includes the king's own reach, which is what the pin/outpost
    // and king-pressure terms want. Mobility must NOT: the king attacking its
    // own back rank would otherwise be credited to the knight and the rook.
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB] = {};
    Bitboard realAttacks[COLOR_NB][PIECE_TYPE_NB] = {};
    Bitboard attackedBy2[COLOR_NB][PIECE_TYPE_NB] = {};
    Bitboard attackedByAll[COLOR_NB] = {};
    Bitboard mobilityArea[COLOR_NB] = {};
    Bitboard kingRing[COLOR_NB] = {};
};

void build(const Position& pos, Tables& t) {
    const U64 occ = pos.pieces();

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Color them = ~c;
        t.pawnAttacks[c] = pos.pieces(c, PAWN) ? Bitboards::pawn_attacks_bb(c, pos.pieces(c, PAWN)) : 0;
        const Square ksq = pos.king_square(c);

        t.realAttacks[c][PAWN] = t.pawnAttacks[c];

        Bitboard n = t.pawnAttacks[c];
        Bitboard bb = pos.pieces(c, KNIGHT);
        while (bb) n |= Bitboards::knight_attacks_bb(pop_lsb(bb));
        t.realAttacks[c][KNIGHT] = n;

        bb = t.pawnAttacks[c];
        Bitboard bs = pos.pieces(c, BISHOP, c, QUEEN);
        while (bs) bb |= Bitboards::bishop_attacks_bb(pop_lsb(bs), occ);
        t.realAttacks[c][BISHOP] = bb;

        bb = t.pawnAttacks[c];
        Bitboard rk = pos.pieces(c, ROOK, c, QUEEN);
        while (rk) bb |= Bitboards::rook_attacks_bb(pop_lsb(rk), occ);
        t.realAttacks[c][ROOK] = bb;

        t.realAttacks[c][QUEEN] = t.realAttacks[c][BISHOP] | t.realAttacks[c][ROOK];
        t.realAttacks[c][KING]  = Bitboards::king_attacks_bb(ksq);

        const Bitboard ka = t.realAttacks[c][KING];
        for (int pt = PAWN; pt <= QUEEN; ++pt) t.attackedBy[c][pt] = t.realAttacks[c][pt] | ka;
        t.attackedBy[c][KING] = ka;

        t.attackedByAll[c] = ka;
        for (int pt = PAWN; pt <= QUEEN; ++pt) t.attackedByAll[c] |= t.attackedBy[c][pt];

        // attackedBy2 is the "attacked by both sides" set used for king
        // pressure, and it is built from *realAttacks* - not from the
        // king-seeded attackedBy. Including the king's own reach made the two
        // colours disagree: a king on the back rank defends two squares of the
        // open file in front of it while its opponent's king defends only one
        // (the other being its own queen's square), so the side whose king sat
        // behind a queen was credited with a free double-attack and took the
        // full danger penalty. Worth -300cp on a symmetric board.
        for (int pt = PAWN; pt <= KING; ++pt)
            t.attackedBy2[c][pt] = t.realAttacks[c][pt] & t.realAttacks[them][pt];

        // Mobility area: not occupied by our pawns, and not blocked by them.
        Bitboard blocked = 0;
        Bitboard p = pos.pieces(c, PAWN);
        const int sh = (c == WHITE) ? int(NORTH) : int(SOUTH);
        while (p) {
            Square s = pop_lsb(p);
            blocked |= Bitboards::shift_bb(sh, Bitboards::square_bb(s));
            blocked |= Bitboards::pawn_attacks_bb(c, Bitboards::square_bb(s));
        }
        t.mobilityArea[c] = ~(blocked | pos.pieces(c, PAWN) | pos.pieces(c, QUEEN, c, KING));

        Bitboard kr = ka & ~Bitboards::square_bb(ksq);
        t.kingRing[c] = kr | Bitboards::shift_bb(int(NORTH), kr) | Bitboards::shift_bb(int(SOUTH), kr)
                      | Bitboards::shift_bb(int(EAST), kr) | Bitboards::shift_bb(int(WEST), kr);
    }
}

// Everything about one colour. Never call this "us" - take the difference of the
// two colours at the call site, so that no term can favour the side to move.
struct SideScore {
    int mg = 0;
    int eg = 0;
    int phase = 0;
    // King-attack units by attacking piece type; filled in only when tracing.
    int uKnight = 0, uBishop = 0, uRook = 0, uQueen = 0;
};

// Per-section contributions, filled in only when tracing.
struct TermRow { const char* name; int mg; int eg; };
std::vector<TermRow> g_rows;

} // namespace

// ---------------------------------------------------------------------------
// Main evaluation
// ---------------------------------------------------------------------------

Value Eval::evaluate(const Position& pos) {
    Tables t;
    build(pos, t);

    const Color us = pos.side_to_move();
    const Color them = ~us;
    const U64 occ = pos.pieces();

    auto sideScore = [&](Color c) {
        const Color opp = ~c;
        SideScore r;
        auto add = [&](int mgv, int egv) { r.mg += mgv; r.eg += egv; };

        // When tracing, accumulate what each *section* contributed so that
        // comparing the two colours names the offending term. A whole-score
        // comparison cannot: a constant colour bias cancels out of
        // "side-to-move minus other side" being seen as symmetric.
        std::vector<TermRow>* rows = trace_enabled ? &g_rows : nullptr;
        int prevMg = 0, prevEg = 0;
        auto section = [&](const char* name) {
            if (rows) rows->push_back({name, r.mg - prevMg, r.eg - prevEg});
            prevMg = r.mg;
            prevEg = r.eg;
        };

        // ---- material + piece-square ----
        for (int p = PAWN; p <= QUEEN; ++p) {
            const PieceType pt = PieceType(p);
            Bitboard bb = pos.pieces(c, pt);
            while (bb) {
                const Square s = pop_lsb(bb);
                const Piece pc = make_piece(c, pt);
                // The PSQT is indexed by piece *type*, with the square mirrored
                // for Black. Indexing it by Piece (i.e. per colour) is what left
                // every Black entry zero.
                const Square v = (c == WHITE) ? s : flip_sq(s);
                r.mg += W.mg_value[pc] + W.mg_psqt[pt][v];
                r.eg += W.eg_value[pc] + W.eg_psqt[pt][v];
                r.phase += mg_phase(pt);
            }
        }
        section("material+psqt");

        const Bitboard freeSquares = ~pos.pieces(c);

        // ---- mobility ----
        // `realAttacks` excludes the king's reach, and each piece subtracts a
        // baseline so an average position contributes roughly nothing instead of
        // a constant proportional to the piece count.
        for (int p = KNIGHT; p <= KING; ++p) {
            const PieceType pt = PieceType(p);
            const int n = pos.count(c, pt);
            if (!n) continue;
            const int free = popcount(t.mobilityArea[c] & t.realAttacks[c][pt] & freeSquares);
            const int k = free - W.mg_mobility_base[pt] * n;
            const int e = free - W.eg_mobility_base[pt] * n;
            switch (pt) {
            case KNIGHT: add(W.mg_knight_mobility * k, W.eg_knight_mobility * e); break;
            case BISHOP: add(W.mg_bishop_mobility * k, W.eg_bishop_mobility * e); break;
            case ROOK:   add(W.mg_rook_mobility   * k, W.eg_rook_mobility   * e); break;
            case QUEEN:  add(W.mg_queen_mobility  * k, W.eg_queen_mobility  * e); break;
            default:     add(W.mg_king_mobility   * k, W.eg_king_mobility   * e); break;
            }
        }
        section("mobility");

        // ---- threats ----
        {
            // Enemy pieces that are attacked by us and defended by neither of us
            // nor their own pawns.
            const Bitboard weak = pos.pieces(opp)
                                & ~t.attackedBy[opp][QUEEN]
                                & freeSquares
                                & ~pos.pieces(c, PAWN);
            add(W.mg_threat_by_knight * popcount(t.realAttacks[c][KNIGHT] & weak), 0);
            add(W.mg_threat_by_bishop * popcount(t.realAttacks[c][BISHOP] & weak), 0);
            add(W.mg_threat_by_rook   * popcount(t.realAttacks[c][ROOK]   & weak), 0);
            add(W.mg_threat_by_queen  * popcount(t.realAttacks[c][QUEEN]  & weak), 0);
            add(W.mg_threat_by_safe_pawn * popcount(t.pawnAttacks[c] & weak), 0);

            Bitboard hanging = pos.pieces(opp) & t.attackedBy[c][QUEEN]
                             & ~t.attackedBy[opp][QUEEN] & freeSquares;
            int h = 0;
            while (hanging) {
                const Square s = pop_lsb(hanging);
                h += W.mg_weak_queen / 10 * (int(type_of(pos.piece_on(s))) - 1);
            }
            add(h, h);
        }
        section("threats");

        // ---- pawn structure ----
        {
            const Bitboard ourPawns   = pos.pieces(c, PAWN);
            const Bitboard theirPawns = pos.pieces(opp, PAWN);
            Bitboard b = ourPawns;
            while (b) {
                const Square s = pop_lsb(b);
                const File f = file_of(s);
                const Rank r = relative_rank(c, s);

                const bool leftPawn   = (f > FILE_A) && (ourPawns & Bitboards::file_bb(File(int(f) - 1)));
                const bool rightPawn  = (f < FILE_H) && (ourPawns & Bitboards::file_bb(File(int(f) + 1)));
                const bool connected  = leftPawn || rightPawn;
                const bool doubled    = (ourPawns & Bitboards::shift_bb(
                                             Direction(c == WHITE ? NORTH : SOUTH),
                                             Bitboards::square_bb(s))) != 0;

                if (!connected) add(W.mg_isolated_pawn, W.eg_isolated_pawn);
                else          add(W.mg_connected_pawn, W.eg_connected_pawn);
                if (doubled)   add(W.mg_doubled_pawn,  W.eg_doubled_pawn);

                // Passed: no enemy pawn ahead of us on our file or a neighbour.
                //
                // Both the comparison and the bonus index must be taken in the
                // same frame. `r` below is a *relative* rank but `rr` walks
                // absolute ranks, so for Black the test looked at absolute rank
                // 1 only, found nothing, and declared all eight starting pawns
                // passed - worth 1532cp of pure noise. The bonus table is keyed
                // by White's perspective, so it too needs the mirrored square.
                bool passed = true;
                const int myRank = int(rank_of(s));
                for (File ff = File(std::max(int(FILE_A), int(f) - 1));
                         ff <= File(std::min(int(FILE_H), int(f) + 1)) && passed; ++ff) {
                    Bitboard ahead = 0;
                    for (Rank rr = RANK_1; rr <= RANK_8; ++rr) {
                        const bool isAhead = (c == WHITE) ? (int(rr) > myRank) : (int(rr) < myRank);
                        if (isAhead) ahead |= Bitboards::square_bb(make_square(ff, rr));
                    }
                    if (theirPawns & Bitboards::file_bb(ff) & ahead) passed = false;
                }
                if (passed) {
                    const Square vs = (c == WHITE) ? s : flip_sq(s);
                    add(W.mg_passed_pawn_bonus[vs], W.eg_passed_pawn_bonus[vs]);
                }
                if (r == RANK_7) add(W.mg_pawn_on_7th, W.eg_pawn_on_7th);
            }
        }
        section("pawns");

        // ---- pieces ----
        {
            const Square theirKing = pos.king_square(opp);

            if (pos.count(c, BISHOP) >= 2) add(W.mg_bishop_pair, W.eg_bishop_pair);

            Bitboard b = pos.pieces(c, ROOK);
            while (b) {
                const Square s = pop_lsb(b);
                const Bitboard file = Bitboards::file_bb(file_of(s));
                const bool ourPawn   = (pos.pieces(c, PAWN)   & file) != 0;
                const bool theirPawn = (pos.pieces(opp, PAWN) & file) != 0;
                if (!ourPawn && !theirPawn) add(W.mg_rook_open_file, W.eg_rook_open_file);
                else if (!ourPawn)          add(W.mg_rook_semi_open, W.eg_rook_semi_open);
                if (relative_rank(c, s) == RANK_7) add(W.mg_rook_on_7th, W.eg_rook_on_7th);
                if (Bitboards::distance(s, theirKing) <= 2) add(W.mg_rook_king_prox, W.eg_rook_king_prox);
            }

            b = pos.pieces(c, KNIGHT, c, BISHOP);
            while (b) {
                const Square s = pop_lsb(b);
                const bool isKnight = type_of(pos.piece_on(s)) == KNIGHT;

                // Outpost: defended by one of our pawns and out of reach of any
                // enemy pawn. `realAttacks` because our own king guarding the
                // square does not make it a pawn outpost.
                const bool defended     = (t.realAttacks[c][PAWN] & Bitboards::square_bb(s)) != 0;
                const bool safeFromPawn = (t.pawnAttacks[opp] & Bitboards::square_bb(s)) == 0;
                if (defended && safeFromPawn && relative_rank(c, s) >= RANK_4) {
                    if (isKnight) add(W.mg_knight_outpost, W.eg_knight_outpost);
                    else          add(W.mg_bishop_outpost, W.eg_bishop_outpost);
                }

                // Minor piece with one of our pawns directly behind it.
                const int back = (c == WHITE) ? int(SOUTH) : int(NORTH);
                if (pos.pieces(c, PAWN) & Bitboards::shift_bb(Direction(back), Bitboards::square_bb(s)))
                    add(W.mg_minor_behind_pawn, W.eg_minor_behind_pawn);

                // Strapped knight: every square it could move to holds our pawn.
                if (isKnight) {
                    const Bitboard att = Bitboards::knight_attacks_bb(s);
                    if (att && att == (att & pos.pieces(c, PAWN)))
                        add(W.mg_strapped_knight, W.eg_strapped_knight);
                }
                if (Bitboards::distance(s, theirKing) <= 2)
                    add(W.mg_bishop_king_prox, W.eg_bishop_king_prox);
            }

            b = pos.pieces(c, QUEEN);
            while (b) {
                const Square s = pop_lsb(b);
                if (Bitboards::distance(s, theirKing) <= 2)
                    add(W.mg_queen_king_prox, W.eg_queen_king_prox);
            }
        }
        section("pieces");

        // ---- king shelter, storm and attack pressure ----
        {
            const Square ksq = pos.king_square(c);

            int shelter = 0, storm = 0;
            for (File f = FILE_A; f <= FILE_H; ++f) {
                const Bitboard ourFile   = pos.pieces(c, PAWN)   & Bitboards::file_bb(f);
                const Bitboard theirFile = pos.pieces(opp, PAWN) & Bitboards::file_bb(f);
                if (ourFile) {
                    const Square nearest = (c == WHITE) ? Square(lsb(ourFile)) : Square(msb(ourFile));
                    const int d = Bitboards::rank_distance(nearest, rank_of(ksq)) - 1;
                    if (d >= 0 && d <= 2) shelter += 3 - d;
                }
                if (theirFile) {
                    const Square nearest = (c == WHITE) ? Square(msb(theirFile)) : Square(lsb(theirFile));
                    const int d = Bitboards::rank_distance(nearest, rank_of(ksq)) - 1;
                    if (d >= 0 && d <= 3) storm += 3 - d;
                }
            }
            add(W.mg_shelter_bonus * shelter, W.eg_shelter_bonus * shelter);
            add(W.mg_storm_penalty * storm, W.eg_storm_penalty * storm);
            section("king.shelter+storm");

            // Attack pressure on OUR king: count enemy *pieces* whose attack
            // set reaches our king zone, weighted by type.
            //
            // An earlier version counted squares in the zone that were attacked
            // by the enemy *and* by us ("attacked by both sides"), a set that is
            // cheaper but not obviously symmetric - and it was not: it read 126
            // units for White and 0 for Black on a board that differs from its
            // own mirror in nothing at all. Counting pieces that attack our zone
            // depends only on our king's square and the enemy's attack sets, so
            // mirroring the position necessarily mirrors the count.
            const Bitboard zone = t.kingRing[c];
            r.uKnight = 0;
            r.uBishop = 0;
            r.uRook = 0;
            r.uQueen = 0;
            {
                Bitboard b;
                b = pos.pieces(opp, KNIGHT);
                while (b) { if (Bitboards::knight_attacks_bb(pop_lsb(b)) & zone) ++r.uKnight; }
                b = pos.pieces(opp, BISHOP);
                while (b) { if (Bitboards::bishop_attacks_bb(pop_lsb(b), occ) & zone) ++r.uBishop; }
                b = pos.pieces(opp, ROOK);
                while (b) { if (Bitboards::rook_attacks_bb(pop_lsb(b), occ) & zone) ++r.uRook; }
                b = pos.pieces(opp, QUEEN);
                while (b) {
                    const Square s = pop_lsb(b);
                    if ((Bitboards::bishop_attacks_bb(s, occ) | Bitboards::rook_attacks_bb(s, occ)) & zone)
                        ++r.uQueen;
                }
            }
            const int units = r.uKnight * W.king_attacker_knight
                            + r.uBishop * W.king_attacker_bishop
                            + r.uRook   * W.king_attacker_rook
                            + r.uQueen  * W.king_attacker_queen
                            // A nearby enemy queen is worth double.
                            + (r.uQueen && Bitboards::distance(pos.king_square(opp), ksq) <= 2
                                   ? r.uQueen * W.king_attacker_queen : 0);

            if (units > 0) {
                const int danger = std::min(units * units * W.king_danger_scale / (100 * 100),
                                            W.king_danger_max);
                add(-danger, -danger / 4);
            }
            section("king.danger");
        }

        // ---- Aurora SPARK --------------------------------------------------
        // "Pressure we have created": enemy pieces that are defended by their own
        // side yet attacked by two or more of ours. Rewards threats that survive
        // the current search rather than ones that simply hang. Bounded.
        if (W.spark_mg != 0 || W.spark_eg != 0) {
            int spark = 0;
            Bitboard theirs = pos.pieces(opp);
            while (theirs) {
                const Square s = pop_lsb(theirs);
                if (!(t.attackedBy[c][QUEEN] & Bitboards::square_bb(s))) continue;
                int na = popcount(Bitboards::bishop_attacks_bb(s, occ)
                                  & (pos.pieces(c, BISHOP, c, QUEEN)))
                       + popcount(Bitboards::rook_attacks_bb(s, occ)
                                  & (pos.pieces(c, ROOK, c, QUEEN)))
                       + popcount(Bitboards::knight_attacks_bb(s) & pos.pieces(c, KNIGHT));
                if (na >= 2 && (t.attackedBy[opp][QUEEN] & Bitboards::square_bb(s)))
                    spark += na;
            }
            spark = std::min(spark, 12);
            add(spark * W.spark_mg, spark * W.spark_eg);
        }
        section("spark");

        return r;
    };

    g_rows.clear();
    const SideScore a = sideScore(us);
    const std::size_t rowsPerSide = g_rows.size();
    const SideScore b = sideScore(them);

    if (trace_enabled) {
        // A healthy position has sideScore(us) == sideScore(them) for a
        // symmetric board. Anything else names the asymmetry directly, which no
        // end-to-end score comparison can do: a *constant* colour offset
        // cancels out of "score when White moves + score when Black moves".
        std::fprintf(stderr,
                     "  side-to-move : mg %7d  eg %7d  phase %2d\n"
                     "  opponent     : mg %7d  eg %7d  phase %2d\n"
                     "  difference   : mg %7d  eg %7d\n\n"
                     "  %-16s %10s %10s %10s\n",
                     a.mg, a.eg, a.phase, b.mg, b.eg, b.phase, a.mg - b.mg, a.eg - b.eg,
                     "section", "us mg", "opp mg", "diff mg");
        for (std::size_t i = 0; i < rowsPerSide && i < g_rows.size() / 2; ++i) {
            const TermRow& usr = g_rows[i];
            const TermRow& opr = g_rows[i + rowsPerSide];
            std::fprintf(stderr, "  %-16s %10d %10d %10d\n",
                         usr.name, usr.mg, opr.mg, usr.mg - opr.mg);
        }
        std::fprintf(stderr,
                     "\n  king-attack units  us: N %d B %d R %d Q %d  (total %d)\n"
                     "                     opp: N %d B %d R %d Q %d  (total %d)\n",
                     a.uKnight, a.uBishop, a.uRook, a.uQueen,
                     a.uKnight + a.uBishop + a.uRook + a.uQueen,
                     b.uKnight, b.uBishop, b.uRook, b.uQueen,
                     b.uKnight + b.uBishop + b.uRook + b.uQueen);
    }

    int mg = a.mg - b.mg;
    int eg = a.eg - b.eg;

    // Game phase belongs to the *position*, not to the side to move.
    //
    // This read the mover's phase alone, which is fine only while both sides
    // have comparable material. In a lopsided position the two phases diverge
    // and the same board was tapered differently depending on whose turn it was:
    // K against K+Q, White to move, got phase 0 and therefore pure endgame
    // weights, while Black to move got phase 8 and a third of the middlegame
    // table. The two evaluations of one board then disagreed by 39cp, found by
    // sweeping a set of lopsided positions and checking that the score with
    // each side to move *sums* to 2*tempo.
    //
    // Averaging the two sides makes the phase identical for a given board no
    // matter who is to move, which is the property the taper actually needs.
    const int phase = std::min((a.phase + b.phase) / 2, PHASE_MAX);

    // ---- tempo ----
    // Applied *after* the taper, not before it. Adding it to the middlegame
    // accumulator means the taper multiplies it by phase, so in a pure endgame
    // (phase 0) the tempo bonus silently vanished - the engine stopped valuing
    // the right to move exactly when the position was decided.
    // ---- taper ----
    int score = (mg * phase + eg * (PHASE_MAX - phase)) / PHASE_MAX;
    score = (score * W.mg_scale) / 100;
    score += W.tempo_bonus;

    // ---- Aurora NOISE -------------------------------------------------
    // A deterministic dither derived from the position key, applied once to the
    // final score. Not random: the same position always evaluates identically,
    // so search stays reproducible and A/B measurements remain meaningful. Its
    // purpose is to break ties between otherwise-equal moves so the engine does
    // not collapse onto one arbitrary choice.
    if (W.noise_max != 0) {
        const int n = int((pos.key() >> 11) & 0xF) - 8;   // -8..7
        score += n * W.noise_max / 8;
    }

    return Value(std::clamp(score, -VALUE_KNOWN_WIN + 1, VALUE_KNOWN_WIN - 1));
}

int Eval::scale_factor(int cp) { return cp; }

} // namespace Aurora
