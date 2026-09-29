#!/usr/bin/env python3
"""
narrow.py - localise a perft divergence by bisection, in a handful of queries.

A brute-force differential walk (tools/difftest.py) has to spawn a process per
position, which is far too slow at depth 3+. Bisecting on the *divide* output
instead is logarithmic: compare the per-root counts at depth N, take the first
root move whose subtree disagrees, descend into it, repeat. Each level costs one
query to Stockfish and one to Aurora.

Both engines' divide output is used as the ground truth for the children, and
the child FEN is taken from Aurora, so a FEN-parsing disagreement can never be
mistaken for a movegen bug.

  python tools/narrow.py --fen "<fen>" --depth 3
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURORA = ROOT / "bin" / "aurora.exe"
DEFAULT_REF = Path(r"C:\Users\surek\chess\stockfish\sf19\stockfish\stockfish-windows-x86-64-universal.exe")


class Ref:
    def __init__(self, path: str):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  cwd=str(Path(path).parent))
        self._s("uci"); self._e("uciok")
        self._s("isready"); self._e("readyok")

    def _s(self, s: str) -> None:
        assert self.p.stdin
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def _e(self, tok: str) -> None:
        while True:
            if self._r() == tok:
                return

    def _r(self) -> str:
        assert self.p.stdout
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("reference closed")
        return line.strip()

    def divide(self, fen: str, depth: int) -> dict[str, int]:
        self._s(f"position fen {fen}")
        self._s(f"go perft {depth}")
        out: dict[str, int] = {}
        while True:
            line = self._r()
            if line.startswith("Nodes searched"):
                break
            if ":" in line:
                mv, n = line.split(":")
                mv = mv.strip()
                if len(mv) == 4:
                    out[mv] = int(n)
        return out

    def close(self) -> None:
        try:
            self._s("quit"); self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def aurora_divide(fen: str, depth: int) -> dict[str, int]:
    out = subprocess.run([str(AURORA), "perft", fen, str(depth)],
                         capture_output=True, text=True, timeout=7200)
    d: dict[str, int] = {}
    for line in (out.stdout + out.stderr).splitlines():
        if ":" in line and "Nodes" not in line:
            mv, n = line.split(":", 1)
            mv = mv.strip()
            if len(mv) == 4:
                d[mv] = int(n.strip())
    return d


def aurora_after(fen: str, mv: str) -> str:
    p = subprocess.run([str(AURORA), "after", fen, mv],
                       capture_output=True, text=True, timeout=120)
    for line in (p.stdout + p.stderr).splitlines():
        if line.startswith("AFTER "):
            return line.split(" ", 1)[1].strip()
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fen", required=True)
    ap.add_argument("--depth", type=int, required=True)
    ap.add_argument("--ref", default=str(DEFAULT_REF))
    args = ap.parse_args()

    if not AURORA.exists():
        print(f"missing {AURORA}", file=sys.stderr)
        return 2

    ref = Ref(args.ref)
    fen, depth = args.fen, args.depth
    try:
        while depth > 1:
            r = ref.divide(fen, depth)
            a = aurora_divide(fen, depth)
            bad = None
            for mv in sorted(r):
                if mv in a and a[mv] != r[mv]:
                    bad = (mv, a[mv], r[mv])
                    break
                if mv not in a:
                    bad = (mv, None, r[mv])
                    break
            for mv in sorted(a):
                if mv not in r:
                    bad = (mv, a[mv], None)
                    break
            if bad is None:
                print(f"MATCH at depth {depth}: {fen}")
                print(f"  perft({depth}) = {sum(r.values())}")
                return 0
            mv, ag, rg = bad
            print(f"depth {depth} in {fen}")
            print(f"  divergence at move {mv}: aurora={ag} reference={rg}")
            if rg is None:
                print("  -> Aurora generates a move the reference does not.")
                return 1
            if ag is None:
                print("  -> Aurora is missing this move entirely.")
                return 1
            nxt = aurora_after(fen, mv)
            if not nxt:
                print("  -> could not obtain the child FEN from Aurora")
                return 2
            # Sanity: the reference must agree about the child position
            if ref.divide(nxt, 1) == {} and depth > 2:
                print(f"  child FEN rejected by reference: {nxt}")
                return 2
            fen, depth = nxt, depth - 1

        # At the leaf, name the offending move rather than making the reader
        # transcribe a FEN and diff two move lists by hand.
        r = ref.divide(fen, 1)
        a = aurora_divide(fen, 1)
        extra = sorted(set(a) - set(r))
        missing = sorted(set(r) - set(a))
        print(f"leaf position: {fen}")
        print(f"  reference perft(1)={sum(r.values())}  aurora perft(1)={sum(a.values())}")
        print(f"  extra (illegal in Aurora): {' '.join(extra) or '-'}")
        print(f"  missing (absent in Aurora): {' '.join(missing) or '-'}")
    finally:
        ref.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
