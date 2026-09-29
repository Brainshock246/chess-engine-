#!/usr/bin/env python3
"""
divdiff.py - compare Aurora's perft divide against Stockfish's for one position
and print exactly which root moves disagree.

  python tools/divdiff.py --fen "<fen>" --depth 2
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURORA = ROOT / "bin" / "aurora.exe"
DEFAULT_REF = Path(r"C:\Users\surek\chess\stockfish\sf19\stockfish\stockfish-windows-x86-64-universal.exe")


def aurora_divide(fen: str, depth: int) -> dict[str, int]:
    out = subprocess.run([str(AURORA), "perft", fen, str(depth)],
                         capture_output=True, text=True, timeout=7200)
    d: dict[str, int] = {}
    for line in out.stdout.splitlines():
        if ":" in line and "Nodes" not in line:
            mv, n = line.split(":", 1)
            mv = mv.strip()
            if len(mv) == 4 and n.strip().isdigit():
                d[mv] = int(n.strip())
    return d


class Ref:
    def __init__(self, path: str):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  cwd=str(Path(path).parent))
        self._s("uci"); self._e("uciok")
        self._s("isready"); self._e("readyok")

    def _s(self, s: str) -> None:
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def _e(self, tok: str) -> None:
        while True:
            if self._r() == tok:
                return

    def _r(self) -> str:
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
                if len(mv) == 4 and n.strip().isdigit():
                    out[mv] = int(n.strip())
        return out

    def close(self) -> None:
        try:
            self._s("quit"); self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def aurora_moves(fen: str) -> list[str]:
    out = subprocess.run([str(AURORA), "moves", fen], capture_output=True, text=True, timeout=120)
    return sorted(ln.strip() for ln in out.stdout.splitlines() if len(ln.strip()) == 4)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fen", required=True)
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--ref", default=str(DEFAULT_REF))
    ap.add_argument("--drill", action="store_true",
                    help="for each disagreeing root move, show the child move diff")
    args = ap.parse_args()

    ref = Ref(args.ref)
    try:
        r = ref.divide(args.fen, args.depth)
        a = aurora_divide(args.fen, args.depth)
        print(f"fen: {args.fen}")
        print(f"aurora total: {sum(a.values())}  ({len(a)} root moves)")
        print(f"reference total: {sum(r.values())}  ({len(r)} root moves)")

        only_a = sorted(set(a) - set(r))
        only_r = sorted(set(r) - set(a))
        if only_a:
            print(f"root moves only in aurora: {' '.join(only_a)}")
        if only_r:
            print(f"root moves only in reference: {' '.join(only_r)}")

        bad = [mv for mv in sorted(r) if mv in a and a[mv] != r[mv]]
        for mv in bad:
            print(f"  DIFF {mv}: aurora={a[mv]} reference={r[mv]}")
            if args.drill:
                child = subprocess.run([str(AURORA), "after", args.fen, mv],
                                       capture_output=True, text=True, timeout=120).stdout.strip()
                child = child.replace("AFTER ", "", 1)
                print(f"    child fen: {child}")
                cr = ref.divide(child, args.depth - 1)
                ca = aurora_divide(child, args.depth - 1)
                ea = sorted(set(ca) - set(cr))
                er = sorted(set(cr) - set(ca))
                print(f"    child totals: aurora={sum(ca.values())} ref={sum(cr.values())}")
                print(f"    child only in aurora : {' '.join(ea) or '-'}")
                print(f"    child only in reference: {' '.join(er) or '-'}")
        if not bad and not only_a and not only_r:
            print("  no divergence at this depth")
    finally:
        ref.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
