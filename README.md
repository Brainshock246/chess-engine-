# Aurora

A chess engine written from scratch in C++20, built to play continuous games
against Stockfish and to be measured honestly against it.

## Status

| stage | state |
|---|---|
| move generation | **verified** — 6 positions, perft to depth 6 (119,060,324 nodes from the start position) |
| make/unmake, legality | **verified** — round-trip FEN check on every legal move in six positions |
| UCI protocol | **working** — accepts `position` (fen, startpos, moves), searches, emits `bestmove` |
| evaluation | **symmetric and sane** — verified by a per-section colour audit; queen ≈ +995, rook ≈ +529 |
| search | **works** — every root move is searched; the score converges with no aspiration thrash |
| tactics | `tools/tactics.py` is the suite; writing it is what exposed that the engine had not been searching at all |
| strength | **unmeasured** — no match has yet been played |

`RESULTS.md` is the real document: every change, the measurement that justified
it, and the bugs found along the way.

## Building

Requires a C++20 compiler with `<bit>` (`__builtin_popcountll` etc.). Tested with
GCC 16.2 on MinGW-w64.

```sh
g++ -O3 -march=native -std=c++20 -DNDEBUG -Isrc \
    src/*.cpp -o aurora
```

Drop `-DNDEBUG` for an assertion-enabled build.

## Command line

Beyond UCI, the engine carries its own diagnostics, because the tools that find
bugs should not be able to drift out of sync with the engine they describe:

| command | purpose |
|---|---|
| `aurora perft "<fen>" <depth>` | node count; `perftcheck` cross-checks against a slow generator |
| `aurora moves "<fen>"` | the legal move list |
| `aurora info "<fen>"` | FEN round-trip, key, checkers, pin blockers, and every rejected pseudo-legal move with the reason |
| `aurora evaldump "<fen>"` | per-side accumulators **and** a per-section, per-colour breakdown |
| `aurora eval "<fen>"` | static evaluation only |
| `aurora terminal [moves...]` | replay a move list from the start position: `mate`, `stalemate` or `ok` |
| `aurora weights` | dump every tunable weight |
| `aurora after "<fen>" <move>` | FEN after playing a move (used by the tree-walking tools) |

## Tools

| tool | purpose |
|---|---|
| `tools/narrow.py` | bisect a perft divergence to a single illegal move, logarithmically, using both engines' divide output |
| `tools/divdiff.py` | per-root-move comparison of the two engines, with an optional drill-down |
| `tools/difftest.py` | breadth-first walk comparing every legal move list against Stockfish |
| `tools/evalsym.py` | attribute a colour asymmetry in the evaluation to a specific term, by zeroing one weight group at a time |
| `tools/tactics.py` | search regression suite: forced mates and captures, and blunder traps it must avoid |
| `tools/scores.py` | score-sanity suite: a score must not change sign or decay with depth in a position that cannot be won or lost |
| `tools/perft.py` | the standard perft suite |
| `tools/match.py` | SPRT match runner with an opening book and adjudication |

Each tool answers a question the others cannot:

- **perft** — is the tree right? (move generation)
- **evalsym / evaldump** — is the evaluation symmetric, and which term breaks it?
- **scores** — is the *score* right, and does it survive depth? (search)
- **tactics** — can the search *find* something that is there?
- **match.py** — is it actually any good?

The guiding idea in all of them: **never guess which of a hundred terms is
wrong — make the engine say so.** Each one turns "the score is off by 1,000,000
nodes" into "this specific move is illegal" or "this specific term contributes
+1580 to Black and 0 to White".

A warning learned the hard way: a diagnostic is only as good as the question it
asks. An earlier symmetry checker reported "15cp residual, every term contributes
zero" — and was blind by construction to the very bug I was hunting, because its
metric cancelled out precisely what it was supposed to detect.

## Design notes

- **Single-threaded search, deliberately.** It keeps every search bit-for-bit
  deterministic, which is the precondition for the A/B measurements that any
  strength claim rests on.
- **Magic bitboards** are searched offline by `tools/genmagics.cpp`, baked into
  `src/magics.h`, and re-verified exhaustively at startup — every square, every
  occupancy subset — with a runtime search as a fallback.
- **The evaluation is a per-colour function, differenced.** Every positional term
  is computed for both sides. Scoring "my features" rather than "my advantage"
  was worth several hundred centipawns of pure mover bias before it was caught.
- **Piece-square tables are indexed by piece type and mirrored**, never stored
  per colour. A per-colour array had been silently half-written; a mirrored table
  cannot be.

## Two ideas of our own

Both are isolated in the evaluation and individually measurable via
`tools/evalsym.py` and `match.py`:

- **NOISE** — a deterministic dither derived from the position hash, not a random
  number. The same position always evaluates identically, so search stays
  reproducible, but it breaks the "evaluation is a staircase" artefact at shallow
  depth and gives genuinely different move choices among equal alternatives.
- **SPARK** — a measure of pressure we have *created*: enemy pieces that are
  defended by their own side yet attacked by two or more of ours. Not a mobility
  term; it rewards threats that outlive the current search.

Both are measured against Stockfish rather than assumed to help.
