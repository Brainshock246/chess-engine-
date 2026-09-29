#include "tt.h"

namespace Aurora {

void TranspositionTable::resize(size_t mbSize) {
    sizeMb_ = mbSize;
    size_t count = (mbSize * 1024 * 1024) / sizeof(TTEntry);
    count = std::max<size_t>(count, 1);
    // Round down to a power of two for cheap masking.
    size_t pow2 = 1;
    while (pow2 * 2 <= count) pow2 *= 2;
    table_.assign(pow2, TTEntry{});
    mask_ = pow2 - 1;
    generation_ = 0;
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), TTEntry{});
    generation_ = 0;
}

void TranspositionTable::new_entry(U64 key, Value v, bool pv, Bound b, int depth, Move m, Value ev, int ply) {
    if (m == MOVE_NONE || b == BOUND_NONE) return;   // never overwrite with junk

    TTEntry* t = &table_[key & mask_];
    // Keep a deeper, age-fresher entry unless we are replacing it outright
    if (t->key16 == U16(key >> 48) && t->depthI > depth - 3 && t->move16() != MOVE_NONE
        && (t->genBound & 0xF8) == (generation_ & 0xF8))
        return;

    // Mate scores are stored relative to the node, not the root.
    v = (v >= VALUE_MATE_IN_MAX_PLY) ? Value(v + ply)
        : (v <= VALUE_MATED_IN_MAX_PLY) ? Value(v - ply) : v;

    if (b == BOUND_EXACT || depth > t->depthI - 3 || (t->genBound & 0xF8) != (generation_ & 0xF8)) {
        t->key16    = U16(key >> 48);
        t->valueI   = i16(v);
        t->evalI    = i16(ev);
        t->moveI    = U16(m);
        t->depthI   = U8(std::clamp(depth, 0, 255));
        t->genBound = U8((generation_ & 0xF8) | (pv ? 0x4 : 0) | (U8(b) & 0x3));
    }
}

void TranspositionTable::store_move(U64 key, Move m) {
    TTEntry& t = table_[key & mask_];
    if (t.key16 == U16(key >> 48) && t.move16() == MOVE_NONE) t.moveI = U16(m);
}

int TranspositionTable::hashfull(int n) const {
    int cnt = 0;
    int sample = std::min<int>(int(table_.size()), n);
    for (int i = 0; i < sample; ++i)
        if (table_[i].depthI != 0 && (table_[i].genBound & 0xF8) == (generation_ & 0xF8))
            ++cnt;
    return cnt * 1000 / std::max(sample, 1);
}

} // namespace Aurora
