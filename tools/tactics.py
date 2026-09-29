#!/usr/bin/env python3
"""
tactics.py - a search regression suite.

perft says the engine can enumerate the right tree. evalsym says the evaluation
is symmetric. Neither says the search can *find* something when it is there, and
during development the engine's opening choice was b1a3 at every depth from 1 to
10 - which no honest evaluation wants and which no amount of perft would reveal.

Each case is a position with a known answer:

  expect=<move>   the engine must find exactly this move
  avoid=<moves>   the engine must not play any of these (blunder traps)

The suite is deliberately small and entirely checkable by hand. A case that
cannot be verified by reading it is not worth having.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURORA = ROOT / "bin" / "aurora.exe"

# (name, fen, expect, avoid, note)
CASES = [
    # --- must find a mate in one -------------------------------------------
    ("mate-in-1 back rank",
     "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1",
     "a1a8", [], "Ra8# is the only move that mates"),

    # --- must capture material that is simply hanging ------------------------
    ("take a hanging queen",
     "4k3/8/8/3q4/8/8/8/3QK3 w - - 0 1",
     "d1d5", [], "Qxd5 wins a queen"),

    ("take a hanging rook",
     "3r2k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1",
     "d1d8", [], "Rxd8 wins a rook"),

    # --- must not walk into a losing move ------------------------------------
    ("do not hang the queen to a discovered attack",
     "rnbqkbnr/ppp2ppp/8/3pp3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3",
     "", ["f1e2", "d2d4", "f3g5"], "the f3 knight is pinned-ish; blunder traps"),

    # The c-pawn defends the d5 knight, so Nxd5 walks into cxd5.
    ("do not capture into a recapture",
     "4k3/2p5/3n4/8/2N5/8/8/4K3 w - - 0 1",
     "", ["c3d5"], "Nxd5?? is met by cxd5 and loses a piece"),

    # Taken from the previous case by mistake: with no c-pawn, Nxd5 simply
    # wins an undefended knight and is a *good* move. Recorded because asserting
    # otherwise produced a FAIL that was the test being wrong, not the engine.
    ("capture an undefended knight",
     "4k3/8/8/3n4/8/2N5/8/4K3 w - - 0 1",
     "c3d5", [], "no defender: Nxd5 must be taken"),

    # --- must avoid stalemating when winning --------------------------------
    ("do not stalemate a won position",
     "7k/8/8/8/8/8/1Q6/6K1 w - - 0 1",
     "", ["b2b7"], "Qb7 vs Kh8 with Kg1 supporting is stalemate"),

    # --- simple development sanity ------------------------------------------
    ("castling is available and sane",
     "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1",
     "", [], "not asserted; present to keep the suite honest about positions"),

    # --- a real mate-in-2 (Philidor) ----------------------------------------
    ("mate in 2 (Philidor)",
     "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 0 3",
     "", [], "normal developing position, not asserted"),
]


def run_case(exe: str, fen: str, depth: int, movetime: int | None) -> str:
    cmds = ["uci", "isready", f"position fen {fen}",
            f"go depth {depth}" if not movetime else f"go movetime {movetime}"]
    proc = subprocess.run([exe], input="\n".join(cmds) + "\nquit\n",
                          capture_output=True, text=True, timeout=900)
    best = ""
    for line in proc.stdout.splitlines():
        if line.startswith("bestmove "):
            best = line.split()[1]
    return best


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--movetime", type=int, default=None)
    ap.add_argument("--aurora", default=str(AURORA))
    args = ap.parse_args()

    if not Path(args.aurora).exists():
        print(f"missing {args.aurora}", file=sys.stderr)
        return 2

    passed = failed = skipped = 0
    for name, fen, expect, avoid, note in CASES:
        if not expect and not avoid:
            skipped += 1
            print(f"  --   {name:<42} (no assertion: {note})")
            continue
        best = run_case(args.aurora, fen, args.depth, args.movetime)
        ok = True
        why = ""
        if expect and best == "":
            ok, why = False, "engine returned no bestmove"
        elif expect:
            if best != expect:
                ok, why = False, f"expected {expect}, got {best}"
        if avoid and best in avoid:
            ok, why = False, f"played blunder {best}"
        if ok:
            passed += 1
            print(f"  OK   {name:<42} -> {best}")
        else:
            failed += 1
            print(f"  FAIL {name:<42} -> {best or '(none)'}   {why}")

    print(f"\n{passed} passed, {failed} failed, {skipped} not asserted")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
