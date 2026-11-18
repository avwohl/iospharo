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
(see "Code-generation VM for macOS arm64" in
[cog-changes.md](cog-changes.md)).  Give the build a stock VM that starts:

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
| SocketPlugin: a failed `bind` or `listen` of the listen primitives gives an error | `fix/socket-listen-bind-result` | `cog-socket-listen-bind-result.patch` | committed on the branch, tested |
| SocketPlugin: an integer option value of each length, and `SO_LINGER` | `fix/socket-option-values` | `cog-socket-option-values.patch` | committed on the branch, tested |
| Spur: low space before the old-space limit | `fix/low-space-signal-before-old-space-limit` | `cog-low-space-before-old-space-limit.patch` | committed on the branch, tested |
| NewFilePlugin: checks of the buffer and of the handles | `fix/newfile-plugin-buffer-and-handle-checks` | `cog-newfile-plugin-buffer-and-handle-checks.patch` | committed on the branch (2 commits), tested |
| Cogit: a followed literal goes back into the method with no store check (a segmentation fault in a full GC) | `fix/indexforselector-store-check` | `cog-indexforselector-store-check.patch` | committed on the branch, tested with the package row that showed the crash |

The defect, the change and the test of each row are in
[cog-changes.md](cog-changes.md).

## Cog defects with no change yet

- A segmentation fault in the garbage collector in the PharoMisc row of the
  package run (`TkThreadTest>>testFinalization`, with three sockets on one
  port).  The cause is not known.
- A segmentation fault of `MorphicWindowManagerTest` in an interactive
  session (1 of 3 runs).
