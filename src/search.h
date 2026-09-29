// Aurora - search
//
// Structure:
//   qsearch   - quiescence, stand-pat, capture-only with delta pruning
//   search    - main alpha-beta node with the usual battery of pruning
//   think     - iterative deepening + aspiration windows + MultiPV
//
// Notable design choices that differ from a textbook engine:
//   * PRR  (Probabilistic Razoring Reduction) - see search()
//   * SPARK / NOISE live in the evaluation (eval.cpp)
//
// Aurora is a single-threaded engine. That is a deliberate choice: it keeps
// the search completely deterministic, which is what makes the A/B testing
// in tools/ meaningful. SMP is a strength feature, not a correctness one.

#pragma once
#include "position.h"
#include "tt.h"
#include "eval.h"
#include "movegen.h"
#include <vector>
#include <iostream>

namespace Aurora {

constexpr int MAX_PLY = 128;
constexpr int MAX_ROOT_MOVES = 256;

struct Stack {
    Move pv[MAX_PLY] = {};
    int pvLen = 0;
    Move currentMove = MOVE_NONE;
    Move excludedMove = MOVE_NONE;
    Move killers[2] = {MOVE_NONE, MOVE_NONE};
    int staticEval = 0;
    int moveCount = 0;
    int nullCount = 0;
    Bound bound = BOUND_NONE;
    bool inCheck = false;
    int ply = 0;
};

struct LimitsType {
    bool useTime = false;
    int64_t time[COLOR_NB] = {0, 0};
    int64_t inc[COLOR_NB] = {0, 0};
    int64_t movetime = 0;
    int64_t startTime = 0;
    int movesToGo = 0;
    Move searchmoves[256] = {};
    int numSearchmoves = 0;
    int depth = 0;
    int64_t nodes = 0;
    int mate = 0;
    bool infinite = false;
    bool ponder = false;
    int multiPV = 1;
};

struct RootMove {
    Move move = MOVE_NONE;
    Value score = Value(-VALUE_INFINITE);
    Value prevScore = Value(-VALUE_INFINITE);
    int selDepth = 0;
};

class Search {
public:
    Search();

    void clear();
    void new_game();
    void set_hash_size(size_t mb) { tt_.resize(mb); }
    void set_multi_pv(int n) { multiPV_ = std::max(1, std::min(n, MAX_ROOT_MOVES)); }
    void clear_hash() { tt_.clear(); }
    void set_node_limit(uint64_t n) { nodeLimit_ = n; }

    void think(Position& pos, StateInfo* si, const LimitsType& limits, std::ostream& os);

    uint64_t nodes() const { return nodes_; }
    int hashfull() const { return tt_.hashfull(); }
    int seldepth() const { return seldepth_; }
    int completed_depth() const { return completedDepth_; }
    int root_move_count() const { return rootMoveCount_; }
    Move root_move(int i) const { return rootMoves_[i].move; }
    Value root_score(int i) const { return rootMoves_[i].score; }
    int root_seldepth(int i) const { return rootMoves_[i].selDepth; }
    int pv_length(int i) const { return pvLen_[i]; }
    Move pv_move(int i, int j) const { return pv_[i][j]; }
    uint64_t nullmoves() const { return nullmoves_; }
    uint64_t cutoffs() const { return cutoffs_; }

private:
    Value  search(Position& pos, Stack* ss, Value alpha, Value beta, int depth, bool cutNode, bool pvNode);
    Value  qsearch(Position& pos, Stack* ss, Value alpha, Value beta);
    bool   check_time();
    int    reduction(bool improving, int depth, int moveNumber, bool quiet, int hist, int ply) const;
    void   update_pv(int ply, Move m, Stack* ss);

    TranspositionTable tt_;
    LimitsType limits_;
    StateStack states_;

    // histories
    Move killers[MAX_PLY + 8][2] = {};
    int history[COLOR_NB][SQUARE_NB][SQUARE_NB] = {};
    int captureHistory[PIECE_NB][SQUARE_NB][SQUARE_NB] = {};
    int counterMoveHistory[SQUARE_NB][SQUARE_NB][PIECE_TYPE_NB] = {};
    int nullHistory[64] = {};
    int threatHistory[PIECE_NB][SQUARE_NB] = {};

    Move pv_[MAX_ROOT_MOVES][MAX_PLY] = {};
    int  pvLen_[MAX_ROOT_MOVES] = {};

    RootMove rootMoves_[MAX_ROOT_MOVES];
    int rootMoveCount_ = 0;

    Stack* ss_ = nullptr;   // points at the current ply's stack slot

    uint64_t nodes_ = 0;
    uint64_t nodeLimit_ = 0;
    // Hard wall-clock budget for the move, in milliseconds, for the side to
    // move. Kept as its own field rather than read out of limits_.time[] in
    // check_time(), because the two colours' time arrays are both initialised
    // and reading a fixed index of them keyed the stop condition to White no
    // matter whose turn it was.
    int64_t timeLimit_ = 0;
    int64_t  startTime_ = 0;
    int seldepth_ = 0;
    int multiPV_ = 1;
    int completedDepth_ = 0;
    bool stop_ = false;
    uint64_t nullmoves_ = 0;
    uint64_t cutoffs_ = 0;
};

} // namespace Aurora
