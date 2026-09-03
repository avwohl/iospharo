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

That XMLParser cluster is the last unexplained arch-specific failure in the
package tier.  Two attempts to reproduce it in isolation both died on harness
problems rather than on the tests -- a stale `startup.st` in the shared working
directory the first time, and eval mode's `startup.st` staging colliding
between two VMs the second.  A third attempt runs each VM from its own
directory.
