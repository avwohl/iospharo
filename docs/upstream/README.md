# Changes for upstream projects, for review by the owner

This directory has the changes that this project prepared for the stock Cog
VM (`pharo-project/pharo-vm`) and for the Pharo image.  The owner reviews
each change before anyone sends it.  This project pushed no change of this
directory to a pharo-project repository, and opened no issue and no pull
request.

Each Cog change is also a local branch in `~/src/pharo-vm`, made from
`origin/pharo-12`.  A patch file here is the copy that stays if that clone
is lost.

## How to build stock Cog on this Mac

The pinned code-generation VM of the Cog build does not start on macOS 27
(see the second change).  Give the build a stock VM that starts:

```
cmake -S ~/src/pharo-vm -B <build-dir> \
  -DGENERATE_PHARO_VM=/tmp/harness/pharo-vm/Pharo.app/Contents/MacOS/Pharo
cmake --build <build-dir> -j4
```

The VM is then `<build-dir>/build/vm/Debug/Pharo.app/Contents/MacOS/Pharo`.
The stock VM in `/tmp/harness` comes from
`curl -sL https://get.pharo.org/64/130+vm | bash`.

## Cog changes

| Change | Branch in `~/src/pharo-vm` | Patch file | State |
|---|---|---|---|
| SocketPlugin: local address from any interface that is up | `fix/resolver-local-address-any-interface` | `cog-socketplugin-local-address.patch` | committed on the branch, tested |
| Typed store primitives respect the write barrier | `fix/write-barrier-typed-stores` | `cog-write-barrier-typed-stores.patch` | committed on the branch, tested |
| `primitiveDoNamedPrimitiveWithArgs`: `PrimErrNotFound` | `fix/named-primitive-not-found-in-simulation` | `cog-named-primitive-not-found.patch` | committed on the branch, tested |
| Code-generation VM for macOS arm64 that starts on macOS 27 | `fix/vmmaker-bootstrap-vm-darwin-arm64` | `cog-vmmaker-bootstrap-vm-darwin-arm64.patch` | tested, not committed: the change is a version pin, and the owner must approve a pin |

### SocketPlugin: local address from any interface that is up

- Defect: `sqResolverLocalAddress` accepts only interfaces with the names
  `eth0` and `wlan0`.  On macOS the primitive answers 0.0.0.0, and
  `NetNameResolver localHostAddress` is then the loopback address on a
  connected machine.  The image has
  `NetNameResolverTest>>testLocalHostAddress` set to skip as "currently
  broken".
- Change: take the first IPv4 address of an interface that is up and is not
  the loopback.
- Test (2026-11-17, macOS 27, arm64, a Debug build of the branch):
  `NetNameResolver primLocalAddress` answers `#[10 3 0 128]`.  The stock VM
  answers `#[0 0 0 0]`.  Seven network test classes give the same results on
  the two VMs (73 tests pass, 1 skip).
- Our VM had its own defect in the same primitive (a stub that answered
  127.0.0.1).  This repository has the fix for that defect.

### Typed store primitives respect the write barrier

- Defect: the primitives 615 to 629 (`primitiveStore*IntoBytes`) and
  `primitiveFFIIntegerAtPut` do not test the receiver for immutability.  A
  typed store into a read-only `ByteArray` changes the bytes, and the VM
  signals no `ModificationForbidden`.
- Change: each primitive fails with `PrimErrNoModification` for a read-only
  receiver, as `primitiveAtPut` does.
- Test (2026-11-17, a Debug build with the change): `WriteBarrierTest` gives
  33 passes of 33 with the image fixes of this repository.  The stock VM
  fails 4 of the 33: the two tests that upstream marked as expected failures
  (`testMutateByteArrayUsingDoubleAtPut`, `testMutateByteArrayUsingFloatAtPut`)
  and the two integer tests that this repository adds.
- The image has a defect in the same path:
  `TFFIBackend>>on:float64At:put:` and `on:float32At:put:` test `self`
  (the backend) for read-only, not `anObject`.  The image fix is in
  `scripts/image-fixes/20-WriteBarrierTest.st`.
- Our VM had a related defect (`primitiveFFIIntegerAtPut` wrote into a
  read-only object).  This repository has the fix for that defect.

### `primitiveDoNamedPrimitiveWithArgs`: `PrimErrNotFound`

- Defect: for a named primitive that does not exist, the real call fails with
  `PrimErrNotFound`, and the simulated call (primitive 218, used by
  `Context>>tryNamedPrimitiveIn:for:withArgs:`) fails with the generic
  code -1.  The debugger simulation then gives another error code than the
  real execution.  The image has `CodeSimulationTest>>testErrorCodeNotFound`
  set to skip.
- Change: primitive 218 fails with `PrimErrNotFound` when the function does
  not exist.
- Test (2026-11-17, a Debug build with the change): `CodeSimulationTest`
  gives 9 passes of 9 with the skip removed.  The stock VM fails
  `testErrorCodeNotFound` ("Got #'not found' instead of nil").
- Our VM gave nil for the real call and for the simulated call.  This
  repository has the fix for that defect.

### Code-generation VM for macOS arm64

- Defect: `cmake/vmmaker.cmake` downloads the stock VM 10.3.2 to generate the
  C sources.  On macOS 27 that VM stops at startup with "Could not allocate
  codeZone in the expected place (0x320000000)", and the build stops at the
  `vmmaker` step.
- Change: download the release `v10.3.12+0.4b0622446` for macOS arm64.  The
  change has the URL and the SHA256 of that archive, in the form that the
  file uses for all platforms.
- Test (2026-11-17): a build of the branch with no `GENERATE_PHARO_VM`
  completes, and the VM that the build makes answers `42 factorial`.
- Not checked: the other platforms keep their old VM, and no check was made
  that those start on current systems.
