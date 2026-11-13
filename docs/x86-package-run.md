# Pharo 14 package run on a real x86_64 host

This file is the task list for a session on an x86_64 host.  The same run
was done on an arm64 Mac, where stock Cog ran under Rosetta.  This host
gives the first native x86_64 numbers.

Read `CLAUDE.md`, then the top section of [WIP.md](WIP.md), then
`scripts/pkg-jit-test/README.md`.

## Goal

Run the Pharo 14 package tests on our VM and on stock Cog, and find the
cause of each difference.

## Steps

1. Build a Release tree of our VM for x86_64.  The Linux x86_64 build is
   not validated since `main` was merged into `jit`.  Fix build errors
   first.  Smoke test:
   `./build/test_load_image <image> eval "42 factorial printString"`.
2. Install stock Pharo 14 with the stock VM in a scratch directory:
   `curl -sL https://get.pharo.org/64/140+vm | bash`.  Make sure that
   `eval`, `eval --save` and a Metacello `github://` load work.  On a new
   Linux, the libgit2 of the stock VM needs OpenSSL 1.1 and libssh2.
3. Run each `scripts/cogdiff/*.st` script on stock Cog and on our VM (JIT
   on, `PHARO_NO_JIT=1`, `PHARO_NO_MCF=1`).  Compare the answers.
   `nlrhome.st` and `callbackstress.st` are new and must give the Cog
   answers.  [vm-compat-bugs.md](vm-compat-bugs.md) lists the known
   deliberate differences.
4. Run the full manifest: `scripts/pkg-jit-test/run-manifest.sh` with
   `packages-200.tsv`.  Stock Cog loads each package.  The two VMs run the
   tests.  Copy `summary.tsv` and the logs to a safe place during the run.
5. For each package where the two VMs differ, run the tests that differ
   alone, some times on each VM.  Put each difference in one class: a
   defect of our VM, a package defect that Cog shows too, or a harness
   defect.  Fix a proven VM defect at the root cause.
6. Write the results into [WIP.md](WIP.md): packages loaded, load failures
   by cause, total passes on each VM, and each difference with its class.

## Results of the arm64 run, for comparison

- 151 of 200 packages load on Pharo 14.  49 do not load on any VM.
- Before the fixes on `jit`: Cog 30436 passes, our VM 30414.  With the
  fixes, `evref-bl-mcp` gives 860 passes, 1 failure and 1 error on each VM.
- `pharo-cig`: the package declares the 16-byte `CXString` struct as one
  pointer.  The run can abort.  This is not a VM defect.
- `smalltalkweb-myprecious`: under Rosetta, Cog did not complete a socket
  reconnect while a package busy loop ran.  Our VM did.  Find which VM
  does what on this host.  This question is open.
- `newapplesho-google-cloud-smalltalk`: Cog crashed in libcrypto under
  Rosetta.  Do the check again on this host.
- `pharo-contributions-mutalk` (`MTCompositeTestFilterTest`) and
  `apptivegrid-soil` (`SoilCleanCodeTest>>testCodeCoverage`) differ only
  because of a time limit.

## Rules for this session

- Commit to `jit` only.  Pull with rebase before each push.  Do not
  force-push.  Do not push to `main`.
- Do not start a second SUnit sweep or package run while one runs.  The
  runners share files in `/tmp`.
