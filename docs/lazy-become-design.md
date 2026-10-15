# Lazy two-way become: design

Status: implemented 2026-10-15 (cb5cb04a two-way, 0d328f09 one-way).
Knob: `PHARO_LAZY_BECOME` (opt-in until a sweep validates it).

## Problem

ReStore swaps each proxy with its object: `proxiedObject become: self`,
one pair per call, about 2000 calls per test.  The two objects have
different sizes, so primitive 128 must exchange every reference to them.
Today each call scans the whole heap (a parallel old-space scan, a serial
eden walk, and a root scan that includes every JIT cache): about 4 ms per
call.  ReStore's test classes take 100 times as long as on Cog, and its
SSWReStoreSpeedTest assertions fail.  Cog does each swap in time
proportional to the stack: it leaves forwarders and follows them lazily.

## Design (Spur's scheme)

For a pair (A, B) of different sizes:

1. Make two copies in old space: B' holds B's contents and A's identity
   hash; A' holds A's contents and B's identity hash.  (References to A
   must reach B's contents and keep A's hash, as the current scan does.)
2. Turn A into a forwarder to B', and B into a forwarder to A':
   class index `ForwardedClassIndex`, slot 0 the target.  Old-space copies
   mean no old-to-young pointer in a forwarder.
3. Replace A and B in the roots that the VM reads without a check: the
   stack frames, the saved frames, the interpreter and machine-code
   registers, the special objects.  Not the JIT caches: they hold only
   methods, classes and selectors, which this path does not accept.
4. Set the global flag "forwarders exist".  It stays set until a full GC
   removes every forwarder.

References in the heap still point to A and B.  Every place that uses a
reference as a real object follows a forwarder first (the read barrier):

- A send follows its receiver and writes it back to the stack.  A JIT
  inline cache never holds the forwarder's class index, so a forwarded
  receiver misses and reaches the interpreter's send path.
- A primitive follows its receiver and arguments (top level) before it
  runs, when the flag is set.
- `==` and `~~` follow both operands when they differ and the flag is set
  (interpreter bytecode, primitive 110, T1 inline code, the MCF `=` site).
- `perform:` and the MCF perform path follow the receiver (refuse the
  native path for a forwarder).

A full GC first replaces every reference to a forwarder, in the heap and
in the roots, with the target (one pass, only when the flag is set).  The
forwarders are then unreferenced, so the mark does not reach them and the
sweep frees them.  The flag clears.

## Not on the lazy path

The current paths stay for pairs where either object is: compiled code,
a class or metaclass, a symbol, a method dictionary, a context, a
process, semaphore or linked list, a literal binding (any LookupKey: the
JIT reads a binding's value slot directly), a weak or ephemeron object,
a pinned or immutable object, an object in the special objects array, or
an object with an overflow header.

## Stages

1. Full-GC unforward pass and the flag.
2. Lazy become install, the narrow root replace.
3. Barriers: send receiver, primitive pre-follow, `==`, perform.
4. Audit for other places that read an object without a send (interpreter
   fast paths, T1 inline paths, C++ helpers); probes for each.
5. Sweep with the knob on; then make it the default.

## As built (2026-10-15)

- One-way become (`becomeForward:`) uses the same scheme without copies:
  each source becomes a forwarder to its target.  Slot 0 is stored with
  the write barrier, so an old forwarder to a young target is remembered;
  pointerSlotsOf answers one pointer slot (the target) for a forwarder, so
  the scavenge updates it when the target moves.
- Repeated swaps of the same objects build forwarding chains in heap
  references that nobody followed.  An unforward pass runs after every 256
  lazy pairs, and followForwarded's cap is 512 hops.
- IC fills key by the followed receiver: an entry keyed with the
  forwarder's class index and the target's method ran that method with a
  forwarder as self (found with the JIT on, ReStore).
- Without the knob, both become primitives fix the roots in one pass for
  all pairs, without the JIT cache roots when no cache can hold the
  objects.

Measured with the knob: ReStore package 282 s (Cog x86 346 s), NewTools
77 s (Cog 372 s).  Probes lazybecome.st / lazyfwd.st in the session
scratchpad match Cog with the knob, without it and without the JIT.
