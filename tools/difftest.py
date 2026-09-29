#!/usr/bin/env python3
"""
difftest.py - find move-generation bugs by differential testing against a
reference engine (Stockfish).

Rationale: hand-checking perft numbers tells you *that* something is wrong but
not *where*. Walking a tree of positions and comparing the legal move list of
Aurora against the reference at every step turns "perft(3) is 8934, should be
8902" into "after 1.e4 e5, Aurora offers 21 moves and the reference offers 20:
Aurora has an extra e2e1".

The search is breadth-first and stops at the first divergence, so the first
run localises the bug in a handful of positions.

Usage:
  python tools/difftest.py --depth 3
  python tools/difftest.py --ref "C:/stockfish.exe" --depth 4
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The diagnostic binary (tools/smoke.cpp) exposes exactly what this needs:
#   smoke2 "<fen>"                  -> FEN echo, blockers, and the legal moves
#   smoke2 --after "<fen>" <move>   -> "AFTER <fen>"
AURORA = ROOT / "bin" / "smoke2.exe"
DEFAULT_REF = Path(r"C:\Users\surek\chess\stockfish\sf19\stockfish\stockfish-windows-x86-64-universal.exe")

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


# ---------------------------------------------------------------------------
# reference engine
# ---------------------------------------------------------------------------

class RefEngine:
    def __init__(self, path: str):
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
            cwd=str(Path(path).parent),
        )
        self._send("uci")
        self._expect("uciok")
        self._send("isready")
        self._expect("readyok")

    def _send(self, s: str) -> None:
        assert self.proc.stdin
        self.proc.stdin.write(s + "\n")
        self.proc.stdin.flush()

    def _read(self) -> str:
        assert self.proc.stdout
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("reference engine closed stdout")
        return line.strip()

    def _expect(self, tok: str) -> None:
        while True:
            if self._read() == tok:
                return

    def moves(self, fen: str) -> list[str]:
        """Legal moves from a FEN, per the reference engine's own perft 1."""
        self._send(f"position fen {fen}")
        self._send("go perft 1")
        out = []
        while True:
            line = self._read()
            if line.startswith("Nodes searched"):
                break
            if ":" in line and len(line.split(":")[0].strip()) == 4:
                out.append(line.split(":")[0].strip())
        return sorted(out)

    def close(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


# ---------------------------------------------------------------------------
# our engine
# ---------------------------------------------------------------------------

def aurora_moves(exe: str, fen: str) -> list[str] | None:
    """Legal moves from the engine's `moves` mode, or None on failure."""
    p = subprocess.run([str(exe), "moves", fen], capture_output=True, text=True, timeout=60)
    moves = [ln.strip() for ln in p.stdout.splitlines() if len(ln.strip()) == 4]
    return sorted(moves) if moves else None


# ---------------------------------------------------------------------------
# applying a move to a FEN (only needs enough to keep walking the tree)
# ---------------------------------------------------------------------------

def apply_move(fen: str, mv: str) -> str | None:
    """
    Ask the engine to play the move and print the resulting FEN.

    We deliberately do not implement FEN manipulation here: getting castling
    rights, en passant and the halfmove clock right by hand is exactly the
    kind of code that hides bugs instead of finding them. The engine is the
    authority on its own state.
    """
    p = subprocess.run([str(AURORA), "after", fen, mv],
                       capture_output=True, text=True, timeout=60)
    for line in (p.stdout + p.stderr).splitlines():
        if line.startswith("AFTER "):
            return line.split(" ", 1)[1].strip()
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--aurora", default=str(AURORA))
    ap.add_argument("--ref", default=str(DEFAULT_REF))
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--start", default=START, help="FEN to start the walk from")
    ap.add_argument("--max", type=int, default=200000, help="cap on positions examined")
    args = ap.parse_args()

    if not Path(args.aurora).exists():
        print(f"missing {args.aurora} (build the debug binary first)", file=sys.stderr)
        return 2
    if not Path(args.ref).exists():
        print(f"missing reference engine {args.ref}", file=sys.stderr)
        return 2

    ref = RefEngine(args.ref)
    queue = deque([(args.start, [])])
    examined = 0
    bad = 0

    try:
        while queue and examined < args.max:
            fen, path = queue.popleft()
            examined += 1

            a = aurora_moves(args.aurora, fen)
            r = ref.moves(fen)
            if a is None:
                print(f"[{examined}] {fen}\n    AURORA PRODUCED NO MOVES")
                bad += 1
                continue
            if a != r:
                only_a = sorted(set(a) - set(r))
                only_r = sorted(set(r) - set(a))
                print(f"[{examined}] MISMATCH after: {' '.join(path) or '(startpos)'}")
                print(f"    fen: {fen}")
                print(f"    aurora extra  : {' '.join(only_a) or '-'}")
                print(f"    aurora missing: {' '.join(only_r) or '-'}")
                bad += 1
                if bad >= 5:
                    print("    (stopping after 5 mismatches)")
                    break
                continue

            if len(path) + 1 >= args.depth:
                continue

            for mv in r:
                nxt = apply_move(fen, mv)
                if nxt:
                    queue.append((nxt, path + [mv]))
    finally:
        ref.close()

    print()
    print(f"examined {examined} positions, {bad} mismatching")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
