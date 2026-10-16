# Benchmarking

This text was in README.md until 2026-10-17.

## Benchmarking

### Quick benchmark (built-in)

The simplest performance check — already in every Pharo image:

```bash
# On the reference Cog VM:
pharo /tmp/Pharo.image eval "1 tinyBenchmarks"
# => "2718114840 bytecodes/sec; 247952910 sends/sec"

# On our VM (results written to Transcript):
./build/test_load_image /tmp/Pharo.image
```

### Benchmark suite

A self-contained benchmark runner that measures 10 workloads and writes
results to `/tmp/pharo_benchmarks.txt`:

```bash
# Inject benchmarks into a fresh image, run on both VMs, compare:
scripts/run_benchmarks.sh

# Or run on just our VM:
scripts/run_benchmarks.sh --ours-only
```

Benchmarks included:

    Benchmark       What it measures
    tinyBenchmarks  Built-in sieve + fibonacci (bytecodes/sec, sends/sec)
    fibonacci(34)   Recursive fib — pure message send overhead
    sieve(100)      Sieve of Eratosthenes — array access + loop cost
    sort(100K)      Sort shuffled array — comparison + collection ops
    dict(100K)      Dictionary put + get — hashing + lookup
    sum(1M)         Sum integers via do: — block/closure + arithmetic
    factorial(10K)  10000! — large integer arithmetic
    block(1M)       1M block evaluations — closure overhead
    instVar(1M)     1M getter/setter — accessor cost
    create(100K)    100K object allocations — GC pressure

Results go to `/tmp/pharo_benchmarks.txt` (one file per VM). The shell
script prints a side-by-side comparison table when run on both VMs.

### External benchmark suites

For more rigorous benchmarking, these community suites work with any
Pharo VM:

- **[Are We Fast Yet](https://github.com/smarr/are-we-fast-yet)** —
  14 cross-language benchmarks (Richards, DeltaBlue, Mandelbrot, NBody,
  etc.) with Pharo implementations. The standard for cross-VM comparison.
  See Marr, Daloze, and Mossenboeck, "Cross-Language Compiler
  Benchmarking — Are We Fast Yet?", DLS 2016.
- **[SMark](https://github.com/smarr/SMark)** (MIT, Stefan Marr) —
  test-like framework for repeatable benchmarks with warmup, iteration
  control, and statistical reporting.
- **[Computer Language Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/measurements/pharo.html)** —
  Pharo entries for binary-trees, mandelbrot, n-body, spectral-norm, etc.
