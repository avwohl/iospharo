-- vmperf — VM / test / benchmark results database.
--
-- Purpose: stop repeating test+bench runs that haven't changed.  A result is
-- keyed by (which VM build, which test source, which machine, which knobs),
-- so once Cog has run a given image's tests we never need to re-run them for
-- correctness — only timing is machine-dependent.
--
-- Apply:  ssh -p 24 wohl@awohl.com 'mysql' < scripts/perfdb/schema.sql
-- (run as root over the local socket; creates the DB if absent.)
--
-- Design notes
--  * Identity rows (machine / vm_build / test_source) are deduplicated by a
--    SHA-256 `fingerprint` UNIQUE key so re-registration is idempotent.
--  * `knobs` (env vars that change JIT behaviour/perf) live on the RUN, not the
--    build — the same binary produces different results under different knobs.
--  * Correctness (pass/fail) is machine-independent; timing is not.  Queries
--    that decide "skip unchanged Cog" therefore key correctness on
--    (source, vm_build) and timing on (source, vm_build, machine).

CREATE DATABASE IF NOT EXISTS vmperf
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE vmperf;

-- ---------------------------------------------------------------------------
-- machine — a host that ran a VM.  AWS spot instances reuse hostnames, so the
-- fingerprint folds in the CPU model + count.  `supports_perf_counters` records
-- whether this host exposes hardware CPU perf meters (some AWS instances don't).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS machine (
  id                     INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  fingerprint            CHAR(64)     NOT NULL,
  hostname               VARCHAR(255) NOT NULL,
  arch                   VARCHAR(32)  NOT NULL,            -- arm64 / x86_64
  cpu_model              VARCHAR(255) NULL,
  cpu_count              INT          NULL,
  mem_mb                 INT          NULL,
  os                     VARCHAR(128) NULL,                -- e.g. Darwin 25.5.0
  cloud                  VARCHAR(32)  NULL,                -- local / aws
  instance_type          VARCHAR(64)  NULL,                -- e.g. c7g.xlarge
  instance_id            VARCHAR(64)  NULL,                -- e.g. i-0abc...
  supports_perf_counters TINYINT(1)   NOT NULL DEFAULT 0,
  notes                  TEXT         NULL,
  created_at             TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_machine_fp (fingerprint),
  KEY ix_machine_host (hostname)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- vm_build — a specific VM binary + build config.  For our VM, git_sha is the
-- iospharo commit; for stock Cog it is the Cog/VMMaker version string (no git).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS vm_build (
  id           INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  fingerprint  CHAR(64)     NOT NULL,
  kind         ENUM('jit','cog','interp') NOT NULL,
  repo_url     VARCHAR(255) NULL,
  git_sha      CHAR(40)     NULL,                          -- our VM commit
  git_dirty    TINYINT(1)   NOT NULL DEFAULT 0,
  build_config VARCHAR(64)  NULL,                          -- Debug / RelWithDebInfo / Release
  arch         VARCHAR(32)  NULL,
  vm_version   VARCHAR(255) NULL,                          -- Cog version string, etc.
  built_at     DATETIME     NULL,
  notes        TEXT         NULL,
  created_at   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_vm_fp (fingerprint),
  KEY ix_vm_kind (kind),
  KEY ix_vm_sha (git_sha)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- test_source — where a set of tests/benchmarks comes from, AND the artifact
-- they run against.  `url`/`git_sha` answer "what URL did this test come from".
-- `image_sig` identifies the Pharo image the tests execute on (so the same
-- package at the same commit on two image builds are distinct sources).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS test_source (
  id            INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  fingerprint   CHAR(64)     NOT NULL,
  name          VARCHAR(128) NOT NULL,                     -- 'sunit-kernel', 'NeoJSON', 'bench-suite'
  kind          ENUM('sunit','bench','package') NOT NULL,
  url           VARCHAR(512) NULL,                         -- git URL the package came from
  git_sha       VARCHAR(64)  NULL,                         -- package commit
  pharo_version VARCHAR(64)  NULL,                         -- e.g. 13.0.x
  image_sig     VARCHAR(128) NULL,                         -- image identity (format ver / build hash)
  notes         TEXT         NULL,
  created_at    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_src_fp (fingerprint),
  KEY ix_src_name (name)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- run — one execution of a test_source on a vm_build on a machine, under a
-- specific set of knobs.  Holds process-level CPU + wall time (from
-- /usr/bin/time wrapping the VM) and the aggregate pass/fail counts.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS run (
  id                INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  vm_build_id       INT UNSIGNED NOT NULL,
  machine_id        INT UNSIGNED NOT NULL,
  source_id         INT UNSIGNED NOT NULL,
  kind              ENUM('sunit','bench') NOT NULL,
  knobs             VARCHAR(1024) NULL,                    -- env knobs, normalized "K=V K=V"
  knobs_hash        CHAR(64)      NOT NULL DEFAULT '',     -- sha256 of normalized knobs ('' => none)
  started_at        DATETIME     NULL,
  finished_at       DATETIME     NULL,
  proc_cpu_seconds  DOUBLE       NULL,                     -- user+sys, /usr/bin/time
  proc_user_seconds DOUBLE       NULL,
  proc_sys_seconds  DOUBLE       NULL,
  proc_real_seconds DOUBLE       NULL,                     -- wall
  max_rss_mb        DOUBLE       NULL,
  pass_count        INT          NOT NULL DEFAULT 0,
  fail_count        INT          NOT NULL DEFAULT 0,
  error_count       INT          NOT NULL DEFAULT 0,
  skip_count        INT          NOT NULL DEFAULT 0,
  timeout_count     INT          NOT NULL DEFAULT 0,
  status            ENUM('ok','aborted','crash','timeout','partial') NOT NULL DEFAULT 'ok',
  git_describe      VARCHAR(255) NULL,
  log_path          VARCHAR(512) NULL,
  notes             TEXT         NULL,
  created_at        TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY ix_run_lookup (source_id, vm_build_id, machine_id, knobs_hash),
  KEY ix_run_vm (vm_build_id),
  KEY ix_run_machine (machine_id),
  KEY ix_run_created (created_at),
  CONSTRAINT fk_run_vm     FOREIGN KEY (vm_build_id) REFERENCES vm_build(id),
  CONSTRAINT fk_run_machine FOREIGN KEY (machine_id) REFERENCES machine(id),
  CONSTRAINT fk_run_source FOREIGN KEY (source_id)  REFERENCES test_source(id)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- test_result — per-test outcome for a sunit run.  Granular so we can diff
-- exactly which tests changed without re-running the unchanged ones.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS test_result (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  run_id      INT UNSIGNED NOT NULL,
  class_name  VARCHAR(255) NOT NULL,
  selector    VARCHAR(255) NOT NULL,
  status      ENUM('pass','fail','error','skip','timeout') NOT NULL,
  cpu_ms      DOUBLE       NULL,
  real_ms     DOUBLE       NULL,
  detail      TEXT         NULL,
  KEY ix_tr_run (run_id),
  KEY ix_tr_test (class_name, selector),
  CONSTRAINT fk_tr_run FOREIGN KEY (run_id) REFERENCES run(id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- bench_result — per-benchmark timing for a bench run.  cpu_ms is the in-image
-- CPU time (the metric that varies least across machines / VM load).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bench_result (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  run_id      INT UNSIGNED NOT NULL,
  bench_name  VARCHAR(128) NOT NULL,
  cpu_ms      DOUBLE       NULL,                           -- in-image CPU ms (prim 247)
  real_ms     DOUBLE       NULL,
  score       VARCHAR(255) NULL,                           -- e.g. tinyBenchmarks string
  status      ENUM('ok','error') NOT NULL DEFAULT 'ok',
  detail      TEXT         NULL,
  KEY ix_br_run (run_id),
  KEY ix_br_name (bench_name),
  CONSTRAINT fk_br_run FOREIGN KEY (run_id) REFERENCES run(id) ON DELETE CASCADE
) ENGINE=InnoDB;
