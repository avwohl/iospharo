# Cog changes: the defect, the change and the test of each one

This file has one section for each row of the table "Cog changes" in
[README.md](README.md).  Each change is a local branch in `~/src/pharo-vm`
and a patch file in this directory.  The owner reviews each change before
anyone sends it.

## SocketPlugin: local address from any interface that is up

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

## Typed store primitives respect the write barrier

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

## `primitiveDoNamedPrimitiveWithArgs`: `PrimErrNotFound`

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

## Code-generation VM for macOS arm64

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

## FFI: a callback from another thread with the same-thread runner

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

## SocketPlugin: a failed `bind` or `listen` of the listen primitives gives an error

- Defect: `sqSocketListenOnPortBacklogSizeInterface` does not test the
  result of `bind` and of `listen`.  `Socket>>setPort:` of a UDP socket uses
  that function.  On a port that another socket has, the image gets no
  error, the socket stays unbound, and it receives nothing on that port.
- Measurement (`scripts/cogdiff/socket_bind_in_use.st`, 2026-11-17): the
  stock VM answers "no error" for the second `setPort:` of one UDP port, and
  the second socket does not have the port.  Our VM signals
  `SocketError: Address already in use`.  For TCP the image uses
  `primitiveSocketBindToPort`, which tests the result, so the two VMs agree
  there.
- Found by: the package NetworkExtras of `bouraqadi/PharoMisc`.  Its tests
  make two multicast sockets on one port and passed on the stock VM only
  because the second bind failed with no error.
- Change: the function records the socket error and fails the primitive, as
  `sqSocketBindToPort` does.
- Test: with the change, the script gives `SocketError: Address already in
  use` for UDP on the patched Cog.
- Not tested: Windows and Linux.

## SocketPlugin: an integer option value of each length, and `SO_LINGER`

- Defect 1: `sqSocketSetOptions...` reads an integer only from a value of
  `sizeof(int)` characters or less.  The image gives the value as a string.
  `Socket>>setOption: 'SO_RCVBUF' value: 65536` goes to `setsockopt` as the
  5 characters `65536`, and the system reads the first 4 of them as the
  `int`.  On macOS the buffer becomes 8 MB.
- Defect 2: `SO_LINGER` is in the option table, but `setsockopt` gets one
  `int` for it, and the system needs a `struct linger`.  The primitive
  fails, so the image cannot set the option.
- Measurement (`scripts/cogdiff/socket_options.st`, 72 option calls on a UDP
  socket and on a TCP socket): the stock VM and our VM differ in these 4
  lines only.
- Change: a value that reads as an integer is an `int` of each length.  For
  `SO_LINGER` the value is the linger time in a `struct linger`, 0 sets the
  option off, and the get primitive answers the linger time.
- Test (2026-11-17, macOS 27, arm64): with the change, the patched Cog and
  our VM give the same answer for each of the 72 calls of the script.
- Not tested: Windows and Linux.
