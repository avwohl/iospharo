# AWS x64 build spot instance

Scripts to stand up a disposable AWS EC2 **spot** instance that builds and
develops the **x86_64 JIT** of the iospharo VM. The box works on `main` directly
(`BASE_BRANCH==WORK_BRANCH==main`). x86 and arm share one line: the `jit-x86`
branch went into `jit` on 2026-06-17 (commit `f317dd0b`), and `jit` went into
`main` on 2026-11-14. The `jit-x86` and `jit` branches do not exist now. Do
not push a branch with the name `jit`: the push makes the branch again.

Every x86 change is arm-neutral (gated
`#if x86` / `if(APPLE)` / behind a knob), so it can't clobber the arm process.
For an isolated experiment, set `WORK_BRANCH=<topic>` and merge back deliberately.

## One-time prerequisites (local machine)

- **A checkout of [`avwohl/aws_watch`](https://github.com/avwohl/aws_watch) next
  to this one**, at `~/src/aws_watch` (or point `AWS_WATCH_DIR` at it). The spot
  box lifecycle is not in this repo — see [Where the lifecycle
  lives](../../docs/aws-spot-box.md#where-the-lifecycle-lives).

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

## Tear down

    ./scripts/aws/teardown.sh              # instance + SG + IAM
    ./scripts/aws/teardown.sh --bucket     # also delete the S3 bucket
    ./scripts/aws/teardown.sh --deploy-key # also remove the GitHub deploy key
    ./scripts/aws/teardown.sh --instance-only

The GitHub deploy key, the S3 bucket and the EC2 key pair stay unless you
pass the option that removes them.

## More

The reference text is in [docs/aws-spot-box.md](../../docs/aws-spot-box.md):

- [What is on the box](../../docs/aws-spot-box.md#whats-on-the-box-the-whats-installed-record)
- [Safety and cost controls](../../docs/aws-spot-box.md#safety--cost-controls)
- [Keep-alive leases](../../docs/aws-spot-box.md#keep-alive-leases-no-more-reapskip)
- [Tear down: the deploy key](../../docs/aws-spot-box.md#tear-down-the-deploy-key)
- [Branch flow](../../docs/aws-spot-box.md#branch-flow)
- [Recovering an autosave](../../docs/aws-spot-box.md#recovering-an-autosave)
- [Where the lifecycle lives](../../docs/aws-spot-box.md#where-the-lifecycle-lives)
