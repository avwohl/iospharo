# AWS x64 build spot instance

Scripts to stand up a disposable AWS EC2 **spot** instance that builds and
develops the **x86_64 JIT** of the iospharo VM. The box works on `jit` directly
(`BASE_BRANCH==WORK_BRANCH==jit`): the x86 JIT was merged into `jit` on
2026-06-17 (commit `f317dd0b`) and the old `jit-x86` topic branch was deleted,
so x86 and arm now share one line. Every x86 change is arm-neutral (gated
`#if x86` / `if(APPLE)` / behind a knob), so it can't clobber the arm process.
For an isolated experiment, set `WORK_BRANCH=<topic>` and merge back deliberately.

## One-time prerequisites (local machine)

- **A checkout of [`avwohl/aws_watch`](https://github.com/avwohl/aws_watch) next
  to this one**, at `~/src/aws_watch` (or point `AWS_WATCH_DIR` at it). The spot
  box lifecycle is not in this repo — see [Where the lifecycle
  lives](#where-the-lifecycle-lives) below.

      git clone git@github.com:avwohl/aws_watch.git ~/src/aws_watch

- `~/.ssh/aws.txt` holds an AWS access key with enough rights (EC2, S3, and IAM
  for the instance role). `aws_watch/spot/load-creds.sh` parses it.
- `gh` authenticated as the repo owner (`repo` + `admin:public_key` scopes) so a
  scoped deploy key can be registered for the box.
- AWS CLI v2 (`brew install awscli`).

## Bring it up

    ./scripts/aws/provision.sh

This creates (idempotently): a private versioned S3 bucket, a scoped IAM
role/instance-profile, an SSH key pair (`~/.ssh/iospharo-x64.pem`), a security
group (SSH from your current public IP only), then launches an Ubuntu 24.04
spot instance (c6a.4xlarge = 16 vCPU / 32 GiB, with capacity fallbacks). It
waits for cloud-init, registers a GitHub deploy key, ships the on-box scripts,
enables the idle + spot-interruption units, and kicks off `clone-and-build.sh`.

Live resource IDs land in `scripts/aws/state.env`. Connection details print at
the end.

    ssh -i ~/.ssh/iospharo-x64.pem ubuntu@<ip>
    ssh -i ~/.ssh/iospharo-x64.pem ubuntu@<ip> 'tail -f clone-and-build.log'

## What's on the box (the "what's installed" record)

`bootstrap.sh` is the canonical install list: build-essential, cmake, ninja,
ccache, clang/lld/gdb/lldb, libffi-dev, libsdl2-dev, git, AWS CLI v2,
Node.js 20 + `@anthropic-ai/claude-code`. To rebuild a future spot box from
scratch, that one script is all you need.

## Safety / cost controls

- **Keep-alive lease** (`lease.sh`, `aws-lease-beat-hook.sh`): the central,
  authoritative defense — see [Keep-alive leases](#keep-alive-leases-no-more-reapskip)
  below. A box stays up only while an actively-working Claude heartbeats it; the
  `aws_watch` reaper on awohl.com terminates boxes whose lease has gone stale.
- **Idle auto-terminate** (`aws_watch/spot/idle-shutdown.sh`,
  `spot-idle.timer`): every 5 min it checks for SSH sessions, processes matching
  `IDLE_ACTIVE_PATTERN`, and load. After `IDLE_SECONDS` (default 1800 = 30 min)
  idle, it preserves state and terminates the instance. **`IDLE_ACTIVE_PATTERN`
  in `config.env` is this project's most dangerous setting** — a sweep that
  waits on the network looks idle. It cost a 74-minute run on 2026-08-11; the
  comment above it in `config.env` is the story.
- **Spot-interruption preservation** (`aws_watch/spot/spot-watch.sh`): watches
  the IMDS spot/instance-action endpoint; on the ~2-min reclaim notice it runs
  `preserve.sh` immediately.
- **`aws_watch/spot/preserve.sh`**: pushes the box's *committed* work to
  `$WORK_BRANCH` (default `jit`), and writes a crash dump of everything else to
  S3. Safe to run by hand anytime; it never moves the box's HEAD, index or
  working tree, so a box that is preserved but not actually reclaimed is left
  exactly as you left it. See [Recovering an autosave](#recovering-an-autosave).

## Keep-alive leases (no more `Reap=skip`)

Idle boxes used to get left running for days because we tagged them `Reap=skip`
to stop the CPU-idle reaper from false-killing low-CPU-but-active SUnit runs —
and that forever-tag then exempted them from *all* reaping, even after the work
was long done. That tag is **gone**. The replacement:

- A central `instance_lease` table on **awohl.com** lists every managed box.
- `provision.sh` **registers** the box at launch (within the reaper's 10-min
  grace window). Registration is a one-time "list it," not a heartbeat.
- The recurring **heartbeat** is sent **only by an actively-working Claude**, via
  Claude Code `PreToolUse` + `PostToolUse` hooks (`aws-lease-beat-hook.sh`). They
  fire on tool use, so the box stays up exactly while a Claude is running /
  looping / in a goal. `PreToolUse` also starts a short-lived in-flight re-beater
  so a *single* long, low-CPU tool call (e.g. a multi-minute SUnit run) keeps the
  lease fresh; it stops at `PostToolUse` or once no `claude` process remains. When
  that Claude finishes and walks away, the heartbeats stop, the lease goes stale
  (default 30 min), and `aws_watch` reaps the box — so a box left built-but-
  unattended is reaped too, which is intended: no Claude means nothing wants it.
  **Nothing else heartbeats** — not a cron, not a daemon, not `provision.sh`.
- `teardown.sh` **releases** the lease.
- The reaper still enforces a 12 h hard age cap as a runaway backstop (a stuck
  heartbeat can't keep a box forever).

A Claude running **on the box** heartbeats automatically (the hook + the
restricted key are installed by `provision.sh`; the box id comes from EC2 DMI, no
config needed). To keep a box alive from a Claude **driving it from this Mac**
instead, export the id that `provision.sh` prints:

    export AWS_LEASE_IID=i-…  AWS_LEASE_PROJECT=iospharo-x64  AWS_LEASE_REGION=us-east-2

### One-time key setup (per machine that registers/heartbeats)

The lease writers use a locked-down SSH key whose only power on awohl.com is to
touch lease rows (a forced command — it can't open a shell or run anything else):

    ssh-keygen -t ed25519 -N "" -f ~/.ssh/aws-lease -C aws-lease-beat
    # then add it on awohl.com (~wohl/.ssh/authorized_keys), restricted:
    #   command="/usr/bin/python3 /home/wohl/src/aws_watch/lease_cmd.py",restrict <pubkey>

See the `aws_watch` repo's README ("Keep-alive leases") for the server side
(schema, the `lease_cmd.py` forced command, and the reaper config). Inspect or
poke leases by hand with `./scripts/aws/lease.sh list` (or `fresh`, `register`,
`release`).

## Tear down

    ./scripts/aws/teardown.sh              # instance + SG + IAM
    ./scripts/aws/teardown.sh --bucket     # also delete the S3 bucket
    ./scripts/aws/teardown.sh --deploy-key # also remove the GitHub deploy key
    ./scripts/aws/teardown.sh --instance-only

The GitHub deploy key is **kept by default**, like the S3 bucket and the EC2 key
pair. `provision.sh` caches it — it reuses `~/.ssh/<KEY_NAME>-deploy` and skips
the GitHub re-add when an identical key is already registered. Teardown used to
delete it every time, which defeated that cache: the next provision re-added it
and GitHub mailed the repo admin a "deploy key added" notification, once per
cycle. Pass `--deploy-key` when you actually want it gone (retiring the project,
or rotating the key). The private half never leaves `~/.ssh` on this machine.

## Branch flow

The box pushes its **committed** work to `jit` directly (`preserve.sh` →
`origin/$WORK_BRANCH`, default `jit`), so there's normally no separate merge
step — just `git pull` locally. That push is an ordinary one: if the box is
behind, the server refuses it as a non-fast-forward, which is the right answer.

What does *not* go to git is the **autosave** — the `git add -A` snapshot
preserve.sh takes of whatever the working tree happens to hold. That goes to S3;
see below.

If you run an isolated experiment with `WORK_BRANCH=<topic>`, fold it back with:

    git fetch origin && git checkout jit && git pull
    git merge origin/<topic>      # resolve, test
    git push origin jit
    git push origin --delete <topic>   # clean up

## Recovering an autosave

When a box is reclaimed or idle-terminated, `preserve.sh` writes a crash dump to

    s3://iospharo-build-670060058357/autosave/<instance-id>/<timestamp>/

containing a git bundle of everything the box held that `origin` did not, the
uncommitted work as a plain patch, `git status`, and a manifest. Recover one
with the shared tool, pointed at this project's config:

    cd ~/src/iospharo-jit
    CONFIG_FILE=scripts/aws/config.env ~/src/aws_watch/spot/restore-autosave.sh list
    CONFIG_FILE=scripts/aws/config.env ~/src/aws_watch/spot/restore-autosave.sh show  <instance>/<ts>
    CONFIG_FILE=scripts/aws/config.env ~/src/aws_watch/spot/restore-autosave.sh fetch <instance>/<ts>

`fetch` lands the dump at `refs/autosave/<instance>/<ts>` and stops — no branch
moves, no file is touched. Then `git log` / `git diff jit..<ref>` /
`git cherry-pick`, and `git update-ref -d <ref>` when you're done.

The full runbook, including what to do when a bundle's prerequisites are
missing, is [`aws_watch/spot/README.md`](https://github.com/avwohl/aws_watch/blob/main/spot/README.md).

### Why the autosave is not in git

Two autosaves have landed on shared `jit` and done damage — `27d378d2`
(2026-06-03, reverted the asmjit Catalyst pin, 68 days broken) and `ede0fd65`
(2026-09-03, a 242-commit-stale tree that deleted `scripts/sweep-3way.sh`). The
cause is structural: the snapshot's *tree* and its *parent* are independent
inputs, so a current parent with a stale tree records the difference as
deletions and pushes as a clean fast-forward nothing can refuse.

A per-box `autosave/<instance-id>` branch fixes that much. S3 is better for a
second reason: a git object is forever. For scale, in this repo the two
autosaves that reached `jit` cost 540 KiB of permanent pack — small next to the
20 MiB of `build*/` binaries that were committed and later untracked, which
reclaimed nothing, but neither is recoverable without rewriting public history.
An S3 object can be deleted, or expired by a lifecycle rule.

## Where the lifecycle lives

`preserve.sh`, `spot-watch.sh`, `idle-shutdown.sh`, `install-box.sh`,
`restore-autosave.sh` and `load-creds.sh` are **not in this repo**. They are
project-independent and live in [`avwohl/aws_watch`](https://github.com/avwohl/aws_watch)
under `spot/`, beside the reaper and the `instance_lease` table these same boxes
register with. `aws-lease-beat-hook.sh` used to exist byte-identically in both
repos; now there is one copy.

    ~/src/aws_watch/spot/     the lifecycle, and its tests
    scripts/aws/              this project: what to build, and how to configure it

`config.env` is the seam. It sets `AWS_WATCH_DIR`/`SPOT_DIR`, the on-box layout
(`SPOT_PREFIX`, `SPOT_USER`, `SPOT_REPO`), the autosave commit identity, and
`IDLE_ACTIVE_PATTERN`. `provision.sh` ships `$SPOT_DIR` to the box and runs
`spot/install-box.sh`, which installs the scripts and writes and enables the
systemd units — so the on-box layout is defined once, for every project.

Run the lifecycle's own tests from there:

    ~/src/aws_watch/spot/test-preserve.sh

Ten cases against a throwaway repo and a stub `aws`; it never contacts AWS or
GitHub. Case 3 is the `ede0fd65` clobber reproduced deliberately.
