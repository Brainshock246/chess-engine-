// Aurora - move generation
#pragma once
#include "position.h"
#include "types.h"

namespace Aurora {

enum GenType { CAPTURES, QUIETS, EVASIONS, NON_EVASIONS, LEGAL };

// A scored move. Larger `value` is searched first.
struct ExtMove {
    Move move = MOVE_NONE;
    int  value = 0;
    operator Move() const { return move; }
    ExtMove& operator=(Move m) { move = m; value = 0; return *this; }
};

// Fixed-capacity move container (no allocation in the search hot path)
struct MoveList {
    static constexpr int MAX_MOVES = 256;
    ExtMove moves[MAX_MOVES];
    int count = 0;

    int size() const { return count; }
    void clear() { count = 0; }
    void add(Move m, int v = 0) { if (count < MAX_MOVES) { moves[count].move = m; moves[count].value = v; ++count; } }
    bool contains(Move m) const {
        for (int i = 0; i < count; ++i) if (moves[i].move == m) return true;
        return false;
    }
    void sort() {
        std::sort(moves, moves + count, [](const ExtMove& a, const ExtMove& b) { return a.value > b.value; });
    }
    ExtMove& operator[](int i) { return moves[i]; }
    const ExtMove& operator[](int i) const { return moves[i]; }
    ExtMove* begin() { return moves; }
    ExtMove* end() { return moves + count; }
};

// MovePicker - lazy staged move generation
class MovePicker {
public:
    // thresh: history threshold above which a quiet move is considered "good"
    MovePicker(const Position& pos, Move ttm, int depth, const int (&killers)[2], int thresh = 0);

    // Returns the next move to try, or MOVE_NONE when exhausted.
    // If skipQuiets is true, returns only captures/evasions.
    Move next_move(bool skipQuiets = false);

private:
    void score_quiets();
    void score_captures();
    ExtMove* pick_best();

    const Position& pos_;
    MoveList moves_;
    ExtMove* cur_ = nullptr;
    ExtMove* end_ = nullptr;
    Move  ttMove_ = MOVE_NONE;
    int   depth_ = 0;
    int   killer0_ = MOVE_NONE, killer1_ = MOVE_NONE;
    int   thresh_ = 0;
    int   stage_ = 0;
};

void generate(const Position& pos, MoveList& list, GenType type);

// Static exchange evaluation: is the net gain of `m` at least `threshold`?
bool see_ge(const Position& pos, Move m, int threshold);

// UCI move string conversion
std::string to_string_move(Move m);
Move to_move(const Position& pos, const std::string& str);

// MVV-LVA
inline int mvv_lva(const Position& pos, Move m) {
    if (!pos.capture_or_promotion(m)) return 0;
    PieceType to = type_of(pos.piece_on(to_sq(m)));
    if (to == NO_PT) to = PAWN;
    PieceType from = type_of(pos.piece_on(from_sq(m)));
    return PieceValue[to] * 16 - PieceValue[from];
}

} // namespace Aurora
