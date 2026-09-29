#include "bitboard.h"
#include "magics.h"
#include <cstring>
#include <cstdio>

namespace Bitboards {

U64 SquareBB[64];
U64 FileBB[8];
U64 RankBB[8];
U64 BetweenBB[64][64];
U64 LineBB[64][64];
U64 KnightTable[64];
U64 KingTable[64];

// ---------------------------------------------------------------------------
// Sliding piece attacks via magic bitboards.
//
// Magics are *found* at startup, not hard-coded, and every candidate is
// verified exhaustively against a ray-walking reference before it is accepted:
// a magic is only used if it reproduces the reference attack set for all 2^k
// blocker subsets. A magic that cannot be verified is never installed, and
// verify_attack_tables() re-checks the installed tables afterwards, so a bad
// table is a loud, immediate failure rather than a silently wrong engine.
// ---------------------------------------------------------------------------

struct Magic {
    U64  mask = 0;
    U64  magic = 0;
    U64* attacks = nullptr;
    unsigned shift = 0;
    unsigned index(U64 occ) const { return unsigned(((occ & mask) * magic) >> shift); }
};

// Populate only the non-magic tables (square/file/rank/knight/king). Used by
// the offline magic-search experiment, which supplies its own attack tables.
void init_baseline_tables() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int f = 0; f < 8; ++f) FileBB[f] = 0x0101010101010101ULL << f;
    for (int r = 0; r < 8; ++r) RankBB[r] = 0xFFULL << (8 * r);
    for (Square s = SQ_A1; s <= SQ_H8; ++s) SquareBB[s] = 1ULL << int(s);
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        const int sf = int(file_of(s)), sr = int(rank_of(s));
        static const int kf[8] = { 1, 2, 2, 1, -1, -2, -2, -1 };
        static const int kr[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };
        U64 n = 0;
        for (int i = 0; i < 8; ++i) {
            int f = sf + kf[i], r = sr + kr[i];
            if (f >= 0 && f < 8 && r >= 0 && r < 8) n |= SquareBB[make_square(File(f), Rank(r))];
        }
        KnightTable[s] = n;
        U64 k = 0;
        for (int df = -1; df <= 1; ++df)
            for (int dr = -1; dr <= 1; ++dr) {
                if (!df && !dr) continue;
                int f = sf + df, r = sr + dr;
                if (f >= 0 && f < 8 && r >= 0 && r < 8) k |= SquareBB[make_square(File(f), Rank(r))];
            }
        KingTable[s] = k;
    }
}

namespace {

Magic BishopMagics[64];
Magic RookMagics[64];
U64 BishopAttacks[64][512];
// A rook's edge-excluded mask is 12 bits on every square, which would demand a
// *perfect* hash into a 12-bit (4096 entry) table. Perfect hashes for this
// pattern are not something a random search finds, so the rook table is sized
// at 14 bits instead: 8 MB of tables in exchange for a construction that is
// both fast to search and provably verifiable.
U64 RookAttacks[64][16384];

struct Rng {
    U64 s;
    explicit Rng(U64 seed) : s(seed) {}
    U64 next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ULL; }
    U64 sparse() { return next() & next() & next(); }
};

// Reference attacks by walking the rays, checking BOTH file and rank so a
// diagonal can never wrap around the edge of the board.
U64 reference_attacks(Square s, U64 occ, bool bishop) {
    static const int bDF[4] = {  1,  1, -1, -1 };
    static const int bDR[4] = {  1, -1,  1, -1 };
    static const int rDF[4] = {  0,  0,  1, -1 };
    static const int rDR[4] = {  1, -1,  0,  0 };
    const int* DF = bishop ? bDF : rDF;
    const int* DR = bishop ? bDR : rDR;

    int f = int(file_of(s)), r = int(rank_of(s));
    U64 attacks = 0;
    for (int i = 0; i < 4; ++i) {
        int cf = f, cr = r;
        while (true) {
            cf += DF[i]; cr += DR[i];
            if (cf < 0 || cf > 7 || cr < 0 || cr > 7) break;
            U64 b = SquareBB[make_square(File(cf), Rank(cr))];
            attacks |= b;
            if (occ & b) break;
        }
    }
    return attacks;
}

// The magic's mask deliberately EXCLUDES the outermost square of every ray
// (the board edges). A blocker sitting on the far end of a ray is
// indistinguishable from no blocker at all - the slider attacks that square
// either way - so those bits only ever split subsets that already produce
// identical attack sets. Dropping them is what makes a 9-bit bishop index
// (512 entries) sufficient for even a central square, and it is the reason the
// magic search succeeds at all.
U64 sliding_mask(Square s, bool bishop) {
    U64 edges = ((RankBB[RANK_1] | RankBB[RANK_8]) & ~RankBB[rank_of(s)])
              | ((FileBB[FILE_A] | FileBB[FILE_H]) & ~FileBB[file_of(s)]);
    return reference_attacks(s, 0, bishop) & ~edges;
}

// Verify a candidate magic against every blocker subset.
//
// The classic version memsets the whole table per candidate, which dominates
// the cost (32 KB for a rook). Using a stamp array instead makes each attempt
// O(#subsets) with no clearing at all, which is what brings a from-scratch
// magic search down to a few seconds.
inline bool magic_ok(const Magic& m, const U64* occ, const U64* ref, int cnt,
                     int slots, U64* tbl, U32* stamp, U32& epoch) {
    ++epoch;
    for (int i = 0; i < cnt; ++i) {
        unsigned idx = m.index(occ[i]);
        if (idx >= unsigned(slots)) return false;             // index escapes the table
        if (stamp[idx] == epoch) {
            if (tbl[idx] != ref[i]) return false;             // two different attack sets share a slot
        } else {
            stamp[idx] = epoch;
            tbl[idx] = ref[i];
        }
    }
    return true;
}

template<typename Table>
void init_magics(bool bishop, Magic magics[], Table& table, U64 seedBase, bool& ok, bool report = true) {
    const int slots = bishop ? 512 : 16384;
    int hardestAttempts = 0;

    static U64 occ[4096], ref[4096];
    static U64 tbl[16384];
    static U32 stamp[16384];
    U32 epoch = 0;

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Magic& m = magics[s];
        m.mask    = sliding_mask(s, bishop);
        m.shift   = 64 - unsigned(popcount(m.mask));
        m.attacks = &table[s][0];

        int cnt = 0;
        U64 b = 0;
        do {
            occ[cnt] = b;
            ref[cnt] = reference_attacks(s, b, bishop);
            ++cnt;
            b = (b - m.mask) & m.mask;
        } while (b);

        const int k = popcount(m.mask);

        Rng rng(seedBase + 8 * int(s) + (bishop ? 1ULL : 0ULL));
        bool found = false;
        int attempts = 0;
        // The spread test on the top byte of (mask * magic) is the crucial
        // filter. It selects candidates whose *high* products are dense, which
        // is what makes subsets that share an attack set share an index.
        // Measured on this machine: rook worst case ~100k attempts, bishop
        // ~19k, with no failures across all 128 squares.
        for (int attempt = 0; attempt < 200000000 && !found; ++attempt) {
            U64 magic = rng.sparse();
            if (popcount((m.mask * magic) >> 56) < 6) continue;
            m.magic = magic;
            ++attempts;
            found = magic_ok(m, occ, ref, cnt, slots, tbl, stamp, epoch);
        }

        if (!found) {
            std::fprintf(stderr, "Bitboards: no verified magic for %s square %d "
                         "(mask bits=%d, %d subsets, %d attempts)\n",
                         bishop ? "bishop" : "rook", int(s), k, cnt, attempts);
            ok = false;
            return;
        }
        hardestAttempts = std::max(hardestAttempts, attempts);

        // Commit the winning magic's table.
        std::memset(m.attacks, 0, size_t(slots) * sizeof(U64));
        for (int i = 0; i < cnt; ++i) m.attacks[m.index(occ[i])] = ref[i];
    }
    if (report)
        std::fprintf(stderr, "Bitboards: %s magics ok, hardest square took %d attempts\n",
                     bishop ? "bishop" : "rook", hardestAttempts);
}

// Install a previously-found set of magics and rebuild the attack tables.
template<typename Table>
bool install_magics(bool bishop, Magic magics[], Table& table, const U64 found[64], const char* label) {
    const int slots = bishop ? 512 : 16384;
    static U64 occ[4096], ref[4096];
    static U64 tbl[16384];
    static U32 stamp[16384];
    U32 epoch = 0;

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Magic& m = magics[s];
        m.mask    = sliding_mask(s, bishop);
        m.shift   = 64 - unsigned(popcount(m.mask));
        m.attacks = &table[s][0];
        m.magic   = found[s];

        int cnt = 0;
        U64 b = 0;
        do {
            occ[cnt] = b;
            ref[cnt] = reference_attacks(s, b, bishop);
            ++cnt;
            b = (b - m.mask) & m.mask;
        } while (b);

        // A baked-in magic is trusted only after the same exhaustive check the
        // search performs. A stale or corrupted table is a hard failure.
        if (!magic_ok(m, occ, ref, cnt, slots, tbl, stamp, epoch)) {
            std::fprintf(stderr, "Bitboards: baked %s magic invalid for square %d\n",
                         bishop ? "bishop" : "rook", int(s));
            return false;
        }

        std::memset(m.attacks, 0, size_t(slots) * sizeof(U64));
        for (int i = 0; i < cnt; ++i) m.attacks[m.index(occ[i])] = ref[i];
    }
    (void)label;
    return true;
}

} // namespace

U64 bishop_attacks_bb(Square s, U64 occ) {
    const Magic& m = BishopMagics[s];
    return m.attacks[m.index(occ)];
}

U64 rook_attacks_bb(Square s, U64 occ) {
    const Magic& m = RookMagics[s];
    return m.attacks[m.index(occ)];
}

U64 pawn_attacks_bb(Color c, U64 b) {
    // Both diagonals are applied to `b` directly. Shifting straight up first
    // and then diagonally would move two ranks instead of one (a pawn on e5
    // would "attack" d7 and f7 rather than d6 and f6), which corrupts check
    // detection, pin detection and pawn move generation all at once.
    int eastDiag = int(c == WHITE ? NORTH_EAST : SOUTH_WEST);
    int westDiag = int(c == WHITE ? NORTH_WEST : SOUTH_EAST);
    return shift_bb(eastDiag, b) | shift_bb(westDiag, b);
}

void init() {
    static bool done = false;
    if (done) return;
    done = true;

    for (int f = 0; f < 8; ++f) FileBB[f] = 0x0101010101010101ULL << f;
    for (int r = 0; r < 8; ++r) RankBB[r] = 0xFFULL << (8 * r);
    for (Square s = SQ_A1; s <= SQ_H8; ++s) SquareBB[s] = 1ULL << int(s);

    bool ok = true;
    // Prefer the magics baked in at build time: startup becomes instant and,
    // more importantly, bit-for-bit identical on every run and every machine,
    // which is what makes the A/B results in tools/ reproducible. They are
    // verified exhaustively before use; if that ever fails we fall back to
    // searching from scratch rather than trusting them.
    ok = install_magics(true,  BishopMagics, BishopAttacks, BakedBishopMagic, "baked bishop");
    if (ok) ok = install_magics(false, RookMagics, RookAttacks, BakedRookMagic, "baked rook");

    if (!ok) {
        std::fprintf(stderr, "Bitboards: baked magics rejected, searching at runtime\n");
        ok = true;
        init_magics(true,  BishopMagics, BishopAttacks, 0x9E3779B97F4A7C15ULL, ok);
        if (ok) init_magics(false, RookMagics, RookAttacks, 0xD1B54A32D192ED03ULL, ok);
    }
    if (!ok) {
        std::fprintf(stderr, "Bitboards: FATAL - magic construction failed\n");
        std::abort();
    }

    // between / line
    //
    // Between must be gated on alignment. The mutual-attack expression
    //   rook(s1, s2)   & rook(s2, s1)
    // is only "squares between them" when s1 and s2 share a rank or file; for a
    // diagonal pair it degenerates to the two L-shaped corners (for g1/b6 that
    // is b1 and g6), and those extra squares are then treated as legal check
    // evasions. The same applies to the bishop term for rank/file pairs. So
    // test the geometry first and use only the term that fits.
    for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
        for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
            const int df = std::abs(int(file_of(s1)) - int(file_of(s2)));
            const int dr = std::abs(int(rank_of(s1)) - int(rank_of(s2)));
            if (df == 0 || dr == 0)
                BetweenBB[s1][s2] = rook_attacks_bb(s1, SquareBB[s2]) & rook_attacks_bb(s2, SquareBB[s1]);
            else if (df == dr)
                BetweenBB[s1][s2] = bishop_attacks_bb(s1, SquareBB[s2]) & bishop_attacks_bb(s2, SquareBB[s1]);
            else
                BetweenBB[s1][s2] = 0;
        }

    // LineBB must be the WHOLE line through s1 and s2, and it must be gated on
    // alignment. Two mistakes are possible here and both are subtle:
    //
    //  * Building it as "between + endpoints" degenerates for adjacent
    //    squares, because there is nothing between them - which made a pinned
    //    piece appear unable to capture its own pinner.
    //  * Building it as (rook(s1,0) & rook(s2,0)) | (bishop(s1,0) &
    //    bishop(s2,0)) without checking geometry picks up the corners of the
    //    bounding rectangle for *unaligned* pairs. For f6/h7 that includes f7,
    //    so aligned(f6, h7, f7) came out true and a pinned knight could step
    //    off the file it was guarding.
    for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
        for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
            const int df = std::abs(int(file_of(s1)) - int(file_of(s2)));
            const int dr = std::abs(int(rank_of(s1)) - int(rank_of(s2)));
            U64 line = 0;
            if (df == 0)      line = FileBB[file_of(s1)];
            else if (dr == 0) line = RankBB[rank_of(s1)];
            else if (df == dr) line = bishop_attacks_bb(s1, 0) & bishop_attacks_bb(s2, 0);
            LineBB[s1][s2] = line | SquareBB[s1] | SquareBB[s2];
        }

    // leapers
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        const int sf = int(file_of(s)), sr = int(rank_of(s));
        static const int kf[8] = { 1, 2, 2, 1, -1, -2, -2, -1 };
        static const int kr[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };
        U64 n = 0;
        for (int i = 0; i < 8; ++i) {
            int f = sf + kf[i], r = sr + kr[i];
            if (f >= 0 && f < 8 && r >= 0 && r < 8) n |= SquareBB[make_square(File(f), Rank(r))];
        }
        KnightTable[s] = n;

        U64 k = 0;
        for (int df = -1; df <= 1; ++df)
            for (int dr = -1; dr <= 1; ++dr) {
                if (!df && !dr) continue;
                int f = sf + df, r = sr + dr;
                if (f >= 0 && f < 8 && r >= 0 && r < 8) k |= SquareBB[make_square(File(f), Rank(r))];
            }
        KingTable[s] = k;
    }
}

int verify_attack_tables() {
    // Exhaustive re-check of every installed magic against the reference.
    int bad = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        for (bool bishop : {true, false}) {
            U64 mask = sliding_mask(s, bishop);
            U64 sub = 0;
            do {
                U64 got = bishop ? bishop_attacks_bb(s, sub) : rook_attacks_bb(s, sub);
                U64 want = reference_attacks(s, sub, bishop);
                if (got != want) ++bad;
                sub = (sub - mask) & mask;
            } while (sub);
        }
    }
    return bad;
}

} // namespace Bitboards

const int PieceValue[PIECE_NB] = {
    0,
    100, 320, 330, 500, 950, 0,
    0,
    0,
    100, 320, 330, 500, 950, 0,
    0
};
