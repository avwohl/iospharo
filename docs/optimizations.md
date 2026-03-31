# Performance Optimizations

Roadmap for interpreter performance improvements. JIT is not possible on iOS
due to Apple restrictions, so all gains must come from interpreter optimization.

## 1. Flat switch dispatch (DONE)
- Replaced if-else chain in `dispatchBytecode()` with a flat `switch(bytecode)`
- Removed V3PlusClosures bytecode paths (only Sista V1 for Pharo 10+)
- Compiler emits a direct jump table — O(1) dispatch instead of O(n) comparisons
- Expected gain: 15-30% on tight loops

## 2. Slim down step() hot path
- `step()` does ~15 conditionals, atomic loads, and a syscall per bytecode
- Should inline the hot path: fetch-dispatch-check_counter loop
- Move GC, timer, signals, yield, stuck-process checks behind a single countdown
- One decrement + branch-predicted comparison per bytecode instead of current overhead
- Expected gain: significant (combined with #1, possibly 2-3x on bytecode-heavy benchmarks)

## 3. Remove diagnostic overhead from sendSelector()
- Selector byte extraction and receiver class name logging happen on every send
- Should move behind the `(++sendCount_ & 0x3FF) == 0` guard
- Corruption check (`rawBits() == 0`) could be debug-only

## 4. Multi-probe method cache
- Current: 2048 entries, single XOR hash probe
- Cog VM uses 4-way set-associative (4096 entries, 4 probes)
- Probe 2-3 secondary positions before full lookup to reduce conflict misses

## 5. Reduce syscalls in periodic checks
- `chrono::steady_clock::now()` (kernel syscall) called every 1024 bytecodes
  for stuck-process tracking
- Use bytecode count as cheap monotonic counter, only call clock every ~64K steps

## 6. Inline caching (PICs) — longer term
- Monomorphic inline cache at each send bytecode site
- Store (classIndex, compiledMethod) pair in side table indexed by PC
- Major step up from global method cache
- High impact but high effort
