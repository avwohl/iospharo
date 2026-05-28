# Pharo 13 image issues we patch

This is the consolidated list of bugs in the standard Pharo 13 image
(`/tmp/harness/Pharo.image`, fetched via `https://get.pharo.org/64/130`)
that we need to patch locally for our VM to work the same way as
stock Cog.  See also `docs/deferred.md` for VM-side issues.

## `Character >>` numeric coercion missing — blocks Metacello load

**Symptom:** any Metacello fetch of a non-preinstalled package fails
deep inside `SHA1>>processBuffer:` → `expandedBlock:` →
`ThirtyTwoBitRegister>>loadFrom:at:` with

       Instance of Character did not understand #bitShift:

**Cause:** `ThirtyTwoBitRegister>>loadFrom:at:` does
`(arg1 at: arg2) bitShift: 8` where `arg1` is a ByteString.  In Pharo
`ByteString>>at:` returns a `Character`, not an `Integer`, so the
pure-Smalltalk SHA1 path needs `Character>>bitShift:` to be defined.
Stock Pharo gets away with this because `SHA1>>primHasSecureHashPrimitive`
returns true (the C plugin handles the whole hash) and the
pure-Smalltalk fallback never runs.

**Patch:** `scripts/patches/character_numeric_coercion.st` adds:

* `Character>>bitShift:` `bitAnd:` `bitOr:` `bitXor:` — convert
  receiver to Integer then forward.
* `Character>>adaptToInteger:andSend:`,
  `adaptToFloat:andSend:`, `adaptToNumber:andSend:` — same trick
  for `10 + $A` style coercion (PolyMath needs these once SHA1 is
  past).

**Verified:** with both sets of patches in place, the following
Metacello load completes:

       Metacello new
         baseline: 'PolyMath';
         repository: 'github://PolyMathOrg/PolyMath/src';
         load.

309 PM classes installed, no errors.

**To apply to a fresh image:**

       cp /tmp/harness/Pharo.image /tmp/harness/Pharo-patched.image
       cp /tmp/harness/Pharo.changes /tmp/harness/Pharo-patched.changes
       /tmp/harness/pharo --headless /tmp/harness/Pharo-patched.image \
         eval --save \
         "'scripts/patches/character_numeric_coercion.st' \
           asFileReference fileIn"

**Upstream wishlist:** the right fix is for `ThirtyTwoBitRegister`
(or the surrounding code) to use `asInteger`/`codePoint`/`asciiValue`
explicitly so it works without depending on either VM behaviour.
