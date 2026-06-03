# AWS x64 build spot instance

Scripts to stand up a disposable AWS EC2 **spot** instance that builds and
develops the **x86_64 JIT** port of the iospharo VM. The arm work continues in
parallel on the `jit` branch; the x86 work lives on `jit-x86` and is merged
back deliberately (so it never clobbers the arm process).

## One-time prerequisites (local machine)

- `~/.ssh/aws.txt` holds an AWS access key with enough rights (EC2, S3, and IAM
  for the instance role). `load-creds.sh` parses it.
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

- **Idle auto-terminate** (`idle-shutdown.sh`, `iospharo-idle.timer`): every
  5 min it checks for SSH sessions, build/dev processes, and load. After
  `IDLE_SECONDS` (default 1800 = 30 min) idle, it preserves state and
  terminates the instance.
- **Spot-interruption preservation** (`spot-watch.sh`): watches the IMDS
  spot/instance-action endpoint; on the ~2-min reclaim notice it runs
  `preserve.sh` immediately.
- **`preserve.sh`**: commits WIP, pushes `jit-x86` to GitHub (durable), and
  syncs notes/logs to S3. Safe to run by hand anytime.

## Tear down

    ./scripts/aws/teardown.sh            # instance + SG + IAM + deploy key
    ./scripts/aws/teardown.sh --bucket   # also delete the S3 bucket
    ./scripts/aws/teardown.sh --instance-only

## Merging the x86 work back

`jit-x86` is pushed from the box. To fold it into `jit`, locally:

    git fetch origin && git checkout jit && git pull
    git merge origin/jit-x86      # resolve, test
    git push origin jit
