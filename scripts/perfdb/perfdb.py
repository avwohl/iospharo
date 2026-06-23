#!/usr/bin/env python3
"""perfdb — record VM test/benchmark results into the `vmperf` MySQL DB.

Goal: stop re-running tests that haven't changed.  Every result is keyed by
(vm_build, test_source, machine, knobs); correctness is machine-independent so
once a VM build has run a test source we never re-run it for pass/fail, only for
timing on a new machine.

Connection: pipes SQL to a remote MySQL over ssh — no local MySQL driver needed,
so this works identically from the laptop and from AWS spot instances.  Override
with $PERFDB_SSH (default "ssh -p 24 wohl@awohl.com") and $PERFDB_DB (default
"vmperf").  Set $PERFDB_SSH="" to talk to a local mysql directly.

Subcommands:
  machine-info                      print detected machine identity (JSON)
  register-machine                  upsert this machine, print its id
  register-vm     --kind ...        upsert a vm_build, print its id
  register-source --name ... --kind upsert a test_source, print its id
  record          --kind ...        insert a run (+ per-test/bench rows) from a
                                    result file, print the run id
  run-and-record  ... -- CMD...     run CMD (measuring CPU+wall), then record
  have-correctness --source --vm    print a prior run id if correctness already
                                    known for (source, vm[, knobs]); else nothing
  query "SELECT ..."                run a read-only query, print TSV
  report-bench    [--source NAME]   latest JIT-vs-Cog per-benchmark comparison
"""

import argparse
import hashlib
import json
import os
import platform
import resource
import shlex
import subprocess
import sys
import time
from datetime import datetime

DB = os.environ.get("PERFDB_DB", "vmperf")
SSH = os.environ.get("PERFDB_SSH", "ssh -p 24 wohl@awohl.com")


# --------------------------------------------------------------------------- #
# DB plumbing — pipe SQL to `<ssh> mysql <db>` (one connection per call).
# --------------------------------------------------------------------------- #
def _mysql_argv():
    base = shlex.split(SSH) if SSH else []
    return base + ["mysql", "--default-character-set=utf8mb4", DB]


def run_sql(script):
    """Execute a SQL script (may contain @vars / multiple statements). Return stdout."""
    p = subprocess.run(_mysql_argv(), input=script, capture_output=True,
                       text=True, timeout=120)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit(f"perfdb: mysql failed (rc={p.returncode})")
    return p.stdout


def query(sql):
    """Run a SELECT; return list[dict] (mysql batch/tab output, NULL -> None)."""
    out = run_sql(sql)
    lines = out.rstrip("\n").split("\n") if out.strip() else []
    if not lines:
        return []
    header = lines[0].split("\t")
    rows = []
    for ln in lines[1:]:
        cells = ln.split("\t")
        rows.append({h: (None if c == "NULL" else c) for h, c in zip(header, cells)})
    return rows


def sql(v):
    """Render a Python value as a SQL literal."""
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, (int, float)):
        return repr(v)
    s = str(v).replace("\\", "\\\\").replace("'", "\\'")
    return "'" + s + "'"


def fingerprint(*parts):
    canon = "|".join("" if p is None else str(p) for p in parts)
    return hashlib.sha256(canon.encode("utf-8")).hexdigest()


def upsert(table, fp, cols):
    """INSERT ... ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id); return row id.

    `cols` is a dict of column->value; `fp` is the fingerprint (its column must be
    the table's UNIQUE key `fingerprint`)."""
    cols = dict(cols)
    cols["fingerprint"] = fp
    names = ", ".join(cols)
    vals = ", ".join(sql(v) for v in cols.values())
    script = (
        f"INSERT INTO {table} ({names}) VALUES ({vals}) "
        f"ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id);\n"
        f"SELECT LAST_INSERT_ID() AS id;\n"
    )
    rows = query(script)
    return int(rows[0]["id"])


# --------------------------------------------------------------------------- #
# Machine detection
# --------------------------------------------------------------------------- #
def _cmd(args):
    try:
        return subprocess.run(args, capture_output=True, text=True,
                              timeout=5).stdout.strip()
    except Exception:
        return ""


def detect_cloud():
    """Probe AWS IMDS (very short timeout); return (cloud, instance_type, instance_id)."""
    import urllib.request
    try:
        req = urllib.request.Request(
            "http://169.254.169.254/latest/api/token", method="PUT",
            headers={"X-aws-ec2-metadata-token-ttl-seconds": "60"})
        token = urllib.request.urlopen(req, timeout=0.4).read().decode()
        hdr = {"X-aws-ec2-metadata-token": token}

        def meta(path):
            r = urllib.request.Request(
                "http://169.254.169.254/latest/meta-data/" + path, headers=hdr)
            return urllib.request.urlopen(r, timeout=0.4).read().decode()
        return "aws", meta("instance-type"), meta("instance-id")
    except Exception:
        return "local", None, None


def detect_perf_counters():
    if sys.platform == "darwin":
        return True  # getrusage child CPU time is always available
    try:
        return any(n.startswith("cpu")
                   for n in os.listdir("/sys/bus/event_source/devices"))
    except OSError:
        return False


def machine_info():
    arch = platform.machine()
    osname = f"{platform.system()} {platform.release()}"
    if sys.platform == "darwin":
        cpu_model = _cmd(["sysctl", "-n", "machdep.cpu.brand_string"]) or None
        memb = _cmd(["sysctl", "-n", "hw.memsize"])
        mem_mb = int(memb) // (1024 * 1024) if memb.isdigit() else None
    else:
        cpu_model = None
        try:
            with open("/proc/cpuinfo") as f:
                for ln in f:
                    if ln.startswith("model name") or ln.startswith("Model name"):
                        cpu_model = ln.split(":", 1)[1].strip()
                        break
        except OSError:
            pass
        if not cpu_model:
            cpu_model = _cmd(["lscpu"]) and next(
                (l.split(":", 1)[1].strip() for l in _cmd(["lscpu"]).splitlines()
                 if l.startswith("Model name")), None)
        mem_mb = None
        try:
            with open("/proc/meminfo") as f:
                for ln in f:
                    if ln.startswith("MemTotal"):
                        mem_mb = int(ln.split()[1]) // 1024
                        break
        except OSError:
            pass
    cloud, itype, iid = detect_cloud()
    info = {
        "hostname": platform.node(),
        "arch": arch,
        "cpu_model": cpu_model,
        "cpu_count": os.cpu_count(),
        "mem_mb": mem_mb,
        "os": osname,
        "cloud": cloud,
        "instance_type": itype,
        "instance_id": iid,
        "supports_perf_counters": detect_perf_counters(),
    }
    info["fingerprint"] = fingerprint(
        info["hostname"], arch, cpu_model, info["cpu_count"], itype)
    return info


def register_machine():
    info = machine_info()
    fp = info.pop("fingerprint")
    return upsert("machine", fp, info)


# --------------------------------------------------------------------------- #
# Result-file parsers
# --------------------------------------------------------------------------- #
def parse_bench(path):
    """bench-suite result file -> list of dict(bench_name, cpu_ms, real_ms, score, status, detail).

    Recognized value forms after 'name = ':
      '<cpu> ms cpu, <wall> ms wall'   -> cpu_ms, real_ms
      '<n> ms'                          -> legacy: cpu_ms only
      'ERROR: <msg>'                    -> status=error
    """
    import re
    both = re.compile(r"^([0-9.]+) ms cpu, ([0-9.]+) ms wall$")
    out = []
    complete = False
    with open(path) as f:
        for raw in f:
            ln = raw.rstrip("\n")
            if ln.strip() == "DONE":
                complete = True
                continue
            if ln.startswith("tinyBenchmarks:"):
                val = ln.split(":", 1)[1].strip()
                if val.startswith("ERROR"):
                    out.append(dict(bench_name="tinyBenchmarks", cpu_ms=None,
                                    real_ms=None, score=None, status="error",
                                    detail=val[:500]))
                else:
                    out.append(dict(bench_name="tinyBenchmarks", cpu_ms=None,
                                    real_ms=None, score=val[:255], status="ok",
                                    detail=None))
                continue
            if " = " in ln:
                name, val = ln.split(" = ", 1)
                name, val = name.strip(), val.strip()
                if val.startswith("ERROR"):
                    out.append(dict(bench_name=name, cpu_ms=None, real_ms=None,
                                    score=None, status="error", detail=val[:500]))
                    continue
                m = both.match(val)
                if m:
                    out.append(dict(bench_name=name, cpu_ms=float(m.group(1)),
                                    real_ms=float(m.group(2)), score=None,
                                    status="ok", detail=None))
                elif val.endswith(" ms"):
                    try:
                        ms = float(val[:-3].strip())
                    except ValueError:
                        continue
                    out.append(dict(bench_name=name, cpu_ms=ms, real_ms=None,
                                    score=None, status="ok", detail=None))
    return out, complete


def parse_sunit_tsv(path):
    """Our runner's detail file: TSV `runNum<TAB>Class<TAB>selector<TAB>STATUS`,
    one row per (test, run). Keep the latest run's verdict per (class, selector)."""
    latest = {}   # (class, selector) -> (runNum, status)
    with open(path) as f:
        for raw in f:
            parts = raw.rstrip("\n").split("\t")
            if len(parts) != 4:
                continue
            run_s, cls, sel, status = parts
            try:
                rn = int(run_s)
            except ValueError:
                continue
            st = status.strip().lower()
            if st not in ("pass", "fail", "error", "skip", "timeout"):
                continue
            key = (cls, sel)
            if key not in latest or rn >= latest[key][0]:
                latest[key] = (rn, st)
    return [dict(class_name=c, selector=s, status=st)
            for (c, s), (_, st) in latest.items()]


def parse_sunit_any(path):
    """Parse either runner's per-test format; return whichever yields tests."""
    block = parse_sunit_detail(path)
    tsv = parse_sunit_tsv(path)
    return block if len(block) >= len(tsv) else tsv


def parse_sunit_detail(path):
    """detail file -> list of dict(class_name, selector, status). Mirrors classify-sunit.py."""
    import re
    CLASS_RE = re.compile(r"^=== (\w+) ===$")
    RUN_RE = re.compile(r"^  RUN: (\S+)")
    RESULT_RE = re.compile(r"^  (FAIL|ERROR|SKIP|TIMEOUT): (\S+)")
    results = {}
    cur_class = None
    pending = None
    with open(path) as f:
        for raw in f:
            ln = raw.rstrip("\n")
            m = CLASS_RE.match(ln)
            if m:
                if cur_class and pending:
                    results[(cur_class, pending)] = "pass"
                cur_class = m.group(1)
                pending = None
                continue
            m = RUN_RE.match(ln)
            if m and cur_class:
                if pending:
                    results[(cur_class, pending)] = "pass"
                pending = m.group(1)
                continue
            m = RESULT_RE.match(ln)
            if m and cur_class and pending:
                results[(cur_class, pending)] = m.group(1).lower()
                pending = None
    if cur_class and pending:
        results[(cur_class, pending)] = "pass"
    return [dict(class_name=c, selector=s, status=st)
            for (c, s), st in results.items()]


# --------------------------------------------------------------------------- #
# Recording
# --------------------------------------------------------------------------- #
def norm_knobs(knobs):
    """Normalize a knobs string ('A=1 B=2') so the hash is order-independent."""
    if not knobs:
        return "", ""
    toks = sorted(t for t in knobs.replace(",", " ").split() if t)
    canon = " ".join(toks)
    return canon, (fingerprint(canon) if canon else "")


def record_run(kind, vm_id, machine_id, source_id, knobs, timing, results,
               status="ok", log_path=None, started=None, finished=None, notes=None):
    canon_knobs, knobs_hash = norm_knobs(knobs)
    if kind == "bench":
        rows, complete = results
        counts = dict(pass_count=sum(1 for r in rows if r["status"] == "ok"),
                      error_count=sum(1 for r in rows if r["status"] == "error"))
        if not complete and status == "ok":
            status = "partial"
    else:
        rows = results
        cnt = {"pass": 0, "fail": 0, "error": 0, "skip": 0, "timeout": 0}
        for r in rows:
            cnt[r["status"]] = cnt.get(r["status"], 0) + 1
        counts = dict(pass_count=cnt["pass"], fail_count=cnt["fail"],
                      error_count=cnt["error"], skip_count=cnt["skip"],
                      timeout_count=cnt["timeout"])

    run_cols = dict(
        vm_build_id=vm_id, machine_id=machine_id, source_id=source_id, kind=kind,
        knobs=(canon_knobs or None), knobs_hash=knobs_hash,
        started_at=started, finished_at=finished,
        proc_cpu_seconds=timing.get("cpu"), proc_user_seconds=timing.get("user"),
        proc_sys_seconds=timing.get("sys"), proc_real_seconds=timing.get("real"),
        max_rss_mb=timing.get("maxrss_mb"), status=status,
        git_describe=timing.get("git_describe"), log_path=log_path, notes=notes,
        **counts)
    names = ", ".join(run_cols)
    vals = ", ".join(sql(v) for v in run_cols.values())
    script = [f"INSERT INTO run ({names}) VALUES ({vals});",
              "SET @r = LAST_INSERT_ID();"]

    if kind == "bench":
        for r in rows:
            script.append(
                "INSERT INTO bench_result (run_id, bench_name, cpu_ms, real_ms, score, status, detail) "
                f"VALUES (@r, {sql(r['bench_name'])}, {sql(r['cpu_ms'])}, "
                f"{sql(r.get('real_ms'))}, {sql(r['score'])}, {sql(r['status'])}, "
                f"{sql(r['detail'])});")
    else:
        # batch per-test rows into multi-row INSERTs (chunks of 200)
        for i in range(0, len(rows), 200):
            chunk = rows[i:i + 200]
            tup = ", ".join(
                f"(@r, {sql(r['class_name'])}, {sql(r['selector'])}, {sql(r['status'])})"
                for r in chunk)
            script.append(
                "INSERT INTO test_result (run_id, class_name, selector, status) "
                f"VALUES {tup};")
    script.append("SELECT @r AS id;")
    rows_out = query("\n".join(script))
    return int(rows_out[0]["id"])


# --------------------------------------------------------------------------- #
# Timing
# --------------------------------------------------------------------------- #
def run_command(cmd, timeout=None):
    """Run cmd; return (status, timing dict with cpu/user/sys/real/maxrss_mb, started, finished)."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    t0 = time.monotonic()
    status = "ok"
    # The VM is very chatty; its results go to a file, so drop its stdout/stderr
    # to keep them out of this process's stdout (which carries the run id).
    try:
        subprocess.run(cmd, timeout=timeout,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        status = "timeout"
    real = time.monotonic() - t0
    finished = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    user = after.ru_utime - before.ru_utime
    sysc = after.ru_stime - before.ru_stime
    rss = after.ru_maxrss
    maxrss_mb = rss / (1024 * 1024) if sys.platform == "darwin" else rss / 1024
    return status, dict(cpu=user + sysc, user=user, sys=sysc, real=real,
                        maxrss_mb=round(maxrss_mb, 1)), started, finished


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(prog="perfdb")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("machine-info")
    sub.add_parser("register-machine")

    p = sub.add_parser("register-vm")
    p.add_argument("--kind", required=True, choices=["jit", "cog", "interp"])
    p.add_argument("--git-sha")
    p.add_argument("--binary", help="VM binary path; its sha256 is the true identity "
                                    "(git HEAD churns on unrelated commits)")
    p.add_argument("--build-config")
    p.add_argument("--arch", default=platform.machine())
    p.add_argument("--vm-version")
    p.add_argument("--built-at")
    p.add_argument("--repo-url")
    p.add_argument("--dirty", action="store_true")

    p = sub.add_parser("register-source")
    p.add_argument("--name", required=True)
    p.add_argument("--kind", required=True, choices=["sunit", "bench", "package"])
    p.add_argument("--url")
    p.add_argument("--git-sha")
    p.add_argument("--pharo-version")
    p.add_argument("--image-sig")

    for name in ("record", "run-and-record"):
        p = sub.add_parser(name)
        p.add_argument("--kind", required=True, choices=["sunit", "bench"])
        p.add_argument("--vm", required=True, type=int)
        p.add_argument("--machine", required=True, type=int)
        p.add_argument("--source", required=True, type=int)
        p.add_argument("--knobs", default="")
        p.add_argument("--result", help="bench result file, or sunit detail file")
        p.add_argument("--detail", help="sunit detail file (if --result is the summary)")
        p.add_argument("--log")
        p.add_argument("--git-describe")
        if name == "record":
            p.add_argument("--cpu", type=float)
            p.add_argument("--user", type=float)
            p.add_argument("--sys", type=float)
            p.add_argument("--real", type=float)
            p.add_argument("--maxrss-mb", type=float)
            p.add_argument("--status", default="ok")
            p.add_argument("--started")
            p.add_argument("--finished")
        else:
            p.add_argument("--timeout", type=float)
            p.add_argument("--shell", required=True,
                           help="shell command to run (executed via sh -c)")

    p = sub.add_parser("have-correctness")
    p.add_argument("--source", required=True, type=int)
    p.add_argument("--vm", required=True, type=int)
    p.add_argument("--knobs", default="")

    p = sub.add_parser("query")
    p.add_argument("sql")

    p = sub.add_parser("report-bench")
    p.add_argument("--source")

    p = sub.add_parser("report-sunit")
    p.add_argument("--source", required=True, help="test_source name, e.g. sunit-kernel")
    p.add_argument("--limit", type=int, default=40, help="max divergent tests to list")

    args = ap.parse_args()

    if args.cmd == "machine-info":
        print(json.dumps(machine_info(), indent=2))
    elif args.cmd == "register-machine":
        print(register_machine())
    elif args.cmd == "register-vm":
        bin_sha = None
        if args.binary and os.path.exists(args.binary):
            h = hashlib.sha256()
            with open(args.binary, "rb") as bf:
                for chunk in iter(lambda: bf.read(1 << 20), b""):
                    h.update(chunk)
            bin_sha = h.hexdigest()
        # Fingerprint on the binary content when available (git HEAD churns on
        # unrelated script/doc commits); fall back to git_sha/vm_version.
        ident = bin_sha or args.git_sha or args.vm_version
        fp = fingerprint(args.kind, ident, args.build_config, args.arch)
        print(upsert("vm_build", fp, dict(
            kind=args.kind, git_sha=args.git_sha, binary_sha256=bin_sha,
            git_dirty=args.dirty, build_config=args.build_config, arch=args.arch,
            vm_version=args.vm_version, built_at=args.built_at,
            repo_url=args.repo_url)))
    elif args.cmd == "register-source":
        fp = fingerprint(args.name, args.kind, args.url, args.git_sha,
                         args.pharo_version, args.image_sig)
        print(upsert("test_source", fp, dict(
            name=args.name, kind=args.kind, url=args.url, git_sha=args.git_sha,
            pharo_version=args.pharo_version, image_sig=args.image_sig)))
    elif args.cmd in ("record", "run-and-record"):
        do_record(args)
    elif args.cmd == "have-correctness":
        _, kh = norm_knobs(args.knobs)
        rows = query(
            "SELECT id, status, pass_count, fail_count, error_count, created_at "
            f"FROM run WHERE source_id={args.source} AND vm_build_id={args.vm} "
            f"AND knobs_hash={sql(kh)} AND status<>'crash' "
            "ORDER BY created_at DESC LIMIT 1;")
        if rows:
            r = rows[0]
            print(f"{r['id']}\t{r['status']}\tP{r['pass_count']} "
                  f"F{r['fail_count']} E{r['error_count']}\t{r['created_at']}")
    elif args.cmd == "query":
        sys.stdout.write(run_sql(args.sql))
    elif args.cmd == "report-bench":
        report_bench(args.source)
    elif args.cmd == "report-sunit":
        report_sunit(args.source, args.limit)


def do_record(args):
    if args.cmd == "run-and-record":
        status, timing, started, finished = run_command(
            ["sh", "-c", args.shell], args.timeout)
    else:
        status = args.status
        timing = dict(cpu=args.cpu, user=args.user, sys=args.sys, real=args.real,
                      maxrss_mb=args.maxrss_mb)
        started, finished = args.started, args.finished
    timing["git_describe"] = args.git_describe

    if args.kind == "bench":
        results = parse_bench(args.result)
    else:
        # Runners disagree on which file holds the per-test blocks: our runner
        # writes them to the detail file, stock Cog's to the results file. Parse
        # each candidate and keep the one that actually has tests.
        results = []
        for cand in (args.detail, args.result):
            if cand and os.path.exists(cand):
                parsed = parse_sunit_any(cand)
                if len(parsed) > len(results):
                    results = parsed

    run_id = record_run(args.kind, args.vm, args.machine, args.source, args.knobs,
                        timing, results, status=status, log_path=args.log,
                        started=started, finished=finished)
    print(run_id)


def report_bench(source_name):
    where = ""
    if source_name:
        where = f"AND s.name = {sql(source_name)}"
    # latest bench run per (vm kind, source); compare cpu_ms by bench_name
    rows = query(f"""
SELECT v.kind AS kind, s.name AS src, b.bench_name AS bench,
       b.cpu_ms AS cpu_ms, b.real_ms AS real_ms, b.status AS st
FROM run r
JOIN vm_build v   ON v.id = r.vm_build_id
JOIN test_source s ON s.id = r.source_id
JOIN bench_result b ON b.run_id = r.id
WHERE r.kind='bench' {where}
  AND r.id IN (SELECT MAX(id) FROM run WHERE kind='bench' GROUP BY vm_build_id, source_id)
ORDER BY s.name, b.bench_name, v.kind;
""")

    def num(v):
        return float(v) if v not in (None, "") else None

    data = {}
    for r in rows:
        d = data.setdefault((r["src"], r["bench"]), {})
        d[r["kind"]] = dict(cpu=num(r["cpu_ms"]), wall=num(r["real_ms"]), st=r["st"])
    # ratio = jit_wall / cog_wall (wall works on both VMs; >1 means JIT slower)
    print(f"{'benchmark':<22} {'cog_wall':>9} {'jit_wall':>9} {'ratio':>7} {'jit_cpu':>8}")
    worst = []
    for (src, bench), d in sorted(data.items()):
        cog = d.get("cog", {})
        jit = d.get("jit", {})
        cw, jw, jc = cog.get("wall"), jit.get("wall"), jit.get("cpu")
        ratio = jw / cw if (cw and jw and cw > 0) else None
        rs = f"{ratio:.2f}x" if ratio else ""
        if ratio:
            worst.append((ratio, bench, cw, jw))
        print(f"{bench:<22} {('%.0f'%cw) if cw else '-':>9} "
              f"{('%.0f'%jw) if jw else '-':>9} {rs:>7} "
              f"{('%.0f'%jc) if jc is not None else '-':>8}")
    if worst:
        print("\nslowest JIT-vs-Cog (by wall ratio):")
        for ratio, bench, cw, jw in sorted(worst, reverse=True)[:8]:
            print(f"  {ratio:5.2f}x  {bench:<22} cog={cw:.0f}ms jit={jw:.0f}ms")


def report_sunit(source_name, limit):
    """Compare the latest JIT run's per-test verdicts vs the latest Cog run's,
    for a source. Surfaces JIT regressions without re-running Cog."""
    def latest_run(kind):
        rows = query(
            "SELECT r.id FROM run r JOIN vm_build v ON v.id=r.vm_build_id "
            "JOIN test_source s ON s.id=r.source_id "
            f"WHERE r.kind='sunit' AND v.kind='{kind}' AND s.name={sql(source_name)} "
            "ORDER BY r.id DESC LIMIT 1;")
        return int(rows[0]["id"]) if rows else None

    cog_run, jit_run = latest_run("cog"), latest_run("jit")
    if not cog_run or not jit_run:
        print(f"need both a cog and jit sunit run for '{source_name}' "
              f"(cog={cog_run}, jit={jit_run})")
        return

    def verdicts(run_id):
        return {(r["class_name"], r["selector"]): r["status"]
                for r in query("SELECT class_name, selector, status "
                               f"FROM test_result WHERE run_id={run_id};")}

    cog, jit = verdicts(cog_run), verdicts(jit_run)
    keys = set(cog) | set(jit)
    OKS = {"pass", "skip"}
    agree = jit_worse = jit_better = cog_only = jit_only = 0
    regressions = []
    for k in keys:
        c, j = cog.get(k), jit.get(k)
        if c is None:
            jit_only += 1
        elif j is None:
            cog_only += 1
        elif c == j:
            agree += 1
        elif c in OKS and j not in OKS:
            jit_worse += 1
            regressions.append((k[0], k[1], c, j))
        elif c not in OKS and j in OKS:
            jit_better += 1
        else:
            agree += 1  # both non-ok, different flavor — not a regression
    print(f"source '{source_name}'  cog_run={cog_run} jit_run={jit_run}")
    print(f"  common tests: {len(keys)}   agree: {agree}")
    print(f"  JIT REGRESSIONS (cog ok, jit not): {jit_worse}")
    print(f"  jit recovered (cog not ok, jit ok): {jit_better}")
    print(f"  cog-only: {cog_only}   jit-only: {jit_only}")
    if regressions:
        print(f"\n  regressions (up to {limit}):")
        for cls, sel, c, j in sorted(regressions)[:limit]:
            print(f"    {cls}>>{sel}   cog={c} jit={j}")


if __name__ == "__main__":
    main()
