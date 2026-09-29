// Experiment: which magic-search recipe actually finds valid rook/bishop magics?
// Reports attempts needed per square for a few candidate strategies.
#include "bitboard.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Bitboards;

struct Rng {
    U64 s;
    explicit Rng(U64 seed) : s(seed) {}
    U64 next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ULL; }
    U64 sparse() { return next() & next() & next(); }
};

static U64 ref_attacks(Square s, U64 occ, bool bishop) {
    static const int bDF[4] = { 1, 1, -1, -1 }, bDR[4] = { 1, -1, 1, -1 };
    static const int rDF[4] = { 0, 0, 1, -1 }, rDR[4] = { 1, -1, 0, 0 };
    const int* DF = bishop ? bDF : rDF;
    const int* DR = bishop ? bDR : rDR;
    int f = int(file_of(s)), r = int(rank_of(s));
    U64 a = 0;
    for (int i = 0; i < 4; ++i) {
        int cf = f, cr = r;
        while (true) {
            cf += DF[i]; cr += DR[i];
            if (cf < 0 || cf > 7 || cr < 0 || cr > 7) break;
            U64 b = 1ULL << (cr * 8 + cf);
            a |= b;
            if (occ & b) break;
        }
    }
    return a;
}

static U64 smask(Square s, bool bishop) {
    U64 edges = ((RankBB[RANK_1] | RankBB[RANK_8]) & ~RankBB[rank_of(s)])
              | ((FileBB[FILE_A] | FileBB[FILE_H]) & ~FileBB[file_of(s)]);
    return ref_attacks(s, 0, bishop) & ~edges;
}

int main() {
    init_baseline_tables();
    std::vector<U64> occ(8192), ref(8192);
    std::vector<U64> tbl(1 << 16);
    std::vector<U32> stamp(1 << 16);
    U32 epoch = 0;

    struct Strat { const char* name; int kind; };
    // kind 0: SF-style sparse + >>56 spread filter
    // kind 1: SF-style sparse, no filter
    // kind 2: 3-6 low random bits, no filter
    Strat strats[] = {
        {"sparse+spread56", 0},
        {"sparse nofilter", 1},
        {"lowbits nofilter", 2},
    };

    for (bool bishop : {true, false}) {
        const int slots = bishop ? 512 : 16384;
        for (int si = 0; si < 3; ++si) {
            int worst = 0, worstSq = -1, failed = 0;
            for (Square s = SQ_A1; s <= SQ_H8; ++s) {
                U64 mask = smask(s, bishop);
                unsigned shift = 64u - unsigned(popcount(mask));
                int cnt = 0;
                U64 b = 0;
                do { occ[cnt] = b; ref[cnt] = ref_attacks(s, b, bishop); ++cnt; b = (b - mask) & mask; } while (b);

                Rng rng(0x9E3779B97F4A7C15ULL + 8 * int(s) + (bishop ? 1 : 0));
                bool found = false;
                int attempts = 0;
                for (int t = 0; t < 20000000 && !found; ++t) {
                    U64 magic;
                    if (strats[si].kind == 2) {
                        magic = 0;
                        int nb = 3 + int(rng.next() % 4);
                        int maxPos = std::max(8, 63 + (bishop ? 9 : 14) - 2 * int(popcount(mask)));
                        for (int q = 0; q < nb; ++q) magic |= 1ULL << (rng.next() % U64(maxPos + 1));
                    } else {
                        magic = rng.sparse();
                        if (strats[si].kind == 0 && popcount((mask * magic) >> 56) < 6) continue;
                    }
                    ++attempts;
                    ++epoch;
                    bool good = true;
                    for (int i = 0; i < cnt; ++i) {
                        unsigned idx = unsigned(((occ[i] & mask) * magic) >> shift);
                        if (idx >= unsigned(slots)) { good = false; break; }
                        if (stamp[idx] == epoch) { if (tbl[idx] != ref[i]) { good = false; break; } }
                        else { stamp[idx] = epoch; tbl[idx] = ref[i]; }
                    }
                    found = good;
                }
                if (!found) ++failed;
                else if (attempts > worst) { worst = attempts; worstSq = int(s); }
            }
            std::fprintf(stderr, "%-6s %-18s failed=%2d  worst=%9d (sq %d)\n",
                         bishop ? "bishop" : "rook", strats[si].name, failed, worst, worstSq);
        }
    }
    return 0;
}
