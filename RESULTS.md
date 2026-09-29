# Aurora — results log

Every change is recorded here with the measurement that justified it. A change
without a number next to it is a guess.

Measurement ladder, cheapest first:
1. `aurora info "<fen>"` — pin/checker/blocker state, pseudo vs legal counts
2. `aurora perft "<fen>" <d>` — node counts
3. `python tools/divdiff.py` — which root moves disagree, with a drill-down
4. `python tools/narrow.py` — bisect a divergence down to a single illegal move
5. `python tools/difftest.py` — breadth-first walk comparing every move list

All perft expectations are the standard published values, and every position is
cross-checked against Stockfish 19's own perft divide.

---

## Correctness: the perft ladder

Final state — every case green. This is the gate everything else depends on: a
search over a move generator that is subtly wrong measures nothing.

| position | depth | nodes | expected |
|---|---|---|---|
| startpos | 1–6 | 20 / 400 / 8 902 / 197 281 / 4 865 609 / 119 060 324 | all OK |
| kiwipete | 1–4 | 48 / 2 039 / 97 862 / 4 085 603 | all OK |
| pos3 | 1–6 | 14 / 191 / 2 812 / 43 238 / 674 624 / 11 030 083 | all OK |
| pos4 (promotions) | 1–4 | 6 / 264 / 9 467 / 422 333 | all OK |
| pos5 | 1–4 | 44 / 1 486 / 62 379 / 2 103 487 | all OK |
| pos6 | 1–4 | 46 / 2 079 / 89 890 / 3 894 594 | all OK |

Six positions covering sliding attacks, castling both ways, en passant, promotion,
pins and discovered checks, at depths deep enough (119 million nodes from the
start position) that no residual movegen error could hide.

Getting here meant fixing **twenty** distinct correctness bugs, listed below. The
recurring lesson is that almost none of them were "wrong chess knowledge" — they
were near-miss table definitions and colour inversions that a plausible-looking
implementation produces happily:

- a "between" table that only described diagonals, and a "line" table that was
  wrong in two *different* ways on either side of being fixed;
- `pawn_attacks_bb` shifting twice, so pawns attacked two ranks away;
- a queen that generated rook moves;
- en passant that existed only in the check-evasion path, and then a square-set
  test using the wrong colour — twice, in opposite directions;
- castling that tested White's back rank for Black, and castled straight through
  its own knight;
- a promotion check that matched any piece reaching rank 1, so a king travelling
  along the back rank came back as a pawn — which is how a logic bug became an
  access violation.

Two of these were found only because I *removed* an optimisation rather than
adding code: generating all adjacent king squares instead of filtering the check
ray immediately exposed the missing `Kxf7`, and rebuilding `LineBB` from geometry
rather than a clever bitwise identity exposed the bounding-rectangle corners. The
optimised versions were not faster in any way that mattered here and each one
concealed a correctness bug.

---

## Evaluation: from unsound to sane

Perft being green says the engine can enumerate the tree correctly. It says
nothing about whether it can *choose* well, and the first thing measured here was
the static evaluator — which had never been tested at all.

### The two failures, and what each looked like from the outside

**Saturating king safety.** The attack term weighted each enemy piece by a
"distance factor" expressed as a *percentage*, multiplying a weight of several
hundred by a number up to 100, and then squared the sum before dividing by 2048.
Reachable values ran past 12000cp in ordinary middlegame positions, so the score
clamped and every position read as ±9999. Symptom: the search reported
`score cp 9999` interleaved with real scores at the same depth, and it played
1.b3-ish garbage out of the opening.

**Favouring the side to move.** Material and piece-square were differenced
between the two colours, but every *positional* term — mobility, threats, pawn
structure, outposts, shelter, storm, king danger — was added for the side to
move and never subtracted for the opponent. The evaluator was scoring "my
features", not "my advantage". Symptom: the start position evaluated at +538 for
White.

Rewritten as a single `sideScore(colour)` function evaluated for both sides and
differenced, so no term can structurally favour the mover. That structural change
mattered more than any weight: it made the impossibility of the bug obvious, in
the same way that making castling generate all squares made the missing
`Kxf7` obvious.

### A tool worth more than the fix

`tools/evalsym.py` measures how far the two colours disagree on a symmetric
position and then zeroes one weight group at a time — including all 832
piece-square weights as one group — to attribute the discrepancy to a term. Each
hypothesis costs a subprocess round trip rather than a rebuild. This is the
evaluation analogue of `tools/narrow.py`, and it follows the same principle: never
guess which of a hundred terms is wrong, make the engine tell you.

### What it found, and then what it could not find

The first run attributed the asymmetry to the piece-square tables, and the cause
was structural rather than arithmetic:

```cpp
int mg_psqt[PIECE_NB][SQUARE_NB] = {   // PIECE_NB == 16
    {0},                               // index 0  NO_PIECE
    { pawn }, { knight }, { bishop },
    { rook },  { queen },  { king },   // indices 1..6  = W_PAWN..W_KING
    {0}, {0},                          // indices 7, 8
    // ...and that was the end of the initialiser list
};
```

Only eight rows were supplied. Indices 9–14 — **every Black piece** — were
zero-initialised. White was scored with its piece-square table and Black with none,
which is worth ~800cp against the mover on the starting position.

Nothing about this fails to compile, warns, or looks unusual at the use site: the
index is a `Piece`, the array is large enough, and the read is in range. The
array is simply half-empty. Fixed structurally rather than by pasting in eight
more rows — the tables are now indexed by `PieceType` and mirrored with `s ^ 56`,
so there is only one copy and it cannot be half-written:

```cpp
int mg_psqt[PIECE_TYPE_NB][SQUARE_NB] = { {0}, {pawn}, {knight}, ... };
...
r.mg += W.mg_value[pc] + W.mg_psqt[pt][v];   // v = (c == WHITE) ? s : s ^ 56
```

**Then the tool ran out of road.** With the tables fixed it reported a 15cp
residual and zero contribution from every term — which looked like success, but the
metric was wrong. It compares "side to move" against "other side" and sums them,
so it cancels *any* offset that is constant per colour, and also any
mover-dependent term. A bias of either kind is fatal and this metric is blind to
it by construction. The actual readings were still −1646 for White and +1661 for
Black on the same board.

The fix was a better instrument, not another guess: `aurora evaldump <fen>` prints
the two per-side accumulators (`mg`, `eg`, `phase`) *and* a per-section breakdown
of each colour's contribution:

```
  side-to-move : mg    4563  eg    4918  phase 12
  opponent     : mg    6095  eg    6694  phase 12
  difference   : mg   -1532  eg   -1776

  section               us mg     opp mg    diff mg
  material+psqt          4478       4478          0
  mobility               -137       -137          0
  threats                   0          0          0
  pawns                    48       1580       -1532   <-- here
  pieces                   30         30          0
  king                    144        144          0
  spark                     0          0          0
```

`pawns`, 1532cp, isolated to one section in one sentence.

### The passed-pawn test: two frame errors in three lines

```cpp
const Rank r = relative_rank(c, s);      // RELATIVE rank
...
for (Rank rr = RANK_1; rr <= RANK_8; ++rr) {
    const bool isAhead = (c == WHITE) ? (int(rr) > int(r))   // compared against ABSOLUTE rr
                                     : (int(rr) < int(r));
    ...
}
if (theirPawns & file_bb(ff) & ahead) passed = false;   // never fires for Black
if (passed) add(W.mg_passed_pawn_bonus[s], ...);        // absolute index for Black
```

For White `relative_rank` is the identity, so the code works by accident. For
Black a pawn on absolute rank 7 has relative rank 2, so `rr < 2` tested only
absolute rank 1, found no enemy pawn there, and declared **all eight starting
pawns passed**. The bonus was then also indexed by the raw square, so Black read
the wrong table row. Together: 1532cp of imaginary passed-pawn bonus, awarded
solely for being Black.

The mirrored value is now used for both the comparison and the table index, so
neither can disagree about which way the pawn is pointing.

The same dump exposed a third bug for free: **`phase 12`**. The phase increments
summed to 12 for a full board while the taper divided by 24, so the middlegame
tables were applied at half strength in the opening — the engine was effectively
playing with the endgame weights while the queens were on. Increments are now
scaled so a full board is exactly `PHASE_MAX`, and a fourth: the **tempo bonus was
added to the middlegame accumulator before the taper**, so the taper multiplied it
by `phase` and it vanished entirely in endgames. The engine stopped valuing the
right to move exactly when the position was decided. It is now applied after the
taper.

### Two more, both from the same stockfish habit

`attackedBy[c][pt]` was seeded with the king's own attack reach for every piece
type — a Stockfish convention, adopted here without its surrounding reasoning. It
produced a 300cp asymmetry on a symmetric board:

```
  king.shelter+storm        126        126          0
  king.danger              -300          0       -300   <-- here
```

With the d-file open and the queens facing down it, White's king on e1 "defends"
**two** squares of the d-file in front of it (d1, d2), so Black's queen counted as
a double-attacker there and took the full danger penalty. Black's king on e8
defends only d7 — the other, d8, being its own queen's square, which a piece never
attacks. Two versus one, and the side whose king sat behind a queen paid.

The fix is to build `attackedBy2` — the "attacked by both sides" set the danger
term actually uses — from `realAttacks`, which has no king seeding. The pawn-defended
outpost test was reading the same set and had the same flaw: an outpost "defended"
by your own king is not a pawn outpost.

The lesson generalises past this bug. Two of the last four eval defects were
inherited from Stockfish's *shape* while dropping the part of the design that made
the shape correct. A convention copied without its invariant does not arrive
neutral — it arrives wrong, and plausibly.

### The king-danger term, twice

After the real-attack fix, `king.danger` still read −158 for White and 0 for
Black on a board that differs from its own mirror in nothing. Adding the attacker
breakdown by piece type showed why I could not derive it:

```
  king-attack units  us: N 0 B 0 R 56 Q 70  (total 126)
                     opp: N 0 B 0 R 0 Q 0  (total 0)
```

Zero for Black, where mirroring demands 126. I spent a long time deriving why,
and the derivation said the two must be equal — which meant the derivation, not
the code, was where I was wrong. So the term was replaced rather than debugged.

The old formulation counted *squares* in the king zone that were attacked by the
enemy **and** by us — an "attacked by both sides" set. It is cheaper, and it is not
obviously symmetric: what it measures depends on which side's pieces happen to
cover the zone, not only on who is attacking it. That is a design whose symmetry
has to be re-established every time it is touched.

The replacement is the classical one and its symmetry is structural:

> Count enemy **pieces** whose attack set reaches our king zone, weighted by type.

It depends only on our king's square and the enemy's attack sets, so mirroring the
position necessarily mirrors the count — there is no configuration that breaks it.
It costs a handful of attack lookups per enemy piece, which is what Stockfish's
own pre-NNUE evaluation did.

I would rather have the term that cannot be wrong than the term that is cheaper,
especially since the engine had already produced three separate king-safety
asymmetries in a row from the same source: weights too large, the king's reach
seeded into every piece's attack set, and this.

---

## The search: it was not searching

This is the finding that matters most in the whole project, and it was hiding in
plain sight the entire time.

### The symptom that was not a symptom

From the first successful depth-8 search onward, Aurora played **`b1a3` from the
start position at every single depth**, with a principal variation and a score that
changed with depth. It reported 5 million nodes. It looked like a search with a
strange opening preference.

The evaluation does not prefer Na3. Measured over all 20 legal first moves:

| move | static eval, White's POV |
|---|---|
| **g1f3** | **+86** |
| b1c3 | +80 |
| g1h3 | +42 |
| b1a3 | +38 |
| b2b4 | −143 |
| f2f4 | −150 |

Nf3 first, Na3 fourth. So either the search was overriding a clear evaluation, or
the evaluation was not being consulted.

### The cause

```cpp
const int pvCount = std::min(multiPV_, rootMoveCount_);
...
for (int i = 0; i < pvCount; ++i) { /* search root move i */ }
```

`multiPV_` is 1. So the root searched **one move** — the first in generation order —
and returned it. `multiPV` is the number of lines to *report*, not the number of
moves to *search*; the two were conflated.

`b1a3` is the first move Aurora generates from the start position. **The engine was
returning the first move in generation order.** The node count, the depth-by-depth
score drift and the principal variation were all real — they described a deep
search of that one move. Nothing was ever compared.

Corroborating evidence, one of which I initially misread:

- `info depth 1 … nodes 1`. **This does not mean one move was searched.** The
  `info` line is printed from inside the root loop after the *first* move, so the
  node count is a progress report, not a completion report. I took it as proof of
  a single-move search and it was not; the print happens mid-loop. Recorded because
  I would otherwise have cited a wrong number here.
- The real confirmation is behavioural. Before the fix, in the hanging-queen
  position the engine played `d1a1` and reported **−613**. After it, the same
  position yields **`d1d5` and +1036** — the undefended queen taken, which is worth
  exactly the queen. The opening move also changed away from the first generated
  move.
- The principal variation's endpoint evaluated to **exactly** the score reported
  (both +107, to the centipawn). The PV was real; it was simply one line, and the
  "best move" was whatever happened to be generated first.

### Why it survived so long

Because every earlier signal was *consistent with a working search*. Node counts
grew with depth. Scores moved. A PV was printed. The evaluation was audited to
symmetry and found correct. Nothing in the move generator was wrong, so perft
stayed green throughout — and a perfectly correct move generator makes a
non-searching engine look like a searching one. The evaluation audit was
particularly reassuring in the wrong way: I had established that the evaluation
ranked Nf3 above Na3, which made the engine's choice look like a search that was
*overriding* a sensible evaluation, rather than one that had never consulted it.

What finally caught it was writing a test that asked a question perft never asks:
*can it find something that is there?* `tools/tactics.py` failed two of its first
three cases — no mate in one, and no capture of a completely undefended queen.
Those are not the symptoms of a weak evaluation. They are the symptoms of not
searching.

The lesson generalises past this bug, and it is the most useful thing learned on
this project: **a test suite whose questions are all variations of one question
will pass an engine that does not search.** Every check built beforehand — perft,
make/unmake round-trips, evaluation symmetry, score convergence, node counts, PV
validity — was about *internal consistency*. All were satisfied by an engine that
picked a move before looking at any. Only "find the mate" and "take the free queen"
ask about the relationship between the engine and the outside world.

That is why `tools/tactics.py` is now part of the suite rather than an
afterthought, and why the ladder in the README runs perft → evaluation → tactics
→ matches: each stage asks something the previous one is structurally unable to
ask.

A second, smaller lesson: the fix did not appear to work at first, because the
link failed (`aurora.exe` was locked by a search still running) and the test
therefore ran against the *old* binary. Rebuilding to a new filename settled it in
one step. "The build printed no errors" and "the binary I tested is the one I just
built" are different claims, and only the second one matters.

### What fixing it exposed: the root never pruned

With every root move finally searched, depth 8 cost **18 million nodes and 16
seconds** — far worse than usable, and worse than the one-move search had looked,
because that had only ever searched one line.

The cause was in the same function:

```cpp
Value score = -search(pos, stack + 1, -beta, -alpha, depth - 1, false, (i == 0));
...
if (score > bestThis) { bestThis = score; bestIdx = i; }   // alpha never updated
```

`alpha` was never advanced inside the root loop. With the window fixed at
`(-inf, +inf)`, every one of the twenty root moves was searched over the entire
range and nothing could be pruned. Replaced with principal variation search at the
root — full window for the first move, a null window for the rest, a re-search for
any move that beats alpha — plus `alpha = max(alpha, score)` as the loop proceeds.

Worth noting how these two bugs hid each other. The missing `alpha` update was
invisible while only one move was being searched, because with a single move there
is nothing to prune against. The one-move bug was invisible as a *slowness* problem
because searching one line to depth 8 is fast. Only after fixing the first did the
second become visible — and it took the naive reading, "it searched one move, so it
must have been quick", to hide in the first place.

A third fault surfaced with it: the reported principal variation was a single move.
The PV is spliced from the child node, and the child was returning early from a
transposition-table cut, so `pvLen` was zero. The `info` line was printing
whichever move was searched *first* while `bestmove` returned the best — so the log
showed a PV belonging to a different move than the one played. Reporting now happens
after the root list is fully searched, and the test for it is the cheapest possible
one: the first move of the reported PV must equal `bestmove`.

---

## Stale memory on the lagged-evaluation path

With the root fixed, the hanging-queen position behaved like this when searched
depth by depth:

    depth 1..6   best=d1d5   score ~ +1036    correct
    depth 7      best=d1d5   score =    +24    correct move, absurd score
    depth 8      best=d1b1   score =     +7    the queen is never taken

A *won* position scoring worse the harder you look is the signature of a search
finding a refutation it should not. I reached for the usual suspects - repetition
detection (there is none in this engine), the fifty-move rule (unreachable at ply
8), null-move pruning, futility - and none of them accounted for a score that
collapses at one specific depth while the *move* stays correct and the *number*
keeps sliding.

The cause was one line, sitting directly under a comment that said the opposite:

    } else {
        // Inherit the parent's value, adjusted for the material swing
        eval = Value(ss->staticEval);          // this node's OWN slot
    }

`ss->staticEval` is *this* node's slot, not the parent's. It is never initialised
for the current position, and the `Stack` array lives for the whole `think()`, so
every odd ply at depth 5 or beyond that missed the transposition table was scored
with whatever value some *unrelated* position left there earlier in the same
search. The fix is one dereference:

    ss->staticEval = (ss - 1)->staticEval;
    eval = Value(ss->staticEval);

Why it was invisible for so long, and why fixing the root made it *worse*:

- The lagged path only triggers at odd plies >= 5 **without** a transposition hit.
  The broken root searched a single move, so nearly every node was a TT hit and the
  bad branch was almost never taken.
- Fixing the root made real searches reach deep odd plies, so the defect went from
  latent to dominant: an unrelated change exposing a bug that had been there the
  whole time.

The general lesson is about comments and code disagreeing. The comment said
"inherit the parent's value"; taken as documentation, the bug would never have been
written. Where a comment states an intent that is *not* an obvious restatement of
the code, that is exactly where to look hardest, because it usually marks a place
where the code was wrong once and the comment was left behind as the fossil. Two of
the worst defects in this project - this one, and `attacked_by` reading the live
position instead of the passed occupancy - both sat directly beneath a comment
describing the correct behaviour.

---

## Turning off the speculative pruning

With the stale-evaluation bug fixed, the move was right at every depth but the
*score* still collapsed with depth, and node counts reached 102 million:

    depth 1..6   best=d1d5   score ~ +1036   nodes <  2k
    depth 7      best=d1d5   score =    +24   nodes    3k
    depth 8      best=d1d5   score =    -11   nodes 102M

That is a search spending a hundred million nodes to arrive at a number that is
wrong about a position it has already proved won. The culprits are the standard
speculative layer - null move, reverse futility, razoring, IIR, singular
extensions - and rather than keep reasoning about which one misbehaves, all five
are now behind a single constant:

    constexpr bool SPECULATIVE_PRUNING = false;

The baseline is plain alpha-beta with a transposition table, quiescence search and
history ordering. That is verifiable, and every one of those features is standard,
fast, and previously unmeasured here.

The reasoning, because it is the part worth keeping: **a wrong score is worse than
a slow one.** The score is not just what gets printed - it drives move ordering, the
transposition table bounds, aspiration windows and every comparison above it. An
engine that reaches the right move by way of a score that is nonsense is one bug
away from reaching the wrong move, and there is no way to notice from the outside,
because the PV still looks reasonable.

So the order is: get a correct, slow engine; measure it against Stockfish; then
re-enable each speculative feature one at a time, keeping it only if games improve.
That is the same discipline as the NOISE and SPARK ideas, and it is the reason
those ideas are currently at their defaults rather than dialled up: a feature
whose benefit has not been measured does not get to be on.

---

## Judging a score: ask Stockfish

With the pruning off, scores stopped collapsing, but one class of absurd number
remained. In a position with a queen on each side and nothing else, Aurora reported
**+1027** for the side to move while its own static evaluation of the same position
said **+9**.

That is either a search bug or a genuine tactic, and there is no way to tell from
inside the engine: a search that finds a real queen win looks exactly like a search
that hallucinates one. Both print a plausible number and a plausible variation.

The resolution is to ask the reference. Comparing Aurora's depth-8 score and move
against Stockfish 19's on the same position separates the two cases immediately, and
it is the same principle as `narrow.py` applied to evaluation rather than to move
generation: when the engine claims something surprising, the cheapest way to find out
whether it is right is to have something known-good look at the same position.

This is also the instrument that settles "is this score a bug or a discovery?"
honestly. Left to its own devices the engine would have been believed in one case
and disbelieved in the other on the same evidence - a score that is large is not by
itself evidence of anything.
### Two standard optimisations, both removed

The comparison against Stockfish settled it: Aurora was not finding a tactic, it
was producing numbers that had nothing to do with the position. Two of the most
standard speed techniques in any alpha-beta engine had to go.

**1. Aspiration windows** - removed earlier, for the same reason.

**2. Lagged evaluation** - evaluate the static score every other ply and inherit
the parent's value in between. This is the standard speed compromise, and it is
what the engine had. Removing it took two separate investigations to justify:

- It first read `ss->staticEval`, *this* node's own uninitialised slot, instead of
  the parent's. Odd plies at depth 5+ were scored with stale values left by earlier
  positions in the same search.
- Fixed, it was still wrong, because the inherited value is not a static
  evaluation: on a transposition hit `ss->staticEval` is overwritten with the
  table's stored **search** score. So a search score was fed back in as though it
  were a static evaluation and then inherited again on the next odd ply,
  compounding with every ply.

The symptom was unmistakable once I looked for it - a score that decays with depth
in a position whose material never changes. White a full queen up:

    depth 1   +1050
    depth 6   +1003
    depth 7    +778
    depth 8     -49

and the mirror image in a position where material is equal and the engine claimed
+1043. Neither is a horizon effect. Both are the same corruption, measured.

Evaluation now happens at every node. It is slower and it is always right, and
"always right" is the property that lets everything above it - pruning, ordering,
the transposition table - be trusted at all.

The pattern across both removals is the part worth keeping: **in this engine,
every unmeasured optimisation has cost more in wrong answers than it has returned
in speed.** None of aspiration, lagged evaluation, null move or IIR had ever been
compared against anything. Each was standard, each was plausible, and each was
wrong here in ways that internal consistency checks cannot see. A fourth and fifth
remain behind `SPECULATIVE_PRUNING` and will stay there until a match says
otherwise.


---

## The bug underneath all of them: quiescence returned its window

Even with lagged evaluation removed and every speculative pruning disabled, scores
still drifted with depth. So the corruption was never in the prunings. It was here,
in the last line of `qsearch`:

    ss->staticEval = int(Eval::evaluate(pos));
    if (ss->staticEval >= beta) return Value(ss->staticEval);
    if (ss->staticEval > alpha) alpha = Value(ss->staticEval);
    ... search captures, raising alpha ...
    return Value(alpha);          // <-- wrong

Consider a quiet position - no captures available - whose static score is *below*
the alpha its parent passed down. That is the ordinary case inside a narrow
principal-variation window. No capture raises alpha, so the function returns **the
alpha it was given**.

A window bound was therefore returned as though it were an evaluation. The parent
used it as a score, derived a window from it for its own children, and the error
compounded by one ply per level. Nothing downstream ever received a number that was
not either a real evaluation or an ancestor's alpha.

The fix is to track the actual value and return that:

    Value best = Value(ss->staticEval);      // the stand pat
    ...
    if (v > best) { best = v; if (v > alpha) { alpha = v; ... } }
    return best;

### Why it looked like three different bugs

The signature was unmistakable once measured, and I read it correctly only after
wasting time: scores that *decay* with depth in positions whose material never
changes, by about the value of a queen.

    White a full queen up            material equal, queen each
    depth 1   +1050                  depth 1     +47
    depth 6   +1029                  depth 6      +1
    depth 7    +778                  depth 7   +1041
    depth 8     +14                  depth 8   +1044
    depth 9     -14                  depth 9     +48

A swing of roughly 1000cp is exactly one queen, and the alpha values being handed
back as scores are roughly queen-sized in these positions. That coincidence is
what finally identified it. A defect producing *arbitrary* wrong numbers would have
been far harder to trace than one whose error always matched the material in play.

### The lesson, and it is not a flattering one

Three separate investigations - aspiration thrash, lagged evaluation, stale static
scores - were all symptoms of this single line. Each was plausible enough to
justify a fix. Each fix removed a symptom and left the cause.

What I should have done after the second failed investigation was stop removing
things and start *bisecting the search itself* - disable quiescence and search to
the horizon, which isolates it immediately. Instead I kept removing optimisations
one at a time, each time finding a genuine defect, each time briefly believing the
newest one was the cause.

**Removing an optimisation hides the bug underneath it.** I did that three times:
removed aspiration, revealed the missing alpha update; removed lagged evaluation,
revealed the depth-decaying scores; removed null move, revealed the node explosion.
The layer I had never touched was the one at fault throughout.

A transposition-table defect was also found along the way and is worth naming
separately because it is the same species: mate scores are shifted by ply on the
*store* side and were never shifted back on retrieval, so every mate score read
from the table was wrong by the distance at which it had been written. It is fixed,
and `TTEntry::value(ply)` now undoes the shift explicitly.
---
And a fourth investigation, which is where the process finally paid off. Isolating
it needed a *minimal* reproduction rather than the original queen end, because the
original was too small to be sure of anything. So I searched the child position on
its own:

    K+Q against a lone king, Black to move, a full queen down.
    static evaluation -961

    depth 1   -1001   correct
    depth 2   -1046   correct
    depth 3   -1016   correct
    depth 4   -1014   correct
    depth 5   +1077   wrong
    depth 6   +1077   wrong

Correct at four, inverted from five, in a position with no tactic and no capture.
A threshold that sharp is not a heuristic misfiring - it is a branch. And there is
exactly one depth threshold in this search between 4 and 5:

    constexpr int PROBCUT_MIN_DEPTH = 5;

The ProbCut probe was being called with

    (-pcBeta, -alpha - PROBCUT_BASE)      //  -alpha-1,  -alpha-210

With `PROBCUT_BASE = 210` that is an **inverted window**: beta sits 209 centipawns
*below* alpha. Alpha-beta is defined only for alpha < beta. Handed an inverted
window, the search's cutoffs fire in the wrong direction, the value it returns is
not a bound in the intended sense, and that value was then stored as the move's
score - which is why a position that cannot be lost came out won by a queen.

The fix is to give ProbCut the null window it was always supposed to have:

    (-pcBeta, -pcBeta + 1)

ProbCut, late move reductions and futility pruning are now all behind
`SPECULATIVE_PRUNING`, alongside null move, reverse futility, razoring, IIR and
singular extensions. The search that remains is alpha-beta with a transposition
table, quiescence, killers and history - the part whose correctness I can actually
argue for.

### The four bugs were one bug's four symptoms

Aspiration thrash, lagged evaluation, the depth-decaying scores, and the
sign-inverted endgame were not four independent defects. Every one of them was
`return Value(alpha)` in quiescence and the inverted ProbCut window propagating
values upward as though they were evaluations.

Which makes the process the real result. I found them one at a time, in the order
the engine happened to misbehave, and each time the newest one looked like the
root cause. The thing that actually cracked it was not insight but a change of
instrument: stop testing the position that revealed the symptom, and test the
*smallest position that still shows it*. From "a position I cannot lose scores as
though I have" to "the threshold is 5, and exactly one constant equals 5" was a
single step once the reproduction was minimal. Every previous step had been
reasoning from a position too rich to reason from.

## The search: aspiration windows were preventing measurement

With the evaluation finally sound, a depth-10 search from the start position
produced this:

```
info depth 9 ... score cp -96  nodes 263624
info depth 9 ... score cp    0  nodes 326602
info depth 9 ... score cp   37  nodes 11194488
info depth 9 ... score cp   91  nodes 29542555
bestmove b1a3
```

One depth. Thirty million nodes — roughly thirty times what it should cost — and a
score that never settled, moving across four values at the same depth.

Two separate faults:

**The aspiration loop was pathological.** The window was centred on the previous
depth's score with a half-width of `ASP_BASE` = 24. On fail-low, the handler
pulled `beta` *towards* the new lower score while lowering `alpha`, so each retry
re-searched every root move again over a window that had not usefully widened.
Each retry replaces truncated bounds with true values, which is exactly why the
reported score kept moving.

**The log was lying about which move it was reporting.** The `info` line printed
the score and PV of whichever root move was searched *first*, and the root order is
re-sorted between depths. The line looked unstable even where the search was
converging.

Both fixed: aspiration narrowing is disabled outright, and `info` now reports the
*best* move's score and PV plus a retry count. The reasoning matters more than the
patch — the aspiration window is not free speed that was missed, it is the thing
standing between me and knowing what the search believes. With a full window there
is exactly one search per depth and every score is a true value, so any future
work on aspiration can be measured against this baseline rather than argued about.

The remaining oddity at this point is that `b1a3` is still the best move at every
depth, which no honest evaluation would choose.

That nearly went into this log twice as an engine bug, and both times the fault
was mine.

**First:** I hand-typed the FEN for the position after 1.Na3, put the knight on f2
instead of a3, and measured Black's evaluation at +206cp — apparently a 220cp
blunder by a harmless knight move. Redone with the engine's own `after` output:

| first move | static eval for White |
|---|---|
| **g1f3** | **+86** |
| b1c3 | +80 |
| g1h3 | +42 |
| b1a3 | +38 |
| … | … |
| f2f4 | −150 |

The evaluation ranks Nf3 *first* and Na3 fourth. It is not asking for Na3.

**Second:** the search reports +107 for Na3 while the static evaluation near the
end of its own principal variation reads +22 — which looks like ~85cp of
horizon-effect inflation and looked like a `qsearch` bug. Evaluating the full
ten-ply line with correct parity gives **+107 for White at the endpoint — exactly
what the search reports**. My script negated on even plies, which are the plies
that are *already* White's point of view. The search and the evaluation agree to
the centipawn.

So there is no search bug. The engine genuinely believes 1.Na3, because its knight
PSQT rates b5 at +53 and f3 at +15, and a knight that swings to b5 is classical
chess orthodoxy. What is missing is not correctness but judgement: a hand-set
evaluation with no term for wasted time, and no tuning at all. That is exactly
what a measured baseline and an A/B loop are for — and the reason to start playing
games rather than continue auditing.

The lesson worth keeping: twice in a row, a "the engine is broken" conclusion came
from a position I assembled by hand rather than one I asked the engine for. Both
times the engine was right and I was wrong. The instrument already existed; I used
my memory of what a FEN should look like instead.

Worth stating plainly as a process point: a green diagnostic is only as good as
the question it asks. I nearly accepted "all terms contribute zero" as proof of
correctness when it was only proof that the terms were symmetric *under a metric
that cannot see the failure*. The per-section dump took five minutes to write and
named the bug outright.

A note on a mistake worth recording: while chasing the second bug I "fixed" pawn
check detection in `set_check_info` to use the opposite colour, and perft got
*worse* (kiwipete depth 4 went from correct to 4091270). I had misread a FEN. The
defender's colour is correct there — the squares an enemy pawn must occupy to
attack `ksq` are exactly the squares one of *our* pawns on `ksq` would attack. The
regression was the useful signal, and it is why the final version is measured
rather than reasoned about.

---

## The crash that blocked playing a single game

Perft green and a sane evaluation are both worthless if the engine cannot accept a
`position` command. It could not: `position startpos` worked, `position fen ...`
died with an access violation. No game could be played, so nothing downstream of
the search was measurable at all.

gdb on a `-O0 -g` build gave the answer in one shot:

```
#0  Bitboards::rook_attacks_bb (s=SQ_NONE, occ=0)   at bitboard.cpp:254
#1  Position::set_check_info                         at position.cpp:191
#2  Position::set (fen="")                           at position.cpp:148
#3  Aurora::UCI::position                            at uci.cpp:56
```

The FEN arriving in `set()` was **empty**. `UCI::position` had no `else` branch,
so a command it did not recognise fell through to `pos_.set("")`, which installs a
board with no pieces — and then:

> `king_square(c)` is `lsb(pieces(c, KING))`. For an empty bitboard `lsb(0)` is
> **64**, which is `SQ_NONE` and exactly one past the board. Every attacker lookup
> on it indexed one past the end of the magic tables.

The root cause was one line above that: `UCI::position` re-skipped the command word.

```cpp
void UCI::position(std::istringstream& is) {
    is >> token;   // "position"   <-- already consumed by the caller
    is >> token;   // this one is the real first argument
```

`loop()` reads the line into a stream and consumes the leading token to dispatch
on it, so by the time `position()` runs the stream is already positioned *after*
`position`. Skipping another word meant the *placement* arrived where `startpos`
or `fen` was expected, both branches were skipped, and the empty FEN fell through.

Adding the missing `else` was still worth doing — it turned a silent wrong state
into a loud error — but the error message is what actually identified the double
skip, and it printed the offending token verbatim:

```
position: expected 'startpos' or 'fen', got 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP'
```

A placement string where a command keyword belonged is an unmistakable
fingerprint; I had been unable to derive it from the code alone. Fixes applied:

- `UCI::position` no longer re-skips the command word.
- `UCI::position` refuses a command that is neither `startpos` nor `fen`, and says
  why. A malformed command is reported, not applied.
- `Position::set` parks a missing king on a free square of its own back rank and
  warns on stderr, so garbage input cannot cause an out-of-bounds read no matter
  which caller supplied it. Silently fabricating a square would hide the problem;
  keeping the position in bounds while making the error loud does not.

Verified afterwards: `position fen`, `position startpos`, `startpos moves ...`,
a FEN with an ep square, a malformed command, and a FEN with no white king — all
behave.

Worth noting what this cost. The engine could not play a game and no strength
measurement of any kind had been attempted, yet the defect was invisible to every
test written so far. `tools/smoke.cpp` and the argv subcommands had all been
exercised against the *same* FENs and all passed, because they call `set()`
directly with a FEN they built themselves. Coverage of a function is not coverage
of its callers — the bug lived entirely in the argument plumbing, which no test
touched.

---

## Debugging log — bugs found and fixed

Ordered as they were found. Each is a real correctness bug, not a tuning issue.

### 1. `++s` on strongly-typed enums never advanced
Only the postfix-shaped `operator++(T) -> T` was defined, so a prefix `++x` bound
`x` to a *copy*, incremented the temporary and discarded it. Every
`for (Square s = A1; s <= H8; ++s)` loop span forever. Fixed by defining the
prefix form `T& operator++(T&)`.

### 2. Magic bitboards: three separate failures
- `ray()` checked only the flat 0..63 index, so diagonals wrapped the board edge
  (h5 + 9 = a7). Masks were wrong and tables were overrun.
- Masks included the outermost square of each ray. A blocker there is
  indistinguishable from no blocker, so excluding edges is what makes a 9-bit
  bishop index sufficient for a central square.
- Candidate generation was wrong: position-bounded "low bit" magics and plain
  dense-random magics both fail. The working recipe is Stockfish's — sparse
  magics (`next() & next() & next()`) filtered by
  `popcount((mask * magic) >> 56) >= 6`. Measured worst case: bishop 18 723
  attempts, rook 99 735, zero failures over all 128 squares. Results are baked
  into `src/magics.h` and re-verified exhaustively at startup.
- A first attempt indexed attack tables by `popcount(occ & file) * 8 +
  popcount(occ & rank)`. That records *how many* blockers are on each line, not
  *which*, so a rook saw straight past a blocker. Discarded.

### 3. `BetweenBB` only described diagonals
Built as `bishop(s1,s2) & rook(s1,s2)`, which is empty for two squares on the
same rank or file. That silently broke rook pins, rook check evasions and the
alignment test behind them. Now the geometry is tested first and only the
matching term is used.

### 4. `LineBB` degenerated for adjacent squares
Built as `between + endpoints`; for touching squares "between" is empty, so a
genuinely collinear triple (f6, f7, g8) tested as not aligned and a pinned piece
could not capture its own pinner. Fixed by building the full shared line.

### 5. `LineBB` then over-included for unaligned pairs
Rebuilt as `(rook(s1,0) & rook(s2,0)) | (bishop(s1,0) & bishop(s2,0))`, which
picks up the corners of the bounding rectangle when s1/s2 are not aligned: for
f6/h7 that includes f7, so `aligned(f6, h7, f7)` came out true and a pinned
knight could step off the file it was guarding. Fixed with the same
geometry-first gating as #3.

### 6. Pawn "captures" onto empty squares
`gen_pawns` intersected the capture ray with the stage's target; in the QUIETS
and NON_EVASIONS stages that target is the empty squares, so `b2a4` was emitted
in the start position (34 moves instead of 20). Captures now intersect with
enemy occupancy and pushes with empty occupancy.

### 7. All captures missing from the main movegen
The NON_EVASIONS branch targeted only empty squares. Target is now
`~pieces(us)`.

### 8. King moves were never generated
The non-check branch generated queen/rook/bishop/knight/pawns but no king moves.
Caught because Kiwipete depth 1 produced 44 instead of 48.

### 9. `attacked_by()` counted our own pieces as attackers
No colour argument: a king move toward a square defended by *our own* knight was
reported illegal. Worth 4 moves in Kiwipete alone.

### 10. `gen_piece<QUEEN>` fell through to the rook case
The `else` branch generated rook attacks, so queens had no diagonal moves at
all. Surfaced as "Aurora is missing d1e2, d1f3, d1g4, d1h5".

### 11. `pawn_attacks_bb` shifted twice
`shift(NORTH, b)` then a diagonal shift moved **two** ranks, so a pawn on e5
"attacked" d7 and f7 instead of d6 and f6. This corrupted check detection, pin
detection and pawn move generation simultaneously.

### 12. Pawn double pushes not restricted to the starting rank
The "intermediate square empty" test was not enough: a pawn on a6 could jump to
a4, and a pawn on a3 to a5. Worth exactly the 32 nodes of inflation that first
sent perft(3) to 8 934.

### 13. Pawn captures never generated in the evasion stage — and pushes neither
A pawn can block a check by *pushing* (king e1, bishop h4, g2-g3). The evasion
generator produced neither, which is precisely what Kiwipete is built to catch.
Both are now generated against the evasion target.

### 14. En passant only generated when evading a check
`add_ep_captures` was reachable only from the in-check branch, so the ordinary
movegen silently dropped every en passant capture.

### 15. En passant square never recorded after a double push
`do_move` set the ep square only if
`pawn_attacks_bb(them, eps) & pieces(us, PAWN)` — asking whether *our* pawns
stood where *their* pawns attack (rank 7/2 rather than 5/4). It essentially
never fired, so the ep square was always lost after a double push. Now recorded
unconditionally, which also makes Aurora's FENs match the reference exactly —
important, because a FEN mismatch makes tree-walking tools compare two different
positions and report phantom bugs.

### 16. En passant capture origin squares
Generating the capture needs the squares the capturing pawn *starts* from, which
is `pawn_attacks_bb(~us, eps)`. The code had a guard asking the wrong question
(it could never pass, so ep was never generated at all) and then briefly had the
wrong colour, which manufactured illegal captures. Both directions were caught
by perft going 48 → 39 → 49 rather than by reasoning alone.

### 17. `set_castling_rights` / castling checks
- `do_move` wrote the new rights into the **parent** state instead of the new
  one, so `undo_move` restored nothing and castling rights leaked away
  permanently across a make/unmake cycle.
- `can_castle_now` tested emptiness of d1/c1 without flipping for Black, so
  Black's castling was judged against White's own back rank.
- The rook's path square was never checked: with a knight on b1, Aurora happily
  castled queenside straight through it.
- The rook is not verified to still be home.

### 18. King moves filtered by the check ray
The evasion generator removed every square on the checker's ray, *including the
checker's square*, so `Kxf7` — the most common single evasion there is — was
never generated. Now every adjacent square is generated and `legal()` decides,
which is correct because `legal()` already lifts the king from its square.

### 19. `undo_move` mistook back-rank pieces for promotions
The un-promotion test was `pt != PAWN && rank(to) is 1 or 8`, which also matches
a king or rook travelling along the back rank. Aurora replaced it with a pawn,
leaving the side kingless, and the next `king_square()` read out of bounds —
which is what turned a logic bug into a hard crash in Kiwipete. The move's own
promotion field is the only reliable test.

### 20. `attacked_by` ignored the post-move occupancy
The slider terms honoured the `occ` argument but the pawn/knight/king terms were
read straight from the live position. Since `occ` is the board *after* the move,
that made two errors: an en passant capture was judged illegal whenever the
captured pawn had been defending the king (the victim was still on the board in
the lookup), and a discovered attack opened by a capture went unnoticed. All
five terms are now filtered through `occ`.

Worth recording as a process note: while chasing this I "fixed" the pawn check
detection in `set_check_info` to use the opposite colour, and perft got
*worse*. I had misread a FEN and thought a black pawn on c5 was checking a king
on a4 when the king was on b4, where that pawn really does give check. The
defender's colour is correct there: the squares an enemy pawn must stand on to
attack `ksq` are exactly the squares one of our own pawns on `ksq` would attack.
Reverted, and the regression was what made the error obvious.


---

## Tooling built during the above

Two of these were worth more than any single engine fix, because they turned
"perft is off by 1 000 000 nodes" into "this specific move is illegal":

- `tools/narrow.py` — bisects a divergence using both engines' perft *divide*
  output. Logarithmic: each level is one query to each engine, so a depth-5
  discrepancy is localised in a handful of calls instead of a full tree walk.
- `tools/divdiff.py` — per-root-move comparison with an optional drill into the
  offending child.
- `tools/difftest.py` — breadth-first walk comparing every legal move list
  against Stockfish, for cases where the divergence is in *move choice* rather
  than node counts.
- `aurora info|moves|after|perft|perftcheck` — the diagnostics live in the
  engine itself, so a helper binary can never drift out of sync with the engine
  it is meant to describe. (It did, twice, and cost real time.)

Deliberately single-threaded search: it keeps every search bit-for-bit
deterministic, which is the precondition for all of the A/B measurement above.
