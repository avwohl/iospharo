# Package tier on both arches with the storm guard, 2026-09-03

`scripts/package-tests-selfhosted.sh` with `REUSE_FROM` the already-loaded
package images, `PER_CLASS_TIMEOUT=200` (arm64) / `400` (x86_64).

    arch      classes   PASS   FAIL  ERROR  TIMEOUT
    arm64       354     9383    16     17      0
    x86_64      354     9376    16     24      0

Against the same packages before the guard build:

    arm64   9326 P / 16 F / 16 E / 1 T   ->   9383 P / 16 F / 17 E / 0 T
    x86_64  9376 P / 16 F / 24 E / 0 T   ->   unchanged

arm64 gains 57 passes and loses its one timeout.  Both arches now agree
exactly on FAIL: 14 DataFrame + 2 PolyMath, the same selectors.

## The one arch difference left in this tier

x86_64 reports 7 more errors than arm64.  Five are `XMLParser`:
`XMLParserTest>>testAttributeDefaultValue{Entity,IDRef,Entities,Nmtokens,IDRefs}`,
a coherent cluster around attribute-default validation, clean on arm64.  The
other two are PolyMath, whose per-class timeout boundary the two arches hit
differently.

**Resolved the same morning: that cluster is not a VM failure.**  Isolated (each
VM in its own directory, after two attempts died on `startup.st` in a shared
one), the four reproduce on x86_64 and pass on arm64 -- and `TestCase>>debug`,
which does not trap, names the exception as **`TestTookTooMuchTime`**.  That is
SUnit's own per-test `timeLimit`, recorded by a TestResult as an ERROR.  The
stack is string-heavy DTD work (`XMLAttributeValidator>>furtherNormalize
AttributeValue:`, `String>>format:`, `WideString>>copyFrom:to:`), which is
where a 2x-slower machine shows up first.

So the whole x86-vs-arm ERROR gap in this tier is Rosetta speed, not codegen,
and the package tier has no unexplained arch-specific failure left.  What
remains is a performance question, shared with the two whole-image scans that
time out on both arches.
