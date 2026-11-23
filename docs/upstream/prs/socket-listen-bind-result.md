# SocketPlugin: fail the listen primitive when bind or listen fails

Local data (not part of the PR text):

- Upstream repo: pharo-project/pharo-vm, base branch pharo-12
- Local branch: pr15/socket-listen-bind-result
- Work tree: /Users/wohl/pharo-work/wt/vm-socket-listen-bind-result
- Commit: 229cf8779 SocketPlugin: fail the listen primitive when bind or listen fails
- Related issue: pharo-project/pharo#11723 (closed).  The records do not show whether the VM function changed, so this text makes no claim that the cause is the same
- Source in this project: Cog branch fix/socket-listen-bind-result, measurement scripts/cogdiff/socket_bind_in_use.st, docs/upstream/cog-changes.md
- Probe: scripts/upstream-pr/probes/socket-listen-bind-result.st
- Review notes (round 1): The author corrected the commit trailer.  The author shortened the comment in the C code.  The author replaced the Notes bullet on the reuse option.  The author stated the platform of the measurement.  The author pasted the probe code into the PR text.  The author rejected no finding.
- Review notes (round 2): The author removed the TCP reproduction as evidence, because in the Pharo 15 image Socket>>listenOn:backlogSize: uses primitiveSocketBindToPort and primitiveSocketListenWithBacklog, not the changed function.  Only Socket>>setPort: reaches the changed function (grep of src/Network-Kernel in the Pharo tree).  The author labelled the TCP lines of the probe as a control.  The author changed the commit title and body.  The reproduction closes its sockets.  The author rejected no finding.

## Pull request text

Socket>>setPort: on a UDP socket calls sqSocketListenOnPortBacklogSizeInterface in the SocketPlugin. This function ignores the results of bind() and listen(). A failed bind gives no error to the image. This change makes the primitive fail, with the system error, as sqSocketBindToPort does.

Related: pharo-project/pharo#11723

### The defect

Socket>>setPort: on a UDP socket uses the listen primitive. If another socket has the port, the system refuses the bind, but the primitive reports success. The socket stays unbound and receives nothing on that port. The image signals no error.

Reproduction in a clean Pharo 15 image with the stock VM:

```smalltalk
| a b |
a := Socket newUDP. b := Socket newUDP.
[ a setPort: 0.
  b setPort: a localPort.
  b localPort = a localPort ]
	ensure: [ a closeAndDestroy. b closeAndDestroy ]
```

Observed (measured on macOS arm64 only): no error, and the last expression answers false.
Expected: the call to setPort: on the second socket signals SocketError (Address already in use).

We expect the reproduction to fail on macOS-like systems only. The plugin sets SO_REUSEADDR on every new socket (createSocket). A second UDP bind to a busy port can therefore succeed on Linux or on Windows, and the result depends on the system. The result on Linux and on Windows was not measured.

In the Pharo 15 image, only Socket>>setPort: (primitiveSocketListenWithOrWithoutBacklog) reaches the changed function. Socket>>listenOn:backlogSize: uses primitiveSocketBindToPort and primitiveSocketListenWithBacklog. These primitives do not call the changed function, and sqSocketBindToPort already tests bind(). A TCP listen on a busy port therefore signals SocketError on stock Cog too.

Only older images that use the primitives primitiveSocketListenOnPortBacklog and primitiveSocketListenOnPortBacklogInterface reach the TCP branch of the changed function (bind and listen for a TCP socket). The change to this branch is defensive, and no probe tests the failure of listen().

### The cause

sqSocketListenOnPortBacklogSizeInterface in plugins/SocketPlugin/src/common/SocketPluginImpl.c calls bind() and (for TCP) listen() and drops the results. The other bind sites in the same file test the result.

### The change

The function tests the results of bind() and listen(). On failure it stores getLastSocketError() in PSP(s)->sockError, calls success(false) and returns. The change follows the pattern of sqSocketBindToPort. The image then signals SocketError with the system text.

### Tests

The plugin is C code, so no Slang test applies. The probe file scripts/upstream-pr/probes/socket-listen-bind-result.st answers a text.

The UDP lines are the proof: they differ between the stock VM and the patched VM. The TCP lines are a control: they are the same on both VMs. The validation below states the measured result.

### How it was tested

Note: the probe results below with the "patched" VM come from an earlier build. That build had an earlier form of this commit, before the review. The results of the final form of the branch are in "Build of this branch alone, and of the set" at the end of this section.

Versions: the image is Pharo 15 build 141 (b4d2dfd). The stock Cog is v12.0.6-beta (bebdbaca8). The patched Cog is pharo-12 at 398023142 plus the fix commits of this project (including this commit). The platform is macOS arm64.

Probe scripts/upstream-pr/probes/socket-listen-bind-result.st, run with the eval mode on the Pharo 15 image. One run on each VM:
- UDP, second socket calls setPort: on the port of the first socket. Stock Cog: "no error", and the second socket does not have the port. Patched Cog: SocketError (Address already in use). The defect shows before the change. After the change the image signals the error.
- TCP control, second socket calls listenOn:backlogSize: on a busy port. Stock Cog and patched Cog both give SocketError (Address already in use). The TCP path of the image uses primitiveSocketBindToPort, which already tests bind(), so the answer is the same on both VMs, as expected.

No SUnit proof test and no VMMakerTests test applies, because the change is in the C code of the SocketPlugin. No regression class was run.

Build of this branch alone, and of the set:

We also built this branch alone (2026-11-23, `scripts/upstream-pr/vm-pr-build.sh`, pharo-12 at 398023142 plus this branch).  The probe on that VM: the UDP setPort: on a busy port signals "SocketError: Address already in use".  The stock VM gives no error.

The five VM branches of this set together (branch `pr15/all-five`,
built alone) ran the whole Pharo 15 suite with the six image pull
requests of the set: 40369 tests, 40278 pass, 2 fail, 88 skip, 1
expected failure.  The skips are the same as on the stock VM.  The two
failures do not come from this change:
`CodeSimulationTest>>testErrorCodeNotFound` needs another Cog change, and
`ZnClientTest>>testQueryGoogle` depends on the answer of Google and fails
on the stock VM too.

### Notes

- A caller that relied on the silent failure now gets a SocketError. A caller that wants two sockets on one port must use an explicit reuse option or another port. The plugin already sets SO_REUSEADDR on every socket, so the result for UDP depends on the system.
- After a failed bind, the image must close the socket. The check was not made for every caller.
- The Windows code path shares this function. Windows and Linux were not tested.

Generated with [Claude Code](https://claude.com/claude-code)
