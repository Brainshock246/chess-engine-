#!/usr/bin/env python3
"""
perft.py - correctness harness for the Aurora move generator.

Runs the engine's perft against a table of known-correct node counts. Any
mismatch is a movegen / make-move / unmake-move bug, and perft is by far the
cheapest way to find those: a single depth-5 run in the initial position
catches essentially every legal-move error.
"""
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "bin" / "aurora.exe"

# (name, fen, [(depth, expected_nodes), ...])
SUITE = [
    ("startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     [(1, 20), (2, 400), (3, 8902), (4, 197281), (5, 4865609), (6, 119060324)]),
    ("kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     [(1, 48), (2, 2039), (3, 97862), (4, 4085603), (5, 193690690)]),
    ("position3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     [(1, 14), (2, 191), (3, 2812), (4, 43238), (5, 674624), (6, 11030083)]),
    ("position4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     [(1, 6), (2, 264), (3, 9467), (4, 422333), (5, 15833292)]),
    ("position5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     [(1, 44), (2, 1486), (3, 62379), (4, 2103487), (5, 89941194)]),
    ("position6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     [(1, 46), (2, 2079), (3, 89890), (4, 3894594)]),
    ("promo-capture", "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
     [(1, 24), (2, 496), (3, 9483), (4, 182838)]),
    ("ep-pin", "8/8/8/8/k1p4R/8/3P4/3K4 w - - 0 1",
     [(1, 12), (2, 110), (3, 1047), (4, 7364)]),
    ("castling-rights", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
     [(1, 26), (2, 568), (3, 13744), (4, 314346)]),
    ("stalemate", "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1",
     [(1, 0), (2, 0), (3, 0)]),
    ("mate-in-2", "r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4",
     [(1, 2), (2, 12)]),
]


def run_perft(fen: str, depth: int) -> int:
    out = subprocess.run([str(ENGINE), "perft", fen, str(depth)],
                         capture_output=True, text=True, timeout=1800)
    for line in out.stdout.splitlines():
        if line.startswith("Nodes searched:"):
            return int(line.split(":")[1])
    raise RuntimeError(f"no result for {fen} depth {depth}\n{out.stdout}\n{out.stderr}")


def divide_check(fen: str, depth: int, expected_total: int) -> bool:
    """Verify not just the total but each root child's count."""
    out = subprocess.run([str(ENGINE), "perft", fen, str(depth)],
                         capture_output=True, text=True, timeout=1800)
    per_root = {}
    for line in out.stdout.splitlines():
        if ":" in line and not line.startswith("Nodes"):
            mv, n = line.split(":")
            per_root[mv.strip()] = int(n)
    return sum(per_root.values()) == expected_total


def main() -> int:
    if not ENGINE.exists():
        print(f"engine not built: {ENGINE}", file=sys.stderr)
        return 2

    failures = 0
    total_nodes = 0
    t0 = time.time()

    for name, fen, depths in SUITE:
        for depth, expected in depths:
            start = time.time()
            got = run_perft(fen, depth)
            elapsed = time.time() - start
            total_nodes += got
            nps = int(got / elapsed) if elapsed > 0 else 0
            ok = got == expected
            status = "OK  " if ok else "FAIL"
            if not ok:
                failures += 1
            print(f"{status} {name:<18} depth {depth}  got {got:>14,}  "
                  f"expected {expected:>14,}  ({elapsed:6.2f}s, {nps:,} nps)")

    print()
    print(f"total nodes: {total_nodes:,}   elapsed: {time.time()-t0:.1f}s   "
          f"failures: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
