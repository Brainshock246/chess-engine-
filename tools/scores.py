#!/usr/bin/env python3
"""
scores.py - score-sanity regression suite.

perft proves the tree is right. evalsym proves the evaluation is symmetric. tactics
proves the search can find something that is there. None of them checks the thing
that was actually broken longest: **the score itself**.

Two of the worst search defects in this project were invisible to every other test
and obvious here. In a position where the side to move is a full queen up, the
search reported

    depth 1  +1050     depth 6  +1029     depth 7   +778
    depth 2  +1065     depth 7   +778     depth 8    +14
    depth 3  +1065     depth 8    +14     depth 9    -14

A score that decays with depth in a position whose material never changes is
arithmetic on stale or misused values, not a horizon effect. And a sign inversion
that appears at exactly one depth is a branch at exactly one threshold - which is
how ProbCut was caught, since PROBCUT_MIN_DEPTH is 5 and the endgame first went
wrong at depth 5.

So each case declares a *sign* the score must keep at every depth, and optionally a
band it must stay within. Cases are chosen so the correct answer is not in doubt:
a lone king cannot mate, and a queen cannot be lost to one.

  python tools/scores.py
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURORA = ROOT / "bin" / "aurora.exe"

# (name, fen, sign, max_abs, note)
#   sign     +1 / -1 / 0   required sign of the searched score, 0 = either
#   max_abs  int or None   |score| must stay under this, None = no bound
CASES = [
    ("queen up for White",
     "4k3/8/8/8/8/8/8/4K1Q1 w - - 0 1", +1, 4000,
     "White a full queen up. A lone king cannot mate and cannot take a queen back."),

    ("queen down for Black",
     "4k3/8/8/8/8/8/8/5KQ1 b - - 0 1", -1, 4000,
     "Black a full queen down, to move. Black has only a king."),

    ("queen up for White, Black to move",
     "4k3/8/8/8/8/8/8/4K1Q1 b - - 0 1", -1, 4000,
     "Same material, other side to move: the sign must flip with the side."),

    ("material equal, one queen each",
     "3qk3/8/8/8/8/8/8/4K1Q1 w - - 0 1", 0, 300,
     "No material difference, so the score must stay near zero - a queen-sized "
     "value means the search invented a piece."),

    ("rooks only, equal",
     "3rk3/8/8/8/8/8/8/3RK3 w - - 0 1", 0, 300,
     "Equal rooks; no tactic exists and the score must stay small."),

    ("bare kings",
     "4k3/8/8/8/8/8/8/4K3 w - - 0 1", 0, 150,
     "A king cannot mate. Any large positive score is a bug."),

    ("White a rook up, endgame",
     "4k3/8/8/8/8/8/8/3RK3 w - - 0 1", +1, 2000,
     "K+R against K is winning, so the sign must stay positive."),

    # Corrected. This case asserted sign +1 on the grounds that "White is up a
    # queen", and it is not: the queen on g2 is Black's. Worse for the assertion,
    # the white king on h1 attacks g2, so 1.Kxg2 wins the queen outright and the
    # material ends level - which is why a score near zero is the right answer
    # and a large one would be the bug. A test whose expected value I got wrong
    # produced a FAIL that meant nothing; that has happened twice on this project.
    ("queen en prise to the king",
     "7k/8/8/8/8/8/6q1/7K w - - 0 1", 0, 200,
     "Kxg2 wins Black's queen and the material ends level, so the score must "
     "stay near zero. A large negative score would mean the engine missed the "
     "capture."),
]


def search_score(exe: str, fen: str, depth: int) -> int | None:
    cmds = f"position fen {fen}\ngo depth {depth}\nquit\n"
    proc = subprocess.run([exe], input=cmds, capture_output=True,
                          text=True, timeout=900)
    best = None
    for line in proc.stdout.splitlines():
        if line.startswith(f"info depth {depth} ") or line.startswith("info depth "):
            if f"depth {depth} " not in line:
                continue
            if " score cp " in line:
                try:
                    best = int(line.split(" score cp ")[1].split()[0])
                except (IndexError, ValueError):
                    pass
    return best


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--aurora", default=str(AURORA))
    ap.add_argument("--depths", default="1,2,3,4,5,6,7,8,9",
                    help="comma-separated depths to check")
    args = ap.parse_args()

    if not Path(args.aurora).exists():
        print(f"missing {args.aurora}", file=sys.stderr)
        return 2

    depths = [int(d) for d in args.depths.split(",") if d.strip()]
    print(f"score-sanity: {len(CASES)} cases x {len(depths)} depths\n")

    failures = 0
    for name, fen, sign, max_abs, note in CASES:
        scores: list[tuple[int, int]] = []
        for d in depths:
            s = search_score(args.aurora, fen, d)
            if s is not None:
                scores.append((d, s))

        if not scores:
            print(f"  ??   {name:<34} no scores returned")
            failures += 1
            continue

        bad: list[str] = []
        for d, s in scores:
            if sign and (s > 0) != (sign > 0):
                bad.append(f"d{d}={s}")
            if max_abs is not None and abs(s) > max_abs:
                bad.append(f"d{d}={s}")

        vals = " ".join(f"{s:>6}" for _, s in scores)
        ds = " ".join(f"{d:>2}" for d, _ in scores)
        if bad:
            failures += 1
            print(f"  FAIL {name:<34} {vals}")
            print(f"       depths {ds}")
            print(f"       offending: {', '.join(bad)}")
            print(f"       {note}")
        else:
            print(f"  OK   {name:<34} {vals}")

    print()
    if failures:
        print(f"{failures} case(s) failed. A score that changes sign or grows")
        print("without the position changing is a search defect, not a heuristic.")
    else:
        print("all cases stable across depth")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
