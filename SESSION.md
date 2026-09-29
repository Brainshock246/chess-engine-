# Aurora — session log

A running record of what was done, what was measured, and what is left. Written as
work happened, not reconstructed afterwards, so the mistakes stay visible.

---

## 1. The move generator (the gate)

Nothing about strength can be measured until the tree being searched is provably
the right tree. This took the bulk of the effort and fixed **twenty** distinct
correctness bugs.

Escalation was deliberate: perft(1) → perft(4) → perft(6) across six positions,
because each level of depth exposes a class the level above cannot. A position can
agree with the reference for 20 moves and still hide a rook pin.

Result: startpos perft(6) = 119,060,324 nodes, exact. Full table in `RESULTS.md`.

Two tools earned their keep:

- **`tools/narrow.py`** — compares both engines' perft *divide* output and
  bisects. Logarithmic: a depth-5 discrepancy is localised in a handful of calls
  instead of walking the tree. This is what turned "kiwipete depth 4 is 302 nodes
  too high" into "f6h7 is legal in Aurora and must not be".
- **`tools/divdiff.py`** — per-root-move diff with drill-down.

I also twice learned that **removing an optimisation** exposes a bug that it was
concealing: generating all adjacent king squares instead of filtering the check ray
immediately exposed a missing `Kxf7`; rebuilding `LineBB` from geometry rather than
a bitwise identity exposed that the identity picks up the corners of the bounding
rectangle.

---

## 2. The evaluation (measured, not assumed)

Perft being green says nothing about move choice, so the evaluation was audited
before any game was played. Eight defects, each found by measurement:

| # | defect | visible as |
|---|---|---|
| 1 | king-safety weights ~1000× too large, then squared | score clamps at ±9999 |
| 2 | positional terms added for the mover only, never subtracted | start position +538 |
| 3 | `mg_psqt`/`eg_psqt` declared `[PIECE_NB]` with 8 rows — **every Black table zero** | start position −829 |
| 4 | passed-pawn test compared a *relative* rank against an *absolute* one | all 8 black starting pawns "passed", +1532 for Black |
| 5 | passed-pawn bonus indexed by raw square | Black read the wrong table row |
| 6 | game phase summed to 12, taper divided by 24 | middlegame tables at half weight in the opening |
| 7 | tempo added before the taper | tempo vanished in endgames |
| 8 | king's attack reach seeded into every piece's attack set | −300cp on a symmetric board |

After all eight the start position evaluates to +15 for either side to move, and a
per-section colour audit shows **zero** difference in every section.

---

## 3. The engine could not accept a `position` command

`UCI::position` re-skipped the command word `loop()` had already consumed, so the
*placement* arrived where `startpos`/`fen` was expected. Both branches were skipped
and `set("")` installed an empty board — then `king_square()` = `lsb(0)` = **64**,
one past the board, indexing every attacker table out of bounds.

Found with gdb; the error message then printed the offending token verbatim and
exposed the double-skip.

---

## 4. The search was not searching

```cpp
const int pvCount = std::min(multiPV_, rootMoveCount_);   // multiPV_ == 1
```

The root searched **one move** — the first in generation order. `b1a3` from the
start position at every depth, because `b1a3` is the first move Aurora generates.
The evaluation ranks Nf3 first (+86) and Na3 fourth (+38); the engine was returning
a move before consulting it.

**Why it survived:** every check built beforehand was about *internal consistency*
— perft, make/unmake round-trips, evaluation symmetry, score convergence, node
counts, PV validity. All pass on an engine that picks a move before looking at
anything. `tools/tactics.py`, which asks "can it find a mate / take the free
queen", failed on its second case and caught it.

Fixing it exposed, in order:

- **the root never updated `alpha`** — all moves got full-window searches, no
  pruning. Depth 8: 18M nodes / 16s → 291k / 164ms with PVS.
- **`qsearch` returned `Value(alpha)`** — a window bound returned as an evaluation,
  compounding one ply per level. This produced scores that *decayed with depth* in
  positions whose material never changed.
- **an inverted ProbCut window** — `(-alpha-1, -alpha-210)` has `beta` 209cp *below*
  `alpha`, and fired from `PROBCUT_MIN_DEPTH = 5`. A queen-down endgame was correct
  at depths 1–4 and sign-inverted from depth 5.
- **mate scores shifted by ply on store, never shifted back on retrieval.**

The last two were found only after isolating a *minimal* reproduction. The step that
cracked it was not insight: stop testing the position that revealed the symptom and
test the smallest position that still shows it. From "a position I cannot lose
scores as though I have won" to "the threshold is 5, and exactly one constant equals
5" was a single step once the reproduction was minimal.

All speculative pruning now sits behind `constexpr bool SPECULATIVE_PRUNING = false`
(null move, reverse futility, razoring, IIR, singular extensions, LMR, futility,
ProbCut). The remaining search is alpha-beta with a transposition table, quiescence,
killers and history.

---

## Where this stands

| | |
|---|---|
| move generation | verified, perft(6) from startpos, 119,060,324 nodes |
| make/unmake, legality | verified, FEN round-trip on every legal move |
| UCI | working — `position` (fen / startpos / moves), `go`, `bestmove` |
| evaluation | symmetric, sane magnitudes, audited per section |
| search | scores stable across depth; no sign inversions; converges |
| tactics | `tools/tactics.py` 7 passed / 0 failed; `tools/scores.py` passes |
| time management | fixed — `movestogo` honoured, budget computed for the side to move, verified colour-symmetric (243ms vs 201ms on the same position) |
| git | repo initialised, first commit `114f98e` |
| strength | **unmeasured** — baseline match running |

## Next

1. **Use Stockfish as a teacher for the evaluation.** Score positions from games
   Aurora actually lost with both engines and rank by disagreement; the worst
   offenders are specific, nameable defects. This is the redirect the A/B forced —
   depth bought nothing, so the evaluation is the bottleneck. Positions must come
   from real losses (`match.py --pgn`), not a random set.
2. **Fix eval terms in disagreement order**, re-running `evalsym.py --sweep` and
   `tools/scores.py` after each, then re-matching.
3. **Measure NOISE and SPARK individually** once there is an eval worth measuring
   them against. They are implemented and isolated at defaults nobody has
   justified.
4. **Publish.** `gh` is installed; `gh auth login` is the last step.
