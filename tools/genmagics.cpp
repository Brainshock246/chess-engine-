// genmagics.cpp - offline magic-bitboard search.
//
// The magics are searched once, here, and written to src/magics.h. The engine
// then loads that header at startup and re-verifies every table exhaustively
// before using it, so search time never appears in a game and a corrupted
// header can never produce a wrong move.
//
//   g++ -O2 -std=c++20 -Isrc tools/genmagics.cpp src/bitboard.cpp -o bin/genmagics.exe
//   ./bin/genmagics.exe > src/magics.h
//
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

    U64 out[2][64];

    for (int kind = 0; kind < 2; ++kind) {
        const bool bishop = (kind == 0);
        const int slots = bishop ? 512 : 16384;
        for (Square s = SQ_A1; s <= SQ_H8; ++s) {
            U64 mask = smask(s, bishop);
            unsigned shift = 64u - unsigned(popcount(mask));

            int cnt = 0;
            U64 b = 0;
            do { occ[cnt] = b; ref[cnt] = ref_attacks(s, b, bishop); ++cnt; b = (b - mask) & mask; } while (b);

            Rng rng(0x9E3779B97F4A7C15ULL + 8 * int(s) + (kind == 0 ? 1ULL : 0ULL));
            bool found = false;
            U64 magic = 0;
            int attempts = 0;
            for (int t = 0; t < 500000000 && !found; ++t) {
                U64 cand = rng.sparse();
                if (popcount((mask * cand) >> 56) < 6) continue;
                ++attempts;
                ++epoch;
                bool good = true;
                for (int i = 0; i < cnt; ++i) {
                    unsigned idx = unsigned(((occ[i] & mask) * cand) >> shift);
                    if (idx >= unsigned(slots)) { good = false; break; }
                    if (stamp[idx] == epoch) { if (tbl[idx] != ref[i]) { good = false; break; } }
                    else { stamp[idx] = epoch; tbl[idx] = ref[i]; }
                }
                if (good) { magic = cand; found = true; }
            }
            if (!found) {
                std::fprintf(stderr, "FAILED %s square %d\n", bishop ? "bishop" : "rook", int(s));
                return 1;
            }
            out[kind][s] = magic;
            std::fprintf(stderr, "%-6s %2d  attempts=%d\n", bishop ? "bishop" : "rook", int(s), attempts);
        }
    }

    // Emit the header
    std::printf("// GENERATED FILE - do not edit by hand.\n");
    std::printf("// Produced by tools/genmagics.cpp. Every value is re-verified\n");
    std::printf("// exhaustively by Bitboards::init() before the engine uses it.\n");
    std::printf("#pragma once\n#include \"types.h\"\n\n");
    std::printf("static const U64 BakedBishopMagic[64] = {\n");
    for (int s = 0; s < 64; s += 2)
        std::printf("    0x%016llxULL, 0x%016llxULL,%s // %s\n",
                    (unsigned long long)out[0][s], (unsigned long long)out[0][s + 1],
                    (s + 2 < 64) ? "" : "  ", SquareName(Square(s)));
    std::printf("};\n\n");
    std::printf("static const U64 BakedRookMagic[64] = {\n");
    for (int s = 0; s < 64; s += 2)
        std::printf("    0x%016llxULL, 0x%016llxULL,%s // %s\n",
                    (unsigned long long)out[1][s], (unsigned long long)out[1][s + 1],
                    (s + 2 < 64) ? "" : "  ", SquareName(Square(s)));
    std::printf("};\n");
    return 0;
}
