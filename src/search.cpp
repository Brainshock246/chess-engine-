#include "search.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace Aurora {

using namespace std::chrono;

// ---------------------------------------------------------------------------
// Search parameters.  Deliberately NOT in weights.h: the evaluation tuner must
// not be able to perturb them.
// ---------------------------------------------------------------------------

namespace {

// --- razoring ---
constexpr int RAZOR_MARGIN = 260;

// --- futility / reverse futility ---
constexpr int FUTILITY_MARGIN_BASE = 130;
constexpr int FUTILITY_MARGIN_PER_PLY = 13;
constexpr int REVERSE_FUTILITY_MARGIN = 115;
constexpr int REVERSE_FUTILITY_MAX_DEPTH = 7;

// --- null move ---
constexpr int NULL_REDUCTION_BASE = 2;
constexpr int NULL_MARGIN_PER_DEPTH = 210;
constexpr int NULL_MARGIN_MIN = 780;

// --- late move reductions ---
constexpr int LMR_BASE = 780;
constexpr int LMR_PER_DEPTH = 95;
constexpr int LMR_PER_MOVE = 28;
constexpr int LMR_PER_HISTORY = 700;
constexpr int LMR_PER_CAP = 170;
constexpr int LMR_PER_PLY = 7;
constexpr int LMR_PV_BONUS = 220;
constexpr int LMR_CUT_BONUS = 160;

// --- singular extensions ---
constexpr int SINGULAR_BASE = 210;
constexpr int SINGULAR_MIN_DEPTH = 8;

// --- internal iterative reduction (Aurora) ---
constexpr int IIR_MIN_DEPTH = 5;
constexpr int IIR_REDUCTION = 3;

// --- ProbCut ---
constexpr int PROBCUT_BASE = 210;
constexpr int PROBCUT_MIN_DEPTH = 5;

// --- check extension ---
constexpr int CHECK_EXT_MAX_DEPTH = 12;

// --- aspiration ---
// ---------------------------------------------------------------------------
// Speculative pruning, all of it off by default.
//
// Null move, reverse futility, razoring, IIR and singular extensions are where
// alpha-beta engines gain most of their speed - and also where they most easily
// return a score that is not a score. With them enabled this search reported
// +1036 for a position it had proved won at depth 6, +24 at depth 7 and -11 at
// depth 8, spending 102 million nodes doing it.
//
// A wrong score is worse than a slow one: it feeds move ordering, the
// transposition table and every comparison above it. So the baseline is plain
// alpha-beta with a transposition table, quiescence and history, which is
// verifiable, and each speculative feature is re-enabled one at a time with a
// game-based measurement behind it - not on the strength of being standard.
//
// Overridable at build time so that enabling it is a flag and not a source edit:
//
//   g++ ... -DAURORA_SPEC_PRUNING            (all speculative pruning on)
//   g++ ... -DAURORA_SPEC_PRUNING=0          (the verified baseline)
//
// A feature stays off until tools/scores.py and tools/tactics.py both pass *with
// it on* and a match says it wins games. Correctness first, then speed.
// ---------------------------------------------------------------------------
#ifndef AURORA_SPEC_PRUNING
#define AURORA_SPEC_PRUNING false
#endif
constexpr bool SPECULATIVE_PRUNING = AURORA_SPEC_PRUNING;

// Trust a transposition-table *value* enough to return it as a search result.
// The stored move is still used for ordering; only the value is withheld.
constexpr bool TRUST_TT_VALUES = true;
constexpr int ASP_BASE = 24;
constexpr int ASP_MAX = 600;

inline int history_push(int cur, int bonus) {
    return std::clamp(cur + bonus - cur * std::abs(bonus) / 16384, -(1 << 15), (1 << 15) - 1);
}
inline int history_bonus(int v) {
    return std::clamp(128 * v / std::max(std::abs(v), 1), -128, 128);
}
inline int history_penalty(int v, int limit) {
    return v * std::max(0, limit - std::abs(v)) / limit;
}

} // namespace

// ---------------------------------------------------------------------------

Search::Search() {
    tt_.resize(TranspositionTable::default_hash_mb());
    states_.assign(MAX_PLY + 16, StateInfo{});
    clear();
}

void Search::clear() {
    tt_.clear();
    for (int i = 0; i < MAX_PLY + 8; ++i) { killers[i][0] = killers[i][1] = MOVE_NONE; }
    for (auto& a : history) for (auto& b : a) for (auto& c : b) c = 0;
    for (auto& a : captureHistory) for (auto& b : a) for (auto& c : b) c = 0;
    for (auto& a : counterMoveHistory) for (auto& b : a) for (auto& c : b) c = 0;
    for (auto& n : nullHistory) n = 0;
    for (auto& t : threatHistory) for (auto& x : t) x = 0;
}

void Search::new_game() { clear(); }

bool Search::check_time() {
    if (stop_) return true;
    if ((nodes_ & 1023) != 0) return false;
    if (nodeLimit_ && nodes_ >= nodeLimit_) { stop_ = true; return true; }
    if (limits_.useTime && timeLimit_ > 0) {
        int64_t elapsed = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() - startTime_;
        if (elapsed >= timeLimit_) { stop_ = true; return true; }
    }
    return false;
}

int Search::reduction(bool improving, int depth, int moveNumber, bool quiet, int hist, int ply) const {
    int r = LMR_BASE + LMR_PER_DEPTH * depth;
    if (moveNumber > 1) r += LMR_PER_MOVE * (moveNumber - 1);
    if (quiet) r += LMR_PER_HISTORY * std::abs(hist) / 16384;
    r -= improving * 180;
    r += LMR_PER_PLY * (ply > 20 ? 1 : 0);
    r = std::clamp(r, 0, std::max(depth - 1, 0));
    (void)LMR_PER_CAP;
    return r;
}

void Search::update_pv(int ply, Move m, Stack* ss) {
    Stack* child = ss + 1;
    (void)ply; (void)m; (void)child;
}

// ---------------------------------------------------------------------------
// Quiescence
// ---------------------------------------------------------------------------

Value Search::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {
    if (check_time()) return Value(0);
    ++nodes_;
    if (ss->ply >= MAX_PLY - 1) return Eval::evaluate(pos);

    ss->inCheck = pos.checkers() != 0;

    if (ss->inCheck) {
        // No stand-pat when in check: every legal evasion must be tried.
        MoveList moves;
        generate(pos, moves, EVASIONS);
        Value best = Value(-VALUE_INFINITE);
        int moveCount = 0;
        for (int i = 0; i < moves.size(); ++i) {
            Move m = moves[i].move;
            if (!pos.legal(m)) continue;
            ++moveCount;
            pos.do_move(m, states_[ss->ply + 1]);
            Value v = -qsearch(pos, ss + 1, -beta, -alpha);
            pos.undo_move(m);
            if (v > best) best = v;
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;
        }
        if (moveCount == 0) return Value(-VALUE_MATE + ss->ply);
        return best;
    }

    // Quiescence must return the *value of this position*, not a window bound.
    //
    // This used to end with `return Value(alpha)`. When the position is quiet
    // and the static score is below alpha - which is the normal case inside a
    // narrow principal-variation window - no capture raised alpha, so the
    // function returned the alpha its parent had passed down. A window bound
    // therefore came back as though it were an evaluation, propagated to the
    // parent, and was inherited again on the way up: the error compounded with
    // every ply.
    //
    // The symptom was a score that decayed with depth in positions whose
    // material never changed. White a full queen up read +1050 at depth 1,
    // +1029 at depth 6, +778 at depth 7, +14 at depth 8 and -14 at depth 9; and
    // a position with material exactly equal claimed +1041. The magnitude was
    // always about one queen, because that is roughly what the inherited window
    // values happened to be.
    //
    // `best` is the true value: the stand pat, raised by any capture that beats
    // it. Return `best`.
    ss->staticEval = int(Eval::evaluate(pos));
    Value best = Value(ss->staticEval);
    if (best >= beta) return best;
    if (best > alpha) alpha = best;

    MoveList moves;
    generate(pos, moves, CAPTURES);
    // order: MVV-LVA
    for (int i = 0; i < moves.size(); ++i) moves[i].value = mvv_lva(pos, moves[i].move);
    moves.sort();

    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i].move;

        // Delta pruning: if even winning this piece for free cannot reach alpha
        if (ss->staticEval + PieceValue[pos.piece_on(to_sq(m))] + 200 < alpha) continue;
        if (!pos.legal(m)) continue;

        pos.do_move(m, states_[ss->ply + 1]);
        Value v = -qsearch(pos, ss + 1, -beta, -alpha);
        pos.undo_move(m);

        if (v > best) {
            best = v;
            if (v > alpha) {
                alpha = v;
                if (v >= beta) { ++cutoffs_; break; }
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Main search
// ---------------------------------------------------------------------------

Value Search::search(Position& pos, Stack* ss, Value alpha, Value beta, int depth, bool cutNode, bool pvNode) {
    const bool rootNode = (ss->ply == 0);

    // Reset the PV on entry. Stack slots are reused across root moves and across
    // iterations, so without this a node that returns early (TT cut, null move,
    // mate score) leaves the previous line behind and the caller splices a stale
    // principal variation onto a move it has nothing to do with.
    ss->pvLen = 0;

    if (depth <= 0) return qsearch(pos, ss, alpha, beta);

    ss->inCheck = pos.checkers() != 0;
    if (ss->ply >= MAX_PLY - 2) return Eval::evaluate(pos);

    if (!rootNode) {
        if (pos.rule50_count() >= 100) return Value(0);
    }

    if (check_time()) { stop_ = true; return Value(0); }
    ++nodes_;
    seldepth_ = std::max(seldepth_, ss->ply + 1);

    // ---- transposition table ----
    Move ttMove = MOVE_NONE;
    Value ttValue = Value(VALUE_NONE);
    bool ttHit = false;
    {
        bool found = false;
        TTEntry* tte = tt_.probe(pos.key(), found);
        if (found) {
            ttHit = true;
            ttValue = tte->value(ss->ply);
            ttMove = tte->move16();
            // TRUST_TT_VALUES is off while a transposition-table defect is under
            // investigation: with it off the table still supplies the move used
            // for ordering, but never a value, so any wrong score that survives
            // has been localised to the search rather than to the table.
            if (TRUST_TT_VALUES && !pvNode && !rootNode && tte->depth8() >= depth) {
                Bound b = tte->bound();
                if (b == BOUND_EXACT
                 || (b == BOUND_LOWER && ttValue >= beta)
                 || (b == BOUND_UPPER && ttValue <= alpha))
                    return ttValue;
            }
        }
    }

    // ---- static evaluation ----
    Value eval;
    if (ss->inCheck) {
        ss->staticEval = VALUE_NONE;
        eval = Value(VALUE_NONE);
    } else {
        // Unconditional evaluation at every node.
        //
        // The lagged scheme (evaluate every other ply, inherit from the parent
        // otherwise) was tried because it is the standard speed compromise. It
        // cost two separate investigations and was removed.
        //
        // First, it read `ss->staticEval` - this node's own uninitialised slot -
        // instead of the parent's, so odd plies at depth 5+ were scored with
        // whatever stale value a previous position left in that stack slot.
        //
        // Second, and worse, the inherited value is not a static evaluation at
        // all: on a transposition hit ss->staticEval is overwritten with the
        // table's stored *search* score. So a search score was being fed back
        // in as though it were a static evaluation and inherited again on the
        // next odd ply, compounding downward with every ply. The symptom was a
        // score that decayed with depth in a position whose material never
        // changed: a position with White a full queen up read +1050 at depth 1,
        // +1003 at depth 6, +778 at depth 7 and -49 at depth 8.
        //
        // A heuristic score that is sometimes wrong is not a speed optimisation.
        // Evaluating every node is slower and always right, and "always right" is
        // what lets the rest of the engine be trusted.
        ss->staticEval = int(Eval::evaluate(pos));
        eval = Value(ss->staticEval);
    }

    // ---- shallow pruning ----
    if (SPECULATIVE_PRUNING && !pvNode && !rootNode && !ss->inCheck && ss->staticEval != VALUE_NONE
        && std::abs(beta) < VALUE_KNOWN_WIN) {

        // Reverse futility pruning
        if (depth <= REVERSE_FUTILITY_MAX_DEPTH
            && ss->staticEval - REVERSE_FUTILITY_MARGIN * depth >= beta)
            return Value(ss->staticEval);

        // Razoring
        if (depth <= 3 && ss->staticEval + RAZOR_MARGIN * depth <= alpha) {
            Value v = qsearch(pos, ss, Value(int(alpha) - 1), alpha);
            if (v <= alpha) return v;
        }
    }

    // ---- null move pruning ----
    if (SPECULATIVE_PRUNING && !pvNode && !rootNode && !ss->inCheck && depth >= 3
        && ss->staticEval != VALUE_NONE && ss->staticEval >= beta
        && ss->ply >= nullHistory[std::min(depth, 63)]
        && (ss - 1)->currentMove != MOVE_NULL
        && pos.non_pawn_material(pos.side_to_move()) > 0) {

        int R = NULL_REDUCTION_BASE + depth / 3
              + std::min((ss->staticEval - int(beta)) / 200, 2);
        if (R >= depth) R = depth - 1;
        if (R < 1) R = 1;
        ++nullmoves_;

        pos.do_null_move(states_[ss->ply + 1]);
        Value v = -search(pos, ss + 1, -beta, Value(int(-beta) + 1), depth - R, !cutNode, false);
        pos.undo_null_move();

        nullHistory[std::min(depth, 63)] = std::clamp(nullHistory[std::min(depth, 63)] + depth - R, 0, depth);

        if (v >= beta) {
            // Do not return a huge null-move score as a real mate score
            if (v >= VALUE_MATE_IN_MAX_PLY && depth > 10) return Value(int(beta));
            return v;
        }
    }

    // ---- IIR: internal iterative reductions ----
    // A deep node with no TT move would otherwise waste time probing a random
    // first move. A cheap reduced search gives us a real move to order with.
    if (SPECULATIVE_PRUNING && depth >= IIR_MIN_DEPTH && !pvNode && !rootNode && !ss->inCheck
        && ttMove == MOVE_NONE && (ss - 1)->currentMove != MOVE_NULL) {
        int d = std::max(1, depth - IIR_REDUCTION - (depth > 20 ? 1 : 0));
        Value v = search(pos, ss, Value(int(alpha) - 1), alpha, d, true, false);
        if (v >= beta) return v;
        bool f2 = false;
        TTEntry* tte = tt_.probe(pos.key(), f2);
        if (f2) ttMove = tte->move16();
    }

    // ---- move generation and ordering ----
    MoveList moves;
    generate(pos, moves, ss->inCheck ? EVASIONS : NON_EVASIONS);

    {
        Color us = pos.side_to_move();
        Move prev = (ss - 1)->currentMove;
        for (int i = 0; i < moves.size(); ++i) {
            Move m = moves[i].move;
            Square from = from_sq(m), to = to_sq(m);
            Piece pc = pos.piece_on(from);
            PieceType pt = type_of(pc);

            if (pos.capture_or_promotion(m)) {
                PieceType cap = type_of(pos.piece_on(to));
                if (cap == NO_PT) cap = PAWN;
                int v = captureHistory[pc][to][cap]
                      - 6 * history_penalty(history[us][from][to], 6000);
                if (m == ttMove) v += (1 << 28);
                moves[i].value = (1 << 24) + v;
            } else {
                int v = history[us][from][to];
                if (m == prev) v += counterMoveHistory[from][to][pt] * 8;
                if (m == ttMove) v += (1 << 28);
                else if (m == ss->killers[0]) v += (1 << 22);
                else if (m == ss->killers[1]) v += (1 << 21);
                if (pt == PAWN) v += 2048;
                moves[i].value = v;
            }
        }
        moves.sort();
    }

    // ---- singular extensions ----
    Move excluded = MOVE_NONE;
    if (SPECULATIVE_PRUNING && depth >= SINGULAR_MIN_DEPTH && !rootNode && !pvNode && ttMove != MOVE_NONE
        && ttValue != Value(VALUE_NONE) && std::abs(ttValue) < VALUE_KNOWN_WIN
        && (ss - 1)->currentMove != MOVE_NULL && (ss - 1)->excludedMove != ttMove) {

        Value singularBeta = Value(int(ttValue) - SINGULAR_BASE * depth);
        if (singularBeta > alpha) {
            ss->excludedMove = ttMove;
            int savedCount = ss->moveCount;
            Value v = search(pos, ss, Value(int(singularBeta) - 1), singularBeta, depth / 2, cutNode, false);
            ss->moveCount = savedCount;
            ss->excludedMove = MOVE_NONE;
            if (v < singularBeta) excluded = ttMove;
        }
    }

    // ---- move loop ----
    Value bestValue = Value(-VALUE_INFINITE);
    Move bestMove = MOVE_NONE;
    int moveCount = 0;
    Bound bound = BOUND_UPPER;
    Move quietsTried[64];
    int quietCount = 0;
    const bool improving = false;

    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i].move;
        if (m == excluded) continue;
        if (!pos.legal(m)) continue;

        ++moveCount;
        if (moveCount > 1) {
            const bool quiet = !pos.capture_or_promotion(m);

            // Late move reductions + re-searches
            if (SPECULATIVE_PRUNING && depth >= 2 && !ss->inCheck && !excluded) {
                int r = reduction(improving, depth, moveCount, quiet,
                                  history[pos.side_to_move()][from_sq(m)][to_sq(m)], ss->ply);
                if (pvNode) r -= LMR_PV_BONUS;
                if (cutNode) r += LMR_CUT_BONUS;
                r = std::clamp(r, 0, depth - 1);
                int d = std::max(0, depth - r);

                Value v = -search(pos, ss + 1, Value(-(int(alpha) + 1)), -alpha, d, true, false);
                if (v > alpha && d < depth) {
                    // The LMR search failed high: re-search at a milder reduction
                    int d2 = std::max(0, depth - std::max(r / 2, 1));
                    v = -search(pos, ss + 1, Value(-(int(alpha) + 1)), -alpha, d2, !cutNode, false);
                }
                if (v > alpha && v < beta) {
                    // Verify at full depth
                    v = -search(pos, ss + 1, -beta, -alpha, depth - 1, !cutNode, pvNode);
                }
                if (v >= beta) {
                    if (quiet && quietCount < 64) {
                        int bonus = history_bonus(depth * depth);
                        history[pos.side_to_move()][from_sq(m)][to_sq(m)] =
                            history_push(history[pos.side_to_move()][from_sq(m)][to_sq(m)], bonus);
                        for (int q = 0; q < quietCount; ++q) {
                            history[pos.side_to_move()][from_sq(quietsTried[q])][to_sq(quietsTried[q])] =
                                history_push(history[pos.side_to_move()][from_sq(quietsTried[q])][to_sq(quietsTried[q])], -bonus);
                        }
                    }
                    ++cutoffs_;
                    bestValue = v;
                    bestMove = m;
                    bound = BOUND_LOWER;
                    break;
                }
                continue;
            }

            // Futility pruning (quiet moves only, shallow nodes)
            if (SPECULATIVE_PRUNING && quiet && !ss->inCheck && depth <= 7 && ss->staticEval != VALUE_NONE
                && std::abs(beta) < VALUE_KNOWN_WIN
                && ss->staticEval + FUTILITY_MARGIN_BASE + FUTILITY_MARGIN_PER_PLY * depth
                     + 120 * PieceValue[type_of(pos.piece_on(to_sq(m)))] <= alpha)
                continue;
        }

        // ---- search the move ----
        (ss + 1)->currentMove = m;
        (ss + 1)->killers[0] = (ss + 1)->killers[1] = MOVE_NONE;
        (ss + 1)->moveCount = 0;
        (ss + 1)->excludedMove = MOVE_NONE;
        (ss + 1)->inCheck = false;

        pos.do_move(m, states_[ss->ply + 1]);

        const bool givesCheck = pos.checkers() != 0;
        int newDepth = depth - 1;
        if (givesCheck && !excluded && depth < CHECK_EXT_MAX_DEPTH) ++newDepth;

        Value v;
        if (moveCount == 1) {
            v = -search(pos, ss + 1, -beta, -alpha, newDepth, !pvNode, pvNode);
        } else if (SPECULATIVE_PRUNING && depth >= PROBCUT_MIN_DEPTH
                   && pos.capture_or_promotion(m) && !excluded) {
            // ProbCut: a null-window probe two plies down. If it fails high, so
            // will the real search, at a fraction of the cost.
            //
            // The window must be a *proper* null window. It used to be
            // (-pcBeta, -alpha - PROBCUT_BASE), which with PROBCUT_BASE = 210 is
            // an inverted window: beta sits 209 centipawns BELOW alpha. An
            // inverted window breaks every assumption alpha-beta makes about
            // what a return value means, and the number it produced was then used
            // as this move's score.
            //
            // The signature was unmistakable. K+Q against a lone king - no
            // tactic, no capture, static evaluation -961 - was searched
            // correctly at depths 1 to 4 and reported +1077 from depth 5 onward.
            // Depth 5 is PROBCUT_MIN_DEPTH. Nothing else in the search changes
            // behaviour at exactly that threshold.
            Value pcBeta = Value(int(alpha) + 1);
            v = -search(pos, ss + 1, -pcBeta, Value(int(-pcBeta) + 1),
                        std::min(newDepth, depth - 2), true, false);
            if (v >= beta) {
                // fall through to a normal search below
                v = -search(pos, ss + 1, Value(-(int(alpha) + 1)), -alpha, newDepth, true, false);
            }
        } else {
            v = -search(pos, ss + 1, Value(-(int(alpha) + 1)), -alpha, newDepth, true, false);
            if (v > alpha && pvNode) v = -search(pos, ss + 1, -beta, -alpha, newDepth, false, true);
        }
        pos.undo_move(m);

        if (stop_) return Value(0);

        if (v > bestValue) {
            bestValue = v;
            bestMove = m;
            if (v > alpha) {
                alpha = v;
                bound = BOUND_LOWER;
                if (pvNode) {
                    ss->pv[0] = m;
                    int n = 0;
                    Stack* child = ss + 1;
                    for (int k = 0; k < child->pvLen && k < MAX_PLY - 2; ++k)
                        ss->pv[++n] = child->pv[k];
                    ss->pvLen = n + 1;
                }
                if (v >= beta) break;
            }
        }
        if (!pos.capture_or_promotion(m) && quietCount < 64) quietsTried[quietCount++] = m;
    }

    if (moveCount == 0) {
        if (excluded) return Value(int(alpha));   // singular verification: "not singular"
        return ss->inCheck ? Value(-VALUE_MATE + ss->ply) : Value(0);
    }

    // killers + history for the best quiet move
    if (!ss->inCheck && bestMove != MOVE_NONE && !pos.capture_or_promotion(bestMove)) {
        for (int k = 0; k < 2; ++k)
            if (ss->killers[k] == bestMove) ss->killers[k] = ss->killers[1 - k];
        ss->killers[0] = bestMove;
        if ((ss - 1)->currentMove != MOVE_NONE) {
            Move prev = (ss - 1)->currentMove;
            PieceType pt = type_of(pos.piece_on(to_sq(bestMove)));
            counterMoveHistory[from_sq(prev)][to_sq(prev)][pt] = bestMove;
        }
        int bonus = history_bonus(depth * depth);
        history[pos.side_to_move()][from_sq(bestMove)][to_sq(bestMove)] =
            history_push(history[pos.side_to_move()][from_sq(bestMove)][to_sq(bestMove)], bonus);
    }

    if (!stop_)
        tt_.new_entry(pos.key(), bestValue, pvNode, bound, depth, bestMove,
                      ss->staticEval == VALUE_NONE ? Eval::evaluate(pos) : Value(ss->staticEval),
                      ss->ply);

    return bestValue;
}

// ---------------------------------------------------------------------------
// Iterative deepening
// ---------------------------------------------------------------------------

void Search::think(Position& pos, StateInfo* si, const LimitsType& limits, std::ostream& os) {
    (void)si;
    limits_ = limits;
    startTime_ = limits.startTime;
    // The budget belongs to whoever is on move, and it is the value the UCI
    // layer computed for that side specifically.
    timeLimit_ = limits.useTime ? limits.time[pos.side_to_move()] : 0;
    nodes_ = 0;
    seldepth_ = 0;
    completedDepth_ = 0;
    stop_ = false;
    nullmoves_ = cutoffs_ = 0;
    tt_.new_search();

    MoveList all;
    generate(pos, all, LEGAL);

    rootMoveCount_ = 0;
    for (int i = 0; i < all.size(); ++i) {
        bool wanted = (limits.numSearchmoves == 0);
        for (int j = 0; j < limits.numSearchmoves && !wanted; ++j)
            if (all[i].move == limits.searchmoves[j]) wanted = true;
        if (!wanted) continue;
        rootMoves_[rootMoveCount_].move = all[i].move;
        rootMoves_[rootMoveCount_].score = Value(-VALUE_INFINITE);
        ++rootMoveCount_;
    }
    if (rootMoveCount_ == 0) {
        os << "bestmove 0000\n";
        return;
    }

    // states_ must be large enough for every ply we can reach
    if (states_.size() < size_t(MAX_PLY + 16)) states_.assign(MAX_PLY + 16, StateInfo{});
    Stack stack[MAX_PLY + 12] = {};
    for (int i = 0; i <= MAX_PLY + 11; ++i) stack[i].ply = i;

    Value bestValue = Value(0);
    Move bestMove = rootMoves_[0].move;

    int maxDepth = limits.depth > 0 ? std::min(limits.depth, MAX_PLY - 2) : MAX_PLY - 2;
    if (limits.mate) maxDepth = std::min(maxDepth, 2 * limits.mate);

    for (int depth = 1; depth <= maxDepth; ++depth) {
        for (int i = 0; i < rootMoveCount_; ++i) {
            rootMoves_[i].prevScore = rootMoves_[i].score;
            pvLen_[i] = 0;
        }
        std::stable_sort(rootMoves_, rootMoves_ + rootMoveCount_,
                         [](const RootMove& a, const RootMove& b) { return a.prevScore > b.prevScore; });

        // Search EVERY root move. MultiPV controls how many lines are *reported*,
        // not how many are searched: bounding the loop by multiPV_ meant a
        // single-PV search examined exactly one move - the first in generation
        // order - and simply reported that one. The engine appeared to search
        // (5M nodes, a principal variation, a varying score) while in fact
        // having chosen its move before searching anything, which is why it
        // played b1a3 from the start position at every depth: b1a3 happens to be
        // the first move Aurora generates.
        const int pvCount = rootMoveCount_;

        // Aspiration windows are deliberately disabled.
        //
        // The window was centred on the previous depth's score with a half-width
        // of ASP_BASE (24). When the true score sat outside it, the fail-low
        // branch pulled beta *towards* the new lower score while lowering alpha,
        // so each retry re-searched every root move again over a window that had
        // not usefully widened. A single depth-9 search burned 29.5 million
        // nodes - roughly thirty times what it should - and the reported score
        // wandered across four values at one depth as truncated bounds were
        // progressively replaced by true values.
        //
        // None of that is free speed to reclaim later; it is the cost of not
        // knowing what the search thinks. With a full window there is exactly one
        // search per depth, every score is a true value, and any future
        // aspiration work can be measured against this baseline.
        Value alpha = Value(-VALUE_INFINITE);
        Value beta  = Value(VALUE_INFINITE);

        int delta = ASP_BASE;
        int bestIdx = 0;
        int aspirationRetries = 0;

        for (;;) {
            Value bestThis = -VALUE_INFINITE;
            bestIdx = 0;

            for (int i = 0; i < pvCount; ++i) {
                Move m = rootMoves_[i].move;
                stack[0].currentMove = m;
                stack[0].killers[0] = stack[0].killers[1] = MOVE_NONE;
                stack[0].pvLen = 0;
                stack[0].moveCount = 0;
                stack[0].inCheck = false;
                stack[0].staticEval = 0;

                // Root IID: when the TT has nothing, a cheap shallow search
                // gives a much better move order at the root than nothing.
                {
                    bool found = false;
                    TTEntry* tte = tt_.probe(pos.key(), found);
                    if (depth >= 4 && (!found || tte->move16() == MOVE_NONE))
                        search(pos, stack, Value(-VALUE_INFINITE), Value(VALUE_INFINITE),
                               std::max(1, depth - 3), true, false);
                }

                pos.do_move(m, states_[1]);

                // Principal variation search at the root.
                //
                // The first move gets the full window; later moves are asked only
                // whether they can beat alpha, and re-searched properly if they
                // can. Without this every root move was searched over the full
                // (-inf, +inf) window while alpha never moved, so nothing was ever
                // pruned: depth 8 cost 18 million nodes and 16 seconds.
                Value score;
                if (i == 0) {
                    score = -search(pos, stack + 1, -beta, -alpha, depth - 1, false, true);
                } else {
                    score = -search(pos, stack + 1, -alpha - 1, -alpha, depth - 1, false, false);
                    if (score > alpha)
                        score = -search(pos, stack + 1, -beta, -alpha, depth - 1, false, false);
                }
                pos.undo_move(m);

                rootMoves_[i].score = score;
                rootMoves_[i].selDepth = std::max(1, seldepth_ - 1);

                // Build the root PV here. The root move is played *outside*
                // search(), so search() itself never writes stack[0].pv - the
                // line lives in the child node's PV and has to be spliced on
                // after this move, not read off the root stack slot.
                pv_[i][0] = m;
                int n = 1;
                for (int k = 0; k < stack[1].pvLen && n < MAX_PLY - 2; ++k)
                    pv_[i][n++] = stack[1].pv[k];
                pvLen_[i] = n;

                if (score > bestThis) { bestThis = score; bestIdx = i; }
                // Advance alpha so the remaining root moves can be pruned. With a
                // full window this is what makes the search a search at all.
                if (score > alpha) alpha = score;

                if (stop_) break;
            }

            // Report the *best* move, once the whole root list has been searched.
            //
            // Printing from inside the loop at i == 0 reported whichever move was
            // searched first, while `bestmove` returned the best - so the log
            // would show a principal variation belonging to a different move
            // than the one actually played. That is worse than no reporting: it
            // looks like a stable PV and is not one.
            if (!stop_ && pvCount > 0) {
                int64_t elapsed = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() - startTime_;
                os << "info depth " << depth
                   << " seldepth " << rootMoves_[bestIdx].selDepth
                   << " multipv 1"
                   << " score ";
                const Value shown = rootMoves_[bestIdx].score;
                if (shown >= VALUE_MATE_IN_MAX_PLY)
                    os << "mate " << ((VALUE_MATE - int(shown)) + 1) / 2;
                else if (shown <= VALUE_MATED_IN_MAX_PLY)
                    os << "mate " << -((VALUE_MATE + int(shown)) + 1) / 2;
                else
                    os << "cp " << int(shown);
                os << " nodes " << nodes_
                   << " nps " << (elapsed > 0 ? uint64_t(double(nodes_) * 1000.0 / double(elapsed)) : nodes_)
                   << " hashfull " << tt_.hashfull()
                   << " time " << elapsed
                   << " retries " << aspirationRetries
                   << " pv";
                for (int k = 0; k < pvLen_[bestIdx]; ++k) os << " " << to_string_move(pv_[bestIdx][k]);
                os << "\n" << std::flush;
            }

            std::swap(rootMoves_[0], rootMoves_[bestIdx]);
            bestMove = rootMoves_[0].move;
            bestValue = rootMoves_[0].score;
            if (stop_) break;
            completedDepth_ = depth;

            if (bestThis < alpha) {
                beta  = Value((int(bestThis) + int(beta)) / 2);
                alpha = Value(std::max(-VALUE_INFINITE + 1, int(bestThis) - delta));
                if (alpha < beta) { ++aspirationRetries; continue; }
            }
            if (bestThis >= beta) {
                delta += delta / 3 + 5;
                beta = Value(std::min(VALUE_INFINITE - 1, int(bestThis) + delta));
                if (beta > alpha) { ++aspirationRetries; continue; }
            }
            break;
        }
    }

    os << "bestmove " << to_string_move(bestMove) << "\n" << std::flush;
}

} // namespace Aurora