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
| FFI: a callback from another thread with the same-thread runner | `fix/same-thread-runner-callback-from-other-thread` | `cog-same-thread-runner-callback-from-other-thread.patch` | committed on the branch (2 commits), tested |

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
  receiver, as `primitiveAtPut` does.  The JIT has machine code versions of
  the 32-bit and 64-bit stores.  The machine code now leaves without a store
  when the receiver is read-only.  Without that part, a store was blocked in
  the interpreter and went through after the JIT compiled the method.
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

### FFI: a callback from another thread with the same-thread runner

- Defect: `sameThreadCallbackEnter` calls `ptEnterInterpreterFromCallback`
  on the thread that made the callback.  When that thread is not the VM
  thread, two threads run the interpreter, and the VM stops with a
  segmentation fault.  The image has
  `TFUFFICallbackTest>>testCallbacksInOtherThread` set to skip for this
  runner, with that cause in a remark.
- Change, first commit: for a callback from another thread, the calling
  thread waits on a platform semaphore.  `callbackFrontend` already puts
  each invocation in the queue of pending callbacks, so the image runs the
  callback in the VM thread.  `sameThreadCallbackExit` gives the signal.
  The worker runner does the same for a thread that is not its own.
- Change, second commit, from a review of the first:
  - A lock for the callback stack of a runner.  `callbackFrontend` pushed
    with no lock from any thread, and `primitiveCallbackReturn` tested and
    popped in two steps.  The worker runner had the same exposure.
  - The runner keeps the VM thread as a `pthread_t` from
    `primitiveGetSameThreadRunnerAddress`.  `ioVMThread` is set only in a
    build with `PHARO_VM_IN_WORKER_THREAD`, and on Windows its type cannot
    hold a `pthread_t`.
- Test (2026-11-17, macOS 27, arm64, the stock app with the core library of
  the changed build): the 14 tests of `TFUFFICallbackTest` pass, 8 runs of
  8.  `testCallbacksInOtherThread` passes for the same-thread runner and
  for the worker runner.  On the stock VM the same-thread case stops the
  VM, so the test skips there.
- Not tested: Windows and Linux, and two threads that call back at the same
  time.
- The image side: `scripts/image-fixes/30-ffi-callback-from-other-thread.st`
  keeps the skip only for a VM without the change.  Our VM always ran such
  a callback in the VM thread.

## Cog changes in work

- Low space before the old-space limit
  (`fix/low-space-signal-before-old-space-limit`, a change in
  `SpurMemoryManager>>sufficientSpaceAfterGC:`, in the worktree, not
  committed, not built).  With `--maxOldSpaceSize`, a recursion with no end
  stops the VM with "Could not allocate new object in the old space", and
  the image gets no low-space signal.  The threshold of the image is 400000
  bytes, and one scavenge can move more than that to the old space.
