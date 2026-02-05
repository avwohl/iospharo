# JIT and Performance Optimization Choices

## Current Situation

This is a Pharo Smalltalk VM port to iOS. We have a working interpreter but no JIT compiler. Smalltalk performance is heavily dependent on JIT compilation due to:
- Dynamic dispatch on every message send
- Late binding / polymorphism everywhere
- No static type information to optimize

The test suite runs slowly compared to the standard Pharo VM with Cog JIT.

## Apple's JIT Policy

**JIT is NOT absolutely prohibited on iOS**, but is restricted:

### What Works Without Approval
- Development builds
- TestFlight distribution
- Ad Hoc / Enterprise distribution

Add to entitlements:
```xml
<key>com.apple.security.cs.allow-jit</key>
<true/>
```

### App Store Distribution
- Case-by-case approval by Apple
- No formal application process - submit and see
- Apple approved game emulators in April 2024, showing policy is loosening
- A Smalltalk IDE/educational tool might qualify

### Technical Requirements (if approved)
```c
// Allocate JIT memory
void* code = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);

// Toggle between write and execute modes
pthread_jit_write_protect_np(false);  // Enable writing
// ... generate code ...
pthread_jit_write_protect_np(true);   // Enable execution
```

## JIT Implementation Options

### Option 1: Port Cog (Pharo's existing JIT)

**Pros:**
- Already has ARM64 and x86-64 backends
- Proven, mature code
- Handles all Smalltalk semantics correctly

**Cons:**
- Deeply intertwined with OpenSmalltalk VM architecture
- Major undertaking to port to our clean C++ VM
- Complex codebase

### Option 2: Use a JIT Library

| Library | Pros | Cons |
|---------|------|------|
| **LLVM** | Powerful, great optimization | Heavy (~20MB), slow compile times |
| **Cranelift** | Fast compilation, production quality | Rust dependency |
| **MIR** | Lightweight C library, designed for JITs | Less mature |
| **libjit** | Simple API | Older, less maintained |

### Option 3: Copy-and-Patch JIT

New technique where you pre-compile code templates at build time and stitch them together at runtime. Lower complexity than traditional JIT.

**Pros:**
- Simpler than full JIT
- Fast code generation
- Templates compiled by Clang (good code quality)

**Cons:**
- Newer technique, less proven
- Still need per-architecture templates

### Option 4: Write Minimal Custom Backends

Focus on ARM64 only (Intel Macs are legacy). Implement a simple method JIT for hot methods only.

**Estimate:** ~5-10K lines per architecture

## Non-JIT Optimization Alternatives

These can provide significant speedups without JIT complexity:

### 1. Inline Caching

Cache method lookups at call sites. Huge win for Smalltalk's dynamic dispatch:

```cpp
// Before: full lookup every time (~100+ cycles)
Method* method = lookupMethod(receiver->classOf(), selector);

// After: monomorphic inline cache (~5 cycles for hit)
if (receiver->classOf() == cachedClass) {
    method = cachedMethod;  // Fast path
} else {
    method = lookupMethod(...);  // Slow path, update cache
}
```

**Expected speedup:** 2-4x for message-send-heavy code

### 2. Threaded Code / Computed Goto

Replace switch-based bytecode dispatch with direct threading:

```cpp
// Before: switch dispatch (~15 cycles per bytecode)
while (true) {
    switch (*ip++) {
        case PUSH_LITERAL: ...
        case SEND: ...
    }
}

// After: computed goto (~3 cycles per bytecode)
static void* dispatch[] = { &&PUSH_LITERAL, &&SEND, ... };
#define NEXT goto *dispatch[*ip++]

PUSH_LITERAL:
    // ... handler ...
    NEXT;
SEND:
    // ... handler ...
    NEXT;
```

**Expected speedup:** 1.5-2x for bytecode dispatch

### 3. Superinstructions

Combine common bytecode sequences into single operations:

```
push temp 0; push temp 1; send +    →    addTemps(0, 1)
push self; send instVarAt:          →    pushInstVar(n)
```

**Expected speedup:** 1.2-1.5x for common patterns

### 4. Method Lookup Caching

Global cache of recent (class, selector) → method lookups:

```cpp
struct LookupCache {
    Oop classOop;
    Oop selector;
    Method* method;
} cache[1024];  // Hash table
```

**Expected speedup:** 1.3-2x (complements inline caching)

### 5. AOT Compilation for Tests

Pre-compile the test framework and common base classes at build time, ship native code alongside the image.

## Recommended Approach

### Step 1: Profile First

**Before implementing ANY optimization, we need profiling data.**

Questions to answer:
1. Where is time actually spent?
   - Bytecode dispatch?
   - Method lookup?
   - Primitive calls?
   - GC?
   - Something else entirely?

2. What are the hot methods?
   - Test framework overhead?
   - Collection operations?
   - Arithmetic?

3. What's the baseline comparison?
   - How fast is standard Pharo VM on same tests?
   - What's our overhead factor? (2x? 10x? 100x?)

### Profiling Methods

#### A. Instruments (macOS)
```bash
# CPU profiling
xcrun xctrace record --template "Time Profiler" --launch ./build/test_load_image /tmp/Pharo.image

# Then open in Instruments.app
```

#### B. Manual Instrumentation
Add counters/timers to interpreter:
```cpp
uint64_t bytecodeDispatchTime = 0;
uint64_t methodLookupTime = 0;
uint64_t primitiveTime = 0;
// ... measure each category ...
```

#### C. Bytecode Frequency Analysis
Count which bytecodes execute most often:
```cpp
uint64_t bytecodeHistogram[256] = {0};
// In dispatch loop:
bytecodeHistogram[bytecode]++;
```

#### D. Method Hotspot Analysis
Track which methods consume the most time:
```cpp
std::unordered_map<Oop, uint64_t> methodExecutionCounts;
// On each method activation:
methodExecutionCounts[methodOop]++;
```

### Step 2: Implement Based on Data

Once profiling identifies the bottleneck:

| If bottleneck is... | Then implement... |
|---------------------|-------------------|
| Method lookup | Inline caching + lookup cache |
| Bytecode dispatch | Threaded code |
| Specific hot methods | Consider JIT for those methods only |
| Primitives | Optimize specific primitives |
| GC | Tune GC parameters |
| Test framework overhead | Consider AOT for test code |

### Step 3: Iterate

After each optimization:
1. Re-profile
2. Measure actual speedup
3. Identify next bottleneck
4. Repeat

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2025-02-05 | Profile before optimizing | Need data to make informed choices |
| | Focus on ARM64 only if JIT needed | Intel Macs are legacy |
| | Consider inline caching first | High ROI, no JIT complexity |

## References

- [Cog VM Source](https://github.com/OpenSmalltalk/opensmalltalk-vm)
- [Copy-and-Patch Compilation](https://arxiv.org/abs/2011.13127)
- [Inline Caching in Smalltalk-80](https://dl.acm.org/doi/10.1145/800017.800542)
- [Apple JIT Entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com_apple_security_cs_allow-jit)
