// Aurora - transposition table
#pragma once
#include "types.h"
#include <cstring>
#include <vector>

namespace Aurora {

enum Bound : int { BOUND_NONE = 0, BOUND_UPPER = 1, BOUND_LOWER = 2, BOUND_EXACT = 3 };

struct TTEntry {
    U16  key16 = 0;
    i16  valueI = 0;
    i16  evalI = 0;
    U16  moveI = 0;
    U8   depthI = 0;
    U8   genBound = 0;

    Move  move16() const { return Move(moveI); }

    // Mate scores are stored relative to the *node*, not the root, so reading
    // one back requires undoing that shift with the current ply. The store side
    // (see tt.cpp) does the adjustment; doing it only there means every mate
    // score retrieved from the table was off by the depth at which it was
    // stored.
    Value value(int ply = 0) const {
        Value v = Value(valueI);
        if (v >= VALUE_MATE_IN_MAX_PLY)  return Value(v - ply);
        if (v <= VALUE_MATED_IN_MAX_PLY) return Value(v + ply);
        return v;
    }
    Value eval() const { return Value(evalI); }
    Bound bound() const { return Bound(genBound & 0x3); }
    int   depth8() const { return int(depthI); }
    bool  is_pv() const { return bool(genBound & 0x4); }
};

class TranspositionTable {
public:
    void resize(size_t mbSize);
    void clear();
    void new_search() { generation_ = U8(generation_ + 8); }
    void set(uint8_t age) { generation_ = age; }

    TTEntry* probe(U64 key, bool& found) {
        TTEntry& e = table_[key & mask_];
        // A stale-generation entry is still perfectly usable when the key
        // matches: generation only drives replacement, never correctness.
        found = (e.key16 == U16(key >> 48)) && (e.depthI != 0);
        return &e;
    }

    void new_entry(U64 key, Value v, bool pv, Bound b, int depth, Move m, Value ev, int ply);
    void store_move(U64 key, Move m);

    int hashfull(int n = 1000) const;
    size_t size_mb() const { return sizeMb_; }
    static size_t default_hash_mb() { return 64; }

private:
    std::vector<TTEntry> table_;
    size_t mask_ = 0;
    U8 generation_ = 0;
    size_t sizeMb_ = 0;
};

} // namespace Aurora
