#!/usr/bin/env python3
"""
evalsym.py - find which evaluation term breaks colour symmetry.

A perfectly symmetric position (the start position, with the side to move
flipped) must evaluate to the same magnitude for either side. Any large
discrepancy means a term is being computed for one colour only, or indexed
with the wrong mirror, and it shows up directly as a bogus hundreds-of-
centipawns bias against the side to move.

This drives the engine over its stdin protocol, zeroing one weight group at a
time in a single process, so each hypothesis costs a round trip rather than a
rebuild. The piece-square tables are 832 of the weights but only a handful of
distinct terms, so they are grouped by piece type.

  python tools/evalsym.py
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURORA = ROOT / "bin" / "aurora.exe"

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"
WHITE_TO_MOVE = f"{START} w KQkq - 0 1"
BLACK_TO_MOVE = f"{START} b KQkq - 0 1"
TEMPO = 14          # must match weights.h tempo_bonus


class Eng:
    def __init__(self) -> None:
        self.p = subprocess.Popen([str(AURORA)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  text=True, bufsize=1, cwd=str(ROOT))
        self.banner = self._r()
        self.banner = self._r()

    def _r(self) -> str:
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("engine closed unexpectedly")
        return line.strip()

    def send(self, s: str) -> None:
        """Send a command that produces no output."""
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def ask(self, s: str) -> str:
        """Send a command and read exactly one line of reply."""
        self.send(s)
        return self._r()

    def eval(self, fen: str) -> int:
        # "position" produces no output, so it must not be sent with ask().
        self.send(f"position fen {fen}")
        return int(self.ask("eval").split()[-1])

    def weights(self) -> list[tuple[str, int]]:
        self.send("weights")
        n = int(self._r().split()[1])
        out = []
        for _ in range(n):
            _, name, val = self._r().split("\t")
            out.append((name, int(val)))
        return out

    def setw(self, name: str, v: int) -> None:
        self.send(f"setweight {name} {v}")

    def close(self) -> None:
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def main() -> int:
    if not AURORA.exists():
        print(f"missing {AURORA}", file=sys.stderr)
        return 2

    eng = Eng()
    try:
        weights = eng.weights()
        print(f"{len(weights)} tunable weights\n")

        def asymmetry() -> int:
            """Disagreement between the two sides, net of the tempo term.

            White to move is credited +tempo and Black to move -tempo, so the
            two evaluations should sum to exactly zero. Anything else is a
            colour asymmetry in the positional terms.
            """
            return eng.eval(WHITE_TO_MOVE) + eng.eval(BLACK_TO_MOVE)

        base = asymmetry()
        w0, b0 = eng.eval(WHITE_TO_MOVE), eng.eval(BLACK_TO_MOVE)
        print(f"white to move: {w0:+5d}   black to move: {b0:+5d}   "
              f"asymmetry: {base:+5d} cp\n")

        # Group the per-square tables so 832 weights become a handful of tests.
        groups: dict[str, list[tuple[str, int]]] = {}
        for name, val in weights:
            key = "_".join(name.split("_")[:3]) + "_psqt" if "_psqt_" in name else name
            groups.setdefault(key, []).append((name, val))

        print(f"{'term':<26} {'asymmetry with term zeroed':>29} {'delta':>8}")
        print("-" * 66)
        results = []
        for key, items in sorted(groups.items()):
            for name, val in items:
                eng.setw(name, 0)
            a = asymmetry()
            for name, val in items:
                eng.setw(name, val)
            results.append((a - base, key, a))
            print(f"{key:<26} {a:>29} {a - base:>+8}")

        results.sort(key=lambda t: -abs(t[0]))
        print("\nlargest contributions to the asymmetry:")
        for delta, key, a in results[:12]:
            print(f"  {key:<26} {delta:>+7} cp")
        return 0
    finally:
        eng.close()


if __name__ == "__main__":
    sys.exit(main())
