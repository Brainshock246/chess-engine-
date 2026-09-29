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


# ---------------------------------------------------------------------------
# Sweep mode
# ---------------------------------------------------------------------------
#
# THE INVARIANT, which I have now got wrong twice:
#
#   evaluate() is from the SIDE TO MOVE's point of view, and tempo is credited to
#   whoever is to move. So for one board:
#
#       eval(w to move) + eval(b to move)  ==  2 * tempo        (28 by default)
#
# The two values must SUM to 2*tempo. They must NOT be equal to each other - a
# position where White is a queen down reads about -1000 for White and +1000 for
# Black, and both are correct. Differencing them flags almost every real position,
# which is exactly what happened when this check was first written by hand.
#
# Noise is +/-3cp per evaluation, so +/-6 is the slack before a discrepancy is
# real. Anything larger is a term being computed for one colour only, or - the
# defect this caught - a position-dependent quantity being read off the mover.

SWEEP = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR",          # symmetric start
    "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR",      # 1.e4 c5
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R",  # italian
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8",                      # rook endgame
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R",   # kiwipete
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1",   # promotions
    "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N",                        # promotion race
    "3rkr2/8/8/8/8/8/3RKR2/8",                              # rooks and king only
    # --- lopsided: the two sides have very different material, which is where a
    # --- position-dependent quantity read off the mover shows up.
    "8/8/8/8/8/5k2/6q1/7K",                                 # K vs KQ  (was 39cp)
    "4k3/8/8/8/8/8/4r3/4K3",                                # K+R vs K
    "8/8/1k6/2b5/2pP4/8/5K2/8",                             # opposite bishops
    "8/2p5/8/1P6/8/8/8/k1K5",                               # opposite pawns
    "8/1P6/8/8/8/8/K6k/8",                                  # pawn about to promote
    "4k3/8/8/2pP4/8/8/8/4K3",                               # blocked pawns
    "4k3/8/8/8/8/8/4P3/4K3",                                # K+P vs K
]

TEMPO_EXPECTED = 28   # 2 * tempo_bonus; must match weights.h
TOLERANCE = 6         # noise is +/-3 per evaluation


def sweep(aurora: str = str(AURORA)) -> int:
    exe = Path(aurora)
    if not exe.exists():
        print(f"missing {exe}", file=sys.stderr)
        return 2

    def ev(board: str, side: str) -> int:
        proc = subprocess.run([str(exe), "eval", f"{board} {side} - - 0 1"],
                              capture_output=True, text=True, timeout=120)
        return int(proc.stdout.strip())

    print("Colour-symmetry sweep")
    print(f"invariant: eval(w to move) + eval(b to move) == {TEMPO_EXPECTED} "
          f"+/-{TOLERANCE}\n")
    failures = 0
    for board in SWEEP:
        w, b = ev(board, "w"), ev(board, "b")
        err = w + b - TEMPO_EXPECTED
        bad = abs(err) > TOLERANCE
        failures += bool(bad)
        print(f"  {'FAIL' if bad else 'OK  '} {w:>6} {b:>6}  err {err:>+4}"
              f"{'   <-- a term depends on the mover, not the position' if bad else ''}"
              f"   {board}")

    print()
    if failures:
        print(f"{failures} position(s) disagree. A position evaluated twice, with")
        print("different sides to move, must not change - unless something in the")
        print("taper or a term is being read off the mover instead of the position.")
    else:
        print("all positions symmetric")
    return 1 if failures else 0


def main() -> int:
    if not AURORA.exists():
        print(f"missing {AURORA}", file=sys.stderr)
        return 2

    if "--sweep" in sys.argv:
        i = sys.argv.index("--sweep")
        return sweep(sys.argv[i + 1] if len(sys.argv) > i + 1 else str(AURORA))

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
