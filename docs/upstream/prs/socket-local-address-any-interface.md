# SocketPlugin: local address from any interface that is up, not only eth0 and wlan0

Local data (not part of the PR text)

- Upstream repo and base branch: pharo-project/pharo-vm, pharo-12
- Local branch: pr15/socket-local-address-any-interface
- Work tree: /Users/wohl/pharo-work/wt/vm-socket-local-address-any-interface
- Commit: 6cad97900, title "SocketPlugin: local address from any interface that is up, not only eth0 and wlan0" (run git log in the work tree for the hash)
- Related issues: pharo-project/pharo#4394 (closed, and the image answers 127.0.0.1 as a fallback). No open pharo-vm issue found.
- Origin in this project: Cog branch fix/resolver-local-address-any-interface, docs/upstream/cog-socketplugin-local-address.patch, docs/upstream/cog-changes.md, image-side entry N1/N2 in scripts/image-fixes/20-network-tests.st
- Probe: scripts/upstream-pr/probes/socket-local-address-any-interface.st
- The Pharo image test NetNameResolverTest>>testLocalHostAddress has "self skip" in Pharo 15. A later image PR can remove the skip and the 127.0.0.1 fallback in NetNameResolver class>>localHostAddress when a VM with this fix is the minimum. That image PR is not part of this PR.
- Review notes (round 1): the commit now keeps the original indentation of unchanged lines, which answers the re-indent finding. The Tests section states the probe finding. The Notes cover the fallback and getnameinfo findings. The commit now follows Simplified Technical English, which answers the STE findings.

## Pull request text

### Summary

The primitive that answers the local IPv4 address (sqResolverLocalAddress in the SocketPlugin) accepts only network interfaces with the names eth0 and wlan0. On macOS and on Linux with predictable interface names, the primitive answers 0.0.0.0. This change takes the first IPv4 address of an interface that is up and is not the loopback.

Related: pharo-project/pharo#4394 (the image hides the defect with a fallback to 127.0.0.1).

### The defect

On macOS (interfaces en0, en1, ...) the primitive answers 0.0.0.0. The same happens on Linux with names such as enp3s0 and wlp2s0. `NetNameResolver primLocalAddress` answers #[0 0 0 0], also when the machine has a network connection. `NetNameResolver localHostAddress` then answers the loopback address 127.0.0.1 in the image.

To reproduce, run in a clean Pharo 15 image with the stock VM on a connected machine:

```smalltalk
NetNameResolver initializeNetwork.
NetNameResolver primLocalAddress
```

Observed: #[0 0 0 0]. Expected: the address of the interface with the connection, for example #[10 3 0 128].

In the Pharo 15 image, NetNameResolverTest>>testLocalHostAddress has "self skip" because the test is "currently broken". The cause is this defect.

### The cause

sqResolverLocalAddress in plugins/SocketPlugin/src/common/SocketPluginImpl.c loops over the result of getifaddrs. The loop accepts an entry only if ifa_name is equal to "eth0" or "wlan0". The function does not find another interface, and answers 0. The source had a TODO that the code does not work with more than one network interface. Two early returns did not call freeifaddrs.

### The change

The loop now accepts the first AF_INET address of an interface that has IFF_UP and does not have IFF_LOOPBACK. An interface that also has IFF_RUNNING, with an address outside 169.254/16 (link-local), has priority over an interface that is only up. If no such interface exists, the answer stays 0.0.0.0.

The function now calls freeifaddrs on all paths after getifaddrs succeeds. The change adds the include of net/if.h for the IFF_ flags. The change removes the getnameinfo call and its failure path (success(false)), because the code reads the address directly from sin_addr. The Windows branch is not changed.

### Tests

The image skips the test NetNameResolverTest>>testLocalHostAddress in Pharo 15 because of this defect, so the test does not show the defect. A VM test in VMMakerTests does not apply, because the code is a C function of an external plugin that the simulation does not run.

The probe scripts/upstream-pr/probes/socket-local-address-any-interface.st calls `NetNameResolver primLocalAddress` directly. The stock VM answers #[0 0 0 0] and the patched VM answers the address of the connected interface. The two VMs give different answers only on a connected machine whose interfaces are not named eth0 or wlan0. On a Linux machine with eth0 or wlan0, or with no network, both VMs give the same answer.

The repository has no automated test that fails before the change. The classes NetNameResolverTest, TCPSocketTest, UDPSocketTest, SocketStreamTest and SocketAddressTest are the regression check.

### How it was tested

Note: the probe results below with the "patched" VM come from an earlier build. That build had an earlier form of this commit, before the review. The results of the final form of the branch are in "Build of this branch alone, and of the set" at the end of this section.

Versions: Pharo 15 image build 141 (b4d2dfd). Stock VM: Cog v12.0.6-beta (bebdbaca8). Patched VM: pharo-12 at 398023142 plus the fix commits of this project, including this commit unchanged.

Machine: macOS arm64, interface en6 at 10.3.0.143 at the time of the run.

Probe scripts/upstream-pr/probes/socket-local-address-any-interface.st, run in the clean Pharo 15 image:
- Stock VM: LOCAL-ADDRESS=#[0 0 0 0] USABLE=false (the defect).
- Patched VM: LOCAL-ADDRESS=#[10 3 0 143] USABLE=true.

Image test NetNameResolverTest>>testLocalHostAddress: SKIP 3 of 3 on the stock VM and SKIP 3 of 3 on the patched VM. The test begins with "self skip", so the test cannot show the defect. The probe is the proof of this change.

Regression classes, run once on each VM in the same image. The result is the same on both VMs:
- NetNameResolverTest: 1 SKIP (testLocalHostAddress) and 1 PASS (testLocalHostName).
- TCPSocketTest: 9 PASS, no failure.
- UDPSocketTest: 2 PASS, no failure.
- SocketStreamTest: 24 PASS, no failure.
- SocketAddressTest: 5 PASS, no failure.

The class SocketTest does not exist in Pharo 15. The classes TCPSocketTest, UDPSocketTest, SocketStreamTest and SocketAddressTest took its place in the regression check.

The tests were not run on Linux or Windows. The tests do not use a VMMakerTests test, because the change is in the C code of an external plugin.

Build of this branch alone, and of the set:

The author also built this branch alone (2026-11-23, `scripts/upstream-pr/vm-pr-build.sh`, pharo-12 at 398023142 plus this branch).  The probe on that VM: LOCAL-ADDRESS=#[10 3 0 143] USABLE=true.  The stock VM answers #[0 0 0 0].

The five VM branches of this set together (branch `pr15/all-five`,
built alone) ran the whole Pharo 15 suite with the six image pull
requests of the set: 40369 tests, 40278 pass, 2 fail, 88 skip, 1
expected failure.  The skips are the same as on the stock VM.  The two
failures do not come from this change:
`CodeSimulationTest>>testErrorCodeNotFound` needs another Cog change, and
`ZnClientTest>>testQueryGoogle` depends on the answer of Google and fails
on the stock VM too.

### Notes

- The author measured the change on macOS 27 (arm64) only, with interface en6 at 10.3.0.128. Test on Linux before the merge.
- The selection rule (first interface that is up and is not the loopback, with a preference for running and not link-local) is a design choice. A machine with more than one interface still gets one answer, as before. The order of getifaddrs gives the answer.
- If no interface is running with a non-link-local address, the function uses the first interface that is up and is not the loopback. That interface can have a link-local address (169.254/16), or can be an unplugged interface that has an address.
- A machine with no usable interface still gets 0.0.0.0. The image test testLocalHostAddress also depends on NetNameResolver isConnected. Check the test on a machine without a network when the image PR removes the skip.
- The image PR that removes the skip and the 127.0.0.1 fallback comes after this PR.

Generated with [Claude Code](https://claude.com/claude-code)
