# Session H — closed 2026-05-25

Closeout record for the JIT BV-inline default-flip session.
Open deferred items now tracked in `docs/deferred.md` §A6
iter N+31.  Deeper analysis + landmines in
`memory/project_blocks_never_run_in_t1.md`.

## What landed

Eight commits (`4ebd4718..d48e02d3`) addressing the
"blocks never run in T1" gap:

```
commit     effect
4ebd4718   docs: Eβ verified-inactive — root cause was block JIT compile failures
2293e035   PHARO_T1_NLR_TAIL_ONLY flag (default-OFF originally)
8395fe5e   extend the flag to also guard 0xF9/0xFA (nested-closure-creating blocks)
fd149f82   closure side-stack: BV prep sets closure_=block, paired pop on J2J return
5fbb2e68   move side-stack into Interpreter so forEachRoot walks it (GC safety)
b059fb8f   also pop the BV closure side-stack from JITRuntime's fast-path return
8ee0cbc7   cleanup guard at tryJITActivation exit drains leaked saves
d48e02d3   *** DEFAULT FLAG FLIP — the actual fix ***
```

Sista IC-promotion work earlier in the session
(`562548ee..3a09e2f4`): hint shifting + SpecialSend TICR +
BlockReturn-aware blacklist.  See `docs/sista-ic-promotion-plan.md`.

## The bigger refactor — what it turned out to be

Goal: stop BV-inlined blocks from corrupting state when they
bail mid-bytecode to interp.  Symptom: intermittent
`ZnByteEncoder class >> #to:do:` DNU at `ipOff=23680` (IP way
past method bytecodes) with `bvDepth=71` on the side-stack —
direct evidence of J2JSave/SavedFrame stack divergence.

After four phases of code-level refactor attempts — all
documented in the memory file — the actual architectural fix
was a **flag-default flip** in `commit d48e02d3`:

- `t1InlineBlockValueNonLeaf`: ON → OFF (leaf-only BV inline)
- `t1NlrTailOnly`: OFF → ON (tail BlockReturnTop is safe)

**Validated**: 20/20 bench-suite pass at default config.

**Catch rate**: 5.9% (62 hits) vs old default 11.8% (118 hits) —
but **bench numbers are identical**.  The 56 lost non-leaf
inlines were unsafe AND not measurably useful — pure risk
removal.

## Phases that didn't work (and why)

Documented for future sessions so the landmines are marked:

1. **Full `activateBlock` call** from BV prep
   — broke 10/10 under `NLR_TAIL_ONLY` because `pushFrame`
   moves `framePointer_`/`stackPointer_` in ways T1's
   `state.sp/tempBase` derivations can't compensate for.

2. **State-only frame push** (save method_/closure_/etc.,
   skip fp/sp updates)
   — broke 10/10 because interp's `framePointer_`-relative
   `PushTemp` reads from the wrong slots when interp resumes.

3. **Restoring `_sv->ip`** in `J2J_INLINE_RETURN` (vs
   bytecode-start)
   — no effect; GC walks weren't the leak source.

4. **`wasBVInline` flag on SavedFrame** + `popFrame`
   coordination
   — broke 9/10 because T1's send/return stencils internally
   compute `state.sp` from caller-relative offsets that
   don't account for a savedFrame push, even when interp
   tracks it correctly.

**Common thread**: T1 stencils are hardcoded around the
"caller's pre-push stack layout" assumption.  Any
mid-execution savedFrame push disturbs that.  Closing this
without modifying every send/return stencil isn't possible.

## Open work

### Future option (a): stencil-emit refactor (multi-week)

Modify T1 send/return stencils to be SavedFrame-aware.
Each send stencil would need to compute `state.sp` from a
SavedFrame-relative offset rather than caller-relative.
Unlocks the lost 56 non-leaf BV inlines.

**Expected upside is small**: those inlines didn't help
measurably even when running unsafely (bench numbers
identical between old and new defaults).  The BV inline
mechanism's per-call overhead (helper call + state setup)
already swamps savings for non-leaf blocks.

### Future option (b): lift BV inlining to Sista IR

Where stack coordination isn't needed.  Sista's
`tryInlineConstReturn` already does this for constant-return
sends via `kGuardClass + kLoadInstVar` emit.  Extending it
to handle leaf-block bodies (no sends, just push/load +
BlockReturnTop) would inline the block's body directly into
the caller's Sista IR with no runtime stack manipulation at
all.  Closes the same gap without T1 stencil changes.

### Sista IC-promotion residuals

`docs/sista-ic-promotion-plan.md` — Sista-side hint plumbing
work.  Catch rate counters look healthy after Session E
fixes; bench gap is in Sista lowering (kGuardClass hoist,
back-edge type narrowing).

## How to verify the current state

```bash
PHARO_JIT_DEFER=15 timeout 60 ./build/test_load_image \
    /tmp/bench_suite-ours.image 2>/dev/null >/dev/null
cat /tmp/bench_suite_result.txt
```

Expected output (within bimodal noise):

```
fib(28) = 117 ms
sort 100K = 167-281 ms (bimodal)
dict 50K = 147-265 ms (anti-correlated with sort)
sum 1M = 52 ms
1M getter+yourself = 33 ms
collect 10x100K = 65 ms
select 10x100K = 370 ms
```

To opt back into the old unsafe behavior (e.g., to reproduce
the corruption or benchmark the lost inlines):
`PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF=1 PHARO_T1_NO_NLR_TAIL_ONLY=1`.

## References

- `docs/deferred.md` §A6 iter N+31 — open deferred items
  (stencil-emit refactor + Sista IR splice options).
- `memory/project_blocks_never_run_in_t1.md` — phase-by-phase
  failure landmines + signatures for future sessions.
- `memory/project_sista_ic_promotion_bench_gap.md` — Sista-side
  IC promotion work (separate but related).
- Commits `4ebd4718..d48e02d3` for the BV inline arc;
  `562548ee..3a09e2f4` for the Sista IC-promotion sub-thread.

## Session-complete checklist

- [x] Default flag flip shipped (`d48e02d3`).
- [x] 20/20 bench-suite passing at default config.
- [x] Open items moved to `docs/deferred.md` §A6 iter N+31.
- [x] Landmines captured in memory file.
- [x] WIP doc closed out — no further edits expected for
      Session H.
