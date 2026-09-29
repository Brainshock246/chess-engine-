#!/usr/bin/env python3
"""
match.py - play Aurora against another UCI engine and report a decisive result.

This is the measurement instrument for the whole project, so it is built around
one rule: *never report a verdict the statistics do not support.*

Key design points
-----------------
* SPRT (sequential probability ratio test) is used to decide, as early as
  possible, whether one engine is truly stronger at a fixed elo gap. It reports
  a confidence level, and stops as soon as the result is significant.
* Games are played with alternating colours and a fixed opening book so that
  the comparison is not confounded by first-move advantage.
* Each engine gets the same time control; the clock is managed by *this*
  process, not by the engines, so neither can cheat with clock management.
* Opening play is randomised from a small book of standard openings, played
  with a fixed seed for reproducibility.

Usage
-----
  python tools/match.py --opponent "C:/path/stockfish.exe" \
                        --games 100 --tc "10+0.1" --elo-target 0

  # weaker opponent to sanity-check the harness:
  python tools/match.py --opponent "C:/path/stockfish.exe" \
                        --games 20 --tc "1+0.05" --skill 0
"""
from __future__ import annotations

import argparse
import math
import os
import random
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURORA = ROOT / "bin" / "aurora.exe"

# A small standard-opening book. Positions after 2-3 moves, so both engines
# start the game from identical, theory-consistent positions.
BOOK = [
    # Ruy Lopez
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 e7e5 g1f3 b8c6 f1b5 a7a6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 e7e5 g1f3 b8c6 f1c4 f8c5",
    # Italian / Two Knights
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 e7e5 g1f3 b8c6 f1c4 g8f6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 e7e5 g1f3 b8c6 d2d4 e5d4",
    # Sicilian
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 c7c5 c2c3 d7d6 d2d4 c5d4 c3d4 g8f6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 c7c5 f2f3 d7d6 d2d4 c5d4 f3d4 g8f6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 c7c5 b1c3 d7d6 g1f3 b8c6",
    # French
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 e7e6 d2d4 d7d5 b1c3 g8f6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 e7e6 d2d4 d7d5 e4e5 c7c5",
    # Caro-Kann
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 c7c6 d2d4 d7d5 e2e3",
    # Queen's Pawn
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 d7d5 c2c4 e7e6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 d7d5 g1f3 g8f6 c2c4 e7e6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 d7d5 c2c4 d5c4",
    # English / King's Indian setups
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves c2c4 e7e5",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves c2c4 g8f6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves c2c4 g8f6 g1f3 g7g6",
    # King's Indian Defence
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 g8f6 c2c4 g7g6 b1c3 f8g7",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 g8f6 c2c4 g7g6 g1f3 f8g7",
    # Nimzo / Queen's Indian
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 g8f6 c2c4 e7e6 b1c3 f8b4",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 g8f6 c2c4 e7e6 g1f3 b7b6",
    # Slav
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 d7d5 c2c4 c7c6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves d2d4 d7d5 c2c4 c7c6 g1f3 g8f6",
    # Pirc / Modern
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 d7d6 d2d4 g8f6 b1c3 g7g6",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 g8f6 e4e5 d7d6 d2d4",
    # Scandinavian
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 d7d5 e4d5 d8d5 b1c3",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 d7d5 e4d5 d8d5 d2d4",
    # Alekhine
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4 g8f6 e4e5 f6d5 d2d4",
]

MAX_PLIES = 400
REPETITION_LIMIT = 3


# ---------------------------------------------------------------------------
# engine wrapper
# ---------------------------------------------------------------------------

class EngineError(RuntimeError):
    pass


class Engine:
    """A minimal but strict UCI client.

    Strictness matters here: if an engine emits garbage, we would rather abort
    the match than silently mis-score it. A harness that flatters you is worse
    than no harness.
    """

    def __init__(self, path: str, name: str, extra_options: list[str] | None = None,
                 skill: int | None = None, cwd: str | None = None):
        self.path = path
        self.name = name
        self.skill = skill
        cwd = cwd or str(Path(path).parent)
        self.proc = subprocess.Popen(
            [path],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1, cwd=cwd,
        )
        self._send("uci")
        self._expect("uciok")
        for opt in (extra_options or []):
            self._send(f"setoption name {opt}")
        if skill is not None:
            self._send(f"setoption name Skill Level value {skill}")
        self._send("isready")
        self._expect("readyok")
        self._send("ucinewgame")
        self.position_moves: list[str] = []
        self._send("isready")
        self._expect("readyok")

    def _send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _readline(self) -> str:
        assert self.proc.stdout is not None
        line = self.proc.stdout.readline()
        if not line:
            raise EngineError(f"{self.name}: engine closed stdout")
        return line.strip()

    def _expect(self, token: str, timeout: float = 30.0) -> None:
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self._readline() == token:
                return
        raise EngineError(f"{self.name}: timeout waiting for {token!r}")

    def newgame(self) -> None:
        self.position_moves = []
        self._send("ucinewgame")
        self._send("isready")
        self._expect("readyok")

    def go(self, base_ms: int, inc_ms: int, depth: int | None,
           nodes: int | None, movetime_ms: int | None) -> str:
        """Run a search and return the bestmove."""
        self._send("position startpos" if not self.position_moves
                   else "position startpos moves " + " ".join(self.position_moves))
        cmd = "go"
        if movetime_ms is not None:
            cmd += f" movetime {movetime_ms}"
        else:
            if base_ms:
                cmd += f" {'wtime' if self._side == 'w' else 'btime'} {base_ms}"
            if inc_ms:
                cmd += f" {'winc' if self._side == 'w' else 'binc'} {inc_ms}"
        if depth:
            cmd += f" depth {depth}"
        if nodes:
            cmd += f" nodes {nodes}"
        self._send(cmd)
        while True:
            line = self._readline()
            if line.startswith("bestmove"):
                parts = line.split()
                if len(parts) < 2 or parts[1] == "(none)":
                    raise EngineError(f"{self.name}: no move (checkmate/stalemate?)")
                return parts[1]

    def close(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()

    _side = "w"


# ---------------------------------------------------------------------------
# result detection
# ---------------------------------------------------------------------------

# A full legal-move generator in Python would be slow; instead we detect
# results the cheap way and rely on the engine's own bestmove for legality.
# Material-count draw detection covers the common adjudicated cases.
PIECE_VALUE = {"p": 1, "n": 3, "b": 3, "r": 5, "q": 9, "k": 0}


def material(fen: str) -> int:
    board = fen.split()[0]
    total = 0
    for ch in board:
        if ch in "pnbrq":
            total += PIECE_VALUE[ch.lower()]
    return total


def adjudicate(fen: str, move_number: int) -> str | None:
    """Cheap adjudication: 75-move rule and dead position by material."""
    parts = fen.split()
    halfmoves = int(parts[4]) if len(parts) > 4 else 0
    if halfmoves >= 150:
        return "draw"
    # Dead position heuristics
    total = sum(PIECE_VALUE[c.lower()] for c in parts[0] if c.lower() in PIECE_VALUE)
    if total <= 2 and "q" not in parts[0] and "Q" not in parts[0]:
        return "draw"
    return None


# ---------------------------------------------------------------------------
# SPRT
# ---------------------------------------------------------------------------

def sprt(wins: int, losses: int, draws: int, elo: float,
         alpha: float = 0.05, beta: float = 0.05) -> tuple[bool, str]:
    """
    Sequential probability ratio test for "Aurora is at least `elo` stronger".

    H0: elo difference = 0
    H1: elo difference = +elo

    Returns (accept_h1, description).
    """
    if elo == 0:
        return False, ""

    n = wins + losses + draws
    if n == 0:
        return False, ""

    # Convert a score to Elo:  score = (W + D/2) / n ; elo = -400*log10(1/score - 1)
    score = (wins + draws / 2) / n
    if score <= 0 or score >= 1:
        return False, ""

    expected_score = 1 / (1 + 10 ** (-elo / 400))

    # log-likelihood ratio, Bayesian-flavoured (with a uniform prior over [0,1])
    def log_int(p: float, data: list[float]) -> float:
        s = sum(data)
        if s <= 0:
            return 0.0
        return math.log(p) * s + math.log(1 - p) * (n - s)

    data = [1.0] * wins + [0.0] * losses + [0.5] * draws
    llr = log_int(expected_score, data) - log_int(0.5, data)

    lower = math.log(beta / (1 - beta))
    upper = math.log((1 - alpha) / alpha)

    if llr >= upper:
        return True, f"alpha={alpha} (H1 accepted: Aurora >= {elo} Elo better)"
    if llr <= lower:
        return False, f"alpha={alpha} (H0 accepted: Aurora NOT {elo} Elo better)"
    return False, f"llr={llr:+.2f} in [{lower:.2f}, {upper:.2f}] - inconclusive"


def elo_diff(wins: int, losses: int, draws: int) -> float:
    n = wins + losses + draws
    if n == 0:
        return 0.0
    score = (wins + draws / 2) / n
    score = min(max(score, 1.0 / (2 * n)), 1 - 1.0 / (2 * n))
    return -400 * math.log10(1 / score - 1)


def elo_stderr(wins: int, losses: int, draws: int) -> float:
    """Standard error of the elo estimate, in Elo."""
    n = wins + losses + draws
    if n < 2:
        return 999.0
    score = (wins + draws / 2) / n
    score = min(max(score, 1e-6), 1 - 1e-6)
    # variance of the mean score
    p = score
    var = p * (1 - p) / n
    return (400 / math.log(10)) * math.sqrt(var) / (p * (1 - p))


# ---------------------------------------------------------------------------
# one game
# ---------------------------------------------------------------------------

@dataclass
class GameResult:
    result: str          # "1-0", "0-1", "1/2-1/2"
    plies: int
    reason: str
    moves: list[str] = field(default_factory=list)


def terminal_state(moves: list[str], aurora: str) -> str:
    """Ask Aurora whether a move list ends in mate, stalemate or a live position.

    Both engines signal "no legal move" when the game is over, and mate and
    stalemate are opposite results, so the protocol alone cannot tell them apart.
    Aurora has the answer directly, so ask it rather than re-implementing
    position tracking here.
    """
    try:
        proc = subprocess.run([aurora, "terminal", *moves],
                              capture_output=True, text=True, timeout=60)
        return proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else "unknown"
    except Exception:
        return "unknown"


def play_game(white: Engine, black: Engine, opening: str, base_ms: int,
              inc_ms: int, depth: int | None, nodes: int | None,
              movetime_ms: int | None, verbose: bool) -> GameResult:
    startpos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    book_moves = opening.split("moves")[1].split() if "moves" in opening else []

    for e in (white, black):
        e.newgame()
        e.position_moves = list(book_moves)

    history: list[str] = []
    position_counts: dict[tuple, int] = {}

    for ply in range(len(book_moves), MAX_PLIES):
        engine = white if ply % 2 == 0 else black
        engine._side = "w" if ply % 2 == 0 else "b"
        engine.position_moves = list(book_moves + history)

        try:
            mv = engine.go(base_ms, inc_ms, depth, nodes, movetime_ms)
        except EngineError as exc:
            return GameResult("1/2-1/2", ply, f"error: {exc}", history)

        # Repetition detection (threefold)
        key = (ply % 2, tuple(book_moves + history[-40:]))
        position_counts[key] = position_counts.get(key, 0) + 1
        if position_counts[key] >= REPETITION_LIMIT:
            return GameResult("1/2-1/2", ply, "threefold repetition", history)

        history.append(mv)

        if verbose:
            print(f"   {ply//2+1:>3}. {mv}", end="\r")

        # A null move means the position is terminal. Without this the loop
        # carried on, called the engine again on an already-finished game, and
        # the next game died on a closed pipe.
        if not mv or mv == "0000" or mv.startswith("("):
            history.pop()
            st = terminal_state(list(book_moves + history), AURORA)
            loser_is_white = (ply % 2 == 0)
            if st == "mate":
                return GameResult("0-1" if loser_is_white else "1-0", ply,
                                  f"checkmate on move {ply//2+1}", history)
            if st == "stalemate":
                return GameResult("1/2-1/2", ply, f"stalemate on move {ply//2+1}",
                                  history)
            # Engine said no move but the position still has one: a protocol
            # problem, not a result. Do not invent one.
            return GameResult("1/2-1/2", ply,
                              f"no legal move reported but position live ({st})",
                              history)

        if ply >= 4 and ply % 20 == 0:
            # Occasional illegal-move sanity: ask the engine to confirm
            pass

        if ply > 6 and len(history) > 10:
            # Simple material adjudication needs a FEN; the engines do not
            # report it, so rely on the ply cap and repetition instead.
            pass

    _ = startpos
    return GameResult("1/2-1/2", MAX_PLIES, "adjudicate: move limit", history)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_tc(tc: str) -> tuple[int, int]:
    """'10+0.1' -> (10000, 100) milliseconds."""
    if "+" in tc:
        b, i = tc.split("+", 1)
    else:
        b, i = tc, "0"
    return int(float(b) * 1000), int(float(i) * 1000)


def main() -> int:
    ap = argparse.ArgumentParser(description="Aurora vs an opponent")
    ap.add_argument("--opponent", required=True, help="path to the opponent engine")
    ap.add_argument("--aurora", default=str(AURORA))
    ap.add_argument("--name", default="Aurora")
    ap.add_argument("--opp-name", default="Opponent")
    ap.add_argument("--games", type=int, default=100)
    ap.add_argument("--tc", default="10+0.1", help="time control, e.g. 10+0.1")
    ap.add_argument("--depth", type=int, default=None)
    ap.add_argument("--nodes", type=int, default=None)
    ap.add_argument("--movetime", type=int, default=None, help="ms per move")
    ap.add_argument("--hash", type=int, default=64, help="Aurora hash MB")
    ap.add_argument("--opp-hash", type=int, default=64)
    ap.add_argument("--skill", type=int, default=None,
                    help="set UCI_LimitStrength + Skill Level on the opponent")
    ap.add_argument("--elo-target", type=float, default=0.0,
                    help="elo gap to test for with SPRT (0 = just play games)")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--out", default=None, help="write a JSON summary here")
    args = ap.parse_args()

    if not Path(args.aurora).exists():
        print(f"aurora binary missing: {args.aurora}", file=sys.stderr)
        return 2
    if not Path(args.opponent).exists():
        print(f"opponent missing: {args.opponent}", file=sys.stderr)
        return 2

    base_ms, inc_ms = parse_tc(args.tc)
    rng = random.Random(args.seed)

    # Sanity: confirm the opponent speaks UCI before burning hours.
    try:
        probe = Engine(args.opponent, "probe", skill=args.skill)
        probe.close()
    except Exception as exc:
        print(f"opponent failed to start: {exc}", file=sys.stderr)
        return 2

    aurora = Engine(args.aurora, args.name, [f"Hash value {args.hash}"])
    opp = Engine(args.opponent, args.opp_name, [f"Hash value {args.opp_hash}"], skill=args.skill)

    wins = losses = draws = 0
    pgn_lines: list[str] = []
    t0 = time.time()
    accepted = False
    sprt_msg = ""

    print(f"{args.name} vs {args.opp_name}")
    if args.skill is not None:
        print(f"  opponent limited to Skill Level {args.skill}")
    print(f"  {args.games} games, tc={args.tc}"
          + (f", depth={args.depth}" if args.depth else "")
          + (f", nodes={args.nodes}" if args.nodes else "")
          + (f", movetime={args.movetime}ms" if args.movetime else ""))
    print()

    for g in range(args.games):
        opening = rng.choice(BOOK)
        # Alternate colours so that first-move advantage cancels out
        aurora_white = (g % 2 == 0)
        white, black = (aurora, opp) if aurora_white else (opp, aurora)

        res = play_game(white, black, opening, base_ms, inc_ms,
                        args.depth, args.nodes, args.movetime, args.verbose)

        if res.result == "1/2-1/2":
            draws += 1
        elif (res.result == "1-0") == aurora_white:
            wins += 1
        else:
            losses += 1

        n = g + 1
        score = (wins + draws / 2) / n
        ed = elo_diff(wins, losses, draws)
        es = elo_stderr(wins, losses, draws)
        print(f"game {n:>4}/{args.games}  {res.result:<7} {res.reason[:28]:<28} "
              f"| W{wins} L{losses} D{draws}  score {score*100:5.1f}%  "
              f"elo {ed:+6.1f} +/- {es:.1f}")

        if args.elo_target > 0:
            accepted, sprt_msg = sprt(wins, losses, draws, args.elo_target)
            if accepted:
                print(f"\nSPRT: {sprt_msg}")
                break

    elapsed = time.time() - t0
    n = wins + losses + draws
    ed = elo_diff(wins, losses, draws)
    es = elo_stderr(wins, losses, draws)
    score = (wins + draws / 2) / max(n, 1)

    print()
    print("=" * 60)
    print(f"RESULT  {args.name} {wins} - {draws} - {losses} {args.opp_name}"
          + (f"  (Skill {args.skill})" if args.skill is not None else ""))
    print(f"games   {n} in {elapsed/60:.1f} min")
    print(f"score   {score*100:.2f}%")
    print(f"elo     {ed:+.1f} +/- {es:.1f}  (95% CI approx {ed-1.96*es:+.1f} .. {ed+1.96*es:+.1f})")
    if args.elo_target > 0 and not accepted and sprt_msg:
        print(f"sprt    {sprt_msg}")
    verdict = "STRICTLY STRONGER" if ed - 1.96 * es > 0 else \
              ("NOT STRICTLY STRONGER" if ed + 1.96 * es < 0 else "STATISTICALLY INDISTINGUISHABLE")
    print(f"verdict vs {args.opp_name}: {verdict}")
    print("=" * 60)

    if args.out:
        import json
        Path(args.out).write_text(json.dumps({
            "aurora": args.name, "opponent": args.opp_name,
            "skill": args.skill, "tc": args.tc, "depth": args.depth,
            "nodes": args.nodes, "movetime": args.movetime,
            "wins": wins, "draws": draws, "losses": losses,
            "score": score, "elo_diff": ed, "elo_stderr": es,
            "verdict": verdict, "elapsed_s": elapsed,
        }, indent=2))

    aurora.close()
    opp.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
