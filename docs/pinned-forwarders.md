# Forwarders left by a pin

## Cause

Primitive 184 (`pin:`) on a young object does not pin the object in
place.  `ObjectMemory::tenureByForwarding` copies the object to old
space, pins the copy, and turns the original into a forwarder (Spur does
the same).  The VM sets `g_lazyForwarders`.  The next scavenge rewrites
all references to the forwarder and clears the flag.

Until that scavenge, each slot that held the original still holds the
forwarder.  Example: `SDL_CreateSurfaceFrom` pins the Bitmap in a Form.
The `bits` slot of the Form keeps the forwarder.  The stack copy of the
receiver is the only reference that the pin primitive can update.

The VM cannot find the other references at pin time.  A search needs a
scan of the young space and of the remembered set for each pin.  A pin
happens on each FFI call that gets a Bitmap, so the VM does not scan.

## Contract

Spur: a primitive that meets a forwarder fails.  Then the VM follows the
forwarders in the receiver and the arguments.  A table gives the depth
for each primitive.  The VM runs the primitive once more
(`checkForAndFollowForwardedPrimitiveState`).  The primitive can fail
because every Spur primitive tests the format of its operands, and the
format of a forwarder is 7.

This VM:

- `executePrimitive` follows a forwarder in the receiver and in each
  argument before the call (docs/lazy-become-design.md).
- `Interpreter::callPrimitive` does the same for the sends that call a
  cached primitive pointer.  Before 2026-11-13 these sends skipped the
  check.  `String class>>translate:from:to:table:` and
  `findSubstring:in:startingAt:matchTable:` on a pinned String then
  failed or answered 0 (x86_64 and arm64 differ in the send path).
- A named or external primitive (primitive 117) can fail while
  forwarders exist.  The VM then follows the fields of its receiver and
  arguments to depth 2 (BitBlt, Form, bits) and runs the primitive once
  more.  The VM rewrites the slots, so the next use finds the target.
  The constant `kPrimitiveFollowDepth` holds the depth.  The scan reads
  at most 256 slots of an object.
- The BitBlt and WarpBlt primitives do not fail on a forwarded `bits`:
  they read the slot as a Bitmap.  They call `followForwarded` at each
  read of the bits (`fa07c935`, `c9c79751`).  A new primitive that
  reads a field of an operand without a format test must do the same.

With no forwarder in the heap, the cost is one test of
`g_lazyForwarders` for each primitive call.

## Test

`scripts/cogdiff/pinforward.st` pins a young Bitmap, ByteArray, String,
LargeInteger or WordArray that an Array, a Form or a stream holds.  It
then runs the primitives that read the object.  Each case runs twice,
with and without the pin.  Both answers must agree, and must equal the
answers of stock Cog.
