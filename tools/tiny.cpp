#include "bitboard.h"
#include <cstdio>
int main() {
    std::fprintf(stderr, "A\n"); std::fflush(stderr);
    for (int i = 0; i < 64; ++i) Bitboards::SquareBB[i] = 1ULL << i;
    std::fprintf(stderr, "B: plain int loop ok\n"); std::fflush(stderr);

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Bitboards::SquareBB[s] = 1ULL << int(s);
        if (int(s) < 3 || int(s) > 61) {
            std::fprintf(stderr, "   s=%d\n", int(s)); std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "C: Square loop ok\n"); std::fflush(stderr);
    return 0;
}
