#!/usr/bin/env python3
r"""GPU contention on the shared self-hosted runner, made impossible to mistake
for a code defect (#875).

The sole self-hosted GPU runner is also the maintainer's workstation. That is an
**accepted constraint**, not a bug to fix: there is one machine, and it will keep
being both. What is not acceptable is the consequence -- a job that runs while
unrelated GPU work is on the box produces timing numbers that read exactly like a
renderer regression, and each one costs a diagnosis cycle.

It has already happened twice in one day:

* **PR #881** failed the streaming tier-budget gate on wall-clock alone
  (``first_visible_ms=3500``, ``frame_p95_to_avg_ratio=1.935``) while residency
  reached ``1.0``, fallback rate ``0.0`` and readiness ``READY``. Nothing failed
  functionally. This repository already recorded (#630/#624) that a clean run on
  that lane sits near ``p95/avg 1.15``; **1.94 means the gate measured the
  machine.**
* **#867**'s ``NodeSceneTree`` 300 s timeout was the same thing, and cost a full
  diagnosis before the load-normalisation showed every batch in the job slowed by
  the same ~2.9x factor.

On the day #875 was written, three unrelated Godot workloads ran on this box:
4x ``Godot_v4.7-stable`` gdUnit4 at 11:13, the same again around 13:00, and 2x
``Godot_v4.5.2-stable`` at 22:28. None were ours. Each contends for the GPU.

What this module does
---------------------
Four things, in the order they matter:

1. **Wait, do not fail immediately.** Failing the instant the GPU is busy would
   let the owner's own work block CI entirely, on a machine that is *also* their
   workstation. So the preflight polls for the GPU to become free, up to a bounded
   wait (:data:`DEFAULT_WAIT_TIMEOUT_SEC`, justified there).
2. **If it never frees, fail unmistakably.** Exit :data:`EXIT_RUNNER_BUSY`, a
   dedicated code that is not the test runner's, behind a banner that says
   ``RUNNER BUSY -- THIS RESULT IS VOID``, naming every process holding the GPU
   with pid, image path and measured GPU share. A reader cannot mistake it for a
   renderer regression, because no renderer ran.
3. **Never silently pass.** Every phase writes a record and prints a verdict. An
   environment this cannot measure is :data:`VERDICT_UNMEASURED`, which is void --
   never green. Absence of a signal is not a passing signal.
4. **Measure at start *and* end, and in between.** #881's interference began
   *after* the job started; a check that only looks at job start would have called
   that run clean. So the preflight leaves a detached sampler running for the life
   of the job and the postflight reads back the whole series, so a timing failure
   arrives with "the machine was contended from 18:33 to 18:51" attached instead
   of being investigated from scratch.

What it deliberately does **not** do
------------------------------------
It does not move, raise or soften a single budget, timeout or threshold. A
contended run is **void**, never *passed* and never *tolerated*: the verdict says
the measurement is worthless, it does not say the measurement was fine after all.
Whether wall-clock budgets belong in correctness lanes at all is #523/#778 and a
much larger change; this module only makes the attribution unambiguous.

How "busy" is decided
---------------------
Two independent sources, because either alone can be wrong in a way that matters:

* **Per-process attribution** -- the Windows ``\GPU Engine(*)\Utilization
  Percentage`` performance counters, whose instance names carry the owning pid
  (``pid_34296_luid_..._engtype_3d``). Summed per pid, joined against
  ``Win32_Process`` for image name and path. This is what makes a failure
  *actionable*: it names the process, not a number.
* **Aggregate** -- ``nvidia-smi --query-gpu=utilization.gpu,...``, the driver's
  own view. Recorded always; used as the only signal if the counters cannot be
  read, in which case attribution is lost and the record says so.

``nvidia-smi --query-compute-apps`` is deliberately *not* the attribution source:
under WDDM it lists every desktop application touching the GPU -- 60 rows of
shell, browser and tray processes on this workstation, measured -- and reports
``[N/A]`` for each one's memory. That is volume without attribution.

Ours vs. foreign
----------------
Only *foreign* GPU load gates. Our own build and tests are supposed to use the
GPU. A process counts as ours when it is a descendant of the job's root process
(the runner worker, resolved by walking our own ancestry) or when its image lives
under a CI workspace root. Everything else is foreign -- including the desktop
compositor, which is why the threshold sits above the measured idle desktop
floor rather than at zero.

Usage::

    python tests/ci/runner_gpu_contention.py preflight    # wait for a free GPU
    python tests/ci/runner_gpu_contention.py postflight    # start-vs-end verdict
    python tests/ci/runner_gpu_contention.py probe         # one-shot, for humans

Exit codes: ``0`` clean, :data:`EXIT_RUNNER_BUSY` (75) runner busy / result void,
``1`` internal error, ``2`` usage error.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Sequence, Tuple

# --------------------------------------------------------------------------
# Policy -- every number here is measured or justified, none is a guess
# --------------------------------------------------------------------------

#: A *foreign* process at or above this share of the GPU counts as contending.
#:
#: Measured on this runner (RTX 3090, Windows 11) with the box otherwise quiet:
#: the whole desktop -- compositor, shell, browser, tray -- produces per-process
#: GPU-engine values of **1-4 %**, and #878's interactive probing recorded the
#: aggregate at **3-7 %** across a working session. A single Godot test process
#: is an order of magnitude above that (see the calibration table in the PR).
#:
#: 15 % therefore sits clear of the desktop floor with roughly 4x margin and far
#: below anything that could produce #867's measured 2.7-3.0x job-wide slowdown.
#: It is deliberately *not* set at the noise floor: a threshold that fires on the
#: shell would void every run, and a gate nobody believes gets disabled.
FOREIGN_GPU_BUSY_PERCENT = 15.0

#: A foreign process must clear the threshold in this many *consecutive* samples
#: before the run is called contended. One sample is a blip -- a thumbnail
#: decode, a window animation -- and voiding a two-hour job on a blip is its own
#: false failure. Two samples at :data:`SAMPLER_INTERVAL_SEC` is a minute of
#: sustained foreign GPU work, which is the shape of every interference event on
#: record (gdUnit4 suites, editor sessions, builds); none of them lasted seconds.
CONTENDED_SAMPLES_REQUIRED = 2

#: Consecutive clean samples needed to declare the GPU free and release the wait.
#: Symmetric with the rule above, and for the same reason in reverse: starting a
#: job in the gap between two frames of someone else's workload is how a "clean at
#: start" reading becomes a contended run.
IDLE_SAMPLES_REQUIRED = 2

#: Seconds between samples **in the preflight's wait loop**. Nothing of ours is
#: running yet at that point, so the sample's own cost does not matter and
#: latency does: 20 s resolves "the machine became free" to within half a minute.
POLL_INTERVAL_SEC = 20.0

#: Seconds between samples **in the background sampler**, which runs while the
#: job's own GPU work does. Here the cost is the whole consideration, because a
#: monitor heavy enough to perturb the thing it measures is the failure this
#: repository has already had once (a perf gate measuring its own harness).
#:
#: Measured on this runner, one sample costs ~8.5 s of wall time in a PowerShell
#: child: ~6.5 s of it is `Get-Counter` expanding the ~890-instance
#: `\GPU Engine(*)` wildcard, ~1.5 s the `Win32_Process` enumeration, ~0.3 s
#: interpreter start. At 60 s that is a ~14 % duty cycle on **one** thread of a
#: 24-thread box, and -- the part that actually matters for these gates -- the
#: sampler does no GPU work at all, which the record proves rather than asserts:
#: its own pid appears in every sample's `loads` list, at 0 %.
SAMPLER_INTERVAL_SEC = 60.0

#: How long the preflight waits for the GPU to become free before declaring the
#: runner busy.
#:
#: **Why 15 minutes.** Three constraints bound it from both sides:
#:
#: * *From below* -- it must outlast a plausible transient. The interference on
#:   record is gdUnit4 suites and editor sessions; our own equivalent, the GPU
#:   harness, runs 3-8 minutes per batch. A bound under ~10 minutes would void
#:   runs that only had to wait for one suite to end.
#: * *From above* -- the runner is serialised, one job at a time, so every minute
#:   spent waiting is a minute the whole queue is stalled. And the wait is not
#:   free of its own risk: 900 s is 12.5 % of the 120-minute job timeout on
#:   ``gpu-tests``, so a job that waits the entire bound still keeps >105 minutes
#:   of its budget and cannot fail *as a timeout* because of the wait.
#: * *From the machine's purpose* -- beyond a quarter of an hour, the contending
#:   work is not a transient, it is the owner using their workstation. That is a
#:   human matter, and the right answer is a loud "retry when the machine is
#:   free", not CI sitting on the queue indefinitely.
#:
#: **Not verified:** how long the observed interfering runs actually lasted. The
#: timestamps in #875 record when they were *seen*, not their duration, so this
#: bound is argued from our own comparable workloads rather than measured against
#: theirs. If void verdicts cluster at exactly this bound, that is the signal to
#: re-derive it from the sampler's own series, which now exists.
DEFAULT_WAIT_TIMEOUT_SEC = 900.0

#: Largest gap between consecutive samples the postflight will still accept as
#: continuous coverage. A sampler that died mid-job leaves a blind window, and a
#: blind window reported as "clean" is precisely the vacuous pass this module is
#: built to remove -- so an over-long gap is :data:`VERDICT_UNMEASURED`, which is
#: void.
#:
#: 300 s is 5x :data:`SAMPLER_INTERVAL_SEC`. Generous on purpose: the GPU harness
#: saturates this box's CPU, and a starved sampler must not be able to void an
#: otherwise sound run. Five blind minutes inside a 30-120 minute job is a real
#: hole in the record; five sampling intervals of scheduling slack is not.
MAX_SAMPLE_GAP_SEC = 300.0

#: Hard lifetime for the detached sampler, so a job killed between preflight and
#: postflight cannot leave a process behind on a persistent runner forever. Set
#: above the longest GPU job timeout in the workflows (120 minutes) plus margin.
SAMPLER_MAX_LIFETIME_SEC = 3.0 * 60.0 * 60.0

#: The clean-run p95/avg frame-time ratio this repository has already measured on
#: the streaming lane, and the issues that measured it. Printed next to every
#: verdict so a reader who arrives at a bare budget failure has the discriminator
#: and its provenance in the same block of log.
CLEAN_FRAME_P95_TO_AVG_RATIO = 1.15
FRAME_RATIO_REFERENCE = "#630/#624"

#: Environment overrides. Calibration knobs, not verdict knobs: they move *when*
#: the guard gives up waiting and *how sensitive* the busy test is. Nothing here
#: can turn a contended verdict into a pass.
ENV_WAIT_TIMEOUT = "GS_CI_GPU_WAIT_TIMEOUT_SEC"
ENV_BUSY_PERCENT = "GS_CI_GPU_BUSY_PERCENT"
ENV_POLL_INTERVAL = "GS_CI_GPU_POLL_INTERVAL_SEC"
ENV_SAMPLER_INTERVAL = "GS_CI_GPU_SAMPLER_INTERVAL_SEC"
ENV_RECORD_DIR = "GS_CI_GPU_CONTENTION_DIR"

#: Process image names that mark the boundary of "the job" when walking our own
#: ancestry. Everything below one of these is ours; the walk never goes above it,
#: so it cannot climb to `services.exe` and declare the whole machine ours.
RUNNER_PROCESS_NAMES = ("runner.worker.exe", "runner.listener.exe")

#: Environment variables naming a directory whose contents are ours. A GPU
#: process launched from inside the job's workspace is the job's, whether or not
#: the ancestry walk found the runner (a detached sampler has no live ancestry).
CI_ROOT_ENV_VARS = ("GITHUB_WORKSPACE", "RUNNER_WORKSPACE", "RUNNER_TEMP", "RUNNER_TOOL_CACHE")

#: `EX_TEMPFAIL`. Chosen because it is not 1: a job that ends here did not fail a
#: test, it produced no valid measurement at all, and the two must not share an
#: exit code.
EXIT_RUNNER_BUSY = 75

VERDICT_CLEAN = "CLEAN"
VERDICT_BUSY = "RUNNER_BUSY"
VERDICT_CONTENDED_MID_RUN = "CONTENDED_MID_RUN"
VERDICT_UNMEASURED = "UNMEASURED"

#: Verdicts that make the job's result void. Everything not in here is clean.
VOID_VERDICTS = (VERDICT_BUSY, VERDICT_CONTENDED_MID_RUN, VERDICT_UNMEASURED)

SESSION_FILE = "gpu_contention_session.json"
SAMPLES_FILE = "gpu_contention_samples.jsonl"
STOP_FILE = "gpu_contention_stop"

_PS_TIMEOUT_SEC = 120
_NVIDIA_SMI_TIMEOUT_SEC = 60
_GPU_QUERY = "utilization.gpu,memory.used,memory.total,name"


# --------------------------------------------------------------------------
# Measurement
# --------------------------------------------------------------------------

#: One PowerShell round trip yields both halves of a sample: the per-pid GPU
#: engine utilisation and the process table needed to name and classify those
#: pids. Two calls would sample two different instants and could attribute a
#: pid to a process that had already exited and been recycled.
#:
#: `Get-Counter` failing is captured rather than raised: losing attribution
#: degrades the sample to aggregate-only, which is worse but is still a
#: measurement, and the caller has to be able to tell the two apart.
_SAMPLE_PS = r"""
$ErrorActionPreference = 'Stop'
$counterError = $null
$byPid = @{}
$validSamples = 0
$invalidSamples = 0
try {
    # `-ErrorAction SilentlyContinue` plus a per-sample Status filter, NOT
    # `-Stop`. Measured on this runner: roughly one query in ten throws
    # "the data in one of the performance counter samples is not valid",
    # because the `\GPU Engine(*)` instance set has ~870 members that churn as
    # processes come and go, and PDH invalidates the *whole* query when any one
    # instance sample is bad. Letting that discard the measurement would turn
    # normal process churn into an UNMEASURED verdict -- a false void roughly
    # every tenth sample. Dropping the individual bad instances keeps the rest,
    # and a query that yields no valid sample at all is still an error below.
    $set = Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue -ErrorVariable counterWarnings
    $samples = @()
    if ($set) { $samples = @($set.CounterSamples) }
    foreach ($s in $samples) {
        if ($s.Status -ne 0) { $invalidSamples++; continue }
        $validSamples++
        $value = [double]$s.CookedValue
        if ($value -le 0) { continue }
        if ($s.InstanceName -match 'pid_(\d+)_') {
            $key = $Matches[1]
            if ($byPid.ContainsKey($key)) { $byPid[$key] = $byPid[$key] + $value }
            else { $byPid[$key] = $value }
        }
    }
    if ($validSamples -eq 0) {
        # The desktop compositor alone always holds a GPU engine instance, so an
        # empty valid set means the query failed, not that the GPU is idle.
        $counterError = 'no valid GPU engine counter samples were returned'
        if ($counterWarnings -and $counterWarnings.Count -gt 0) {
            $counterError = $counterError + ': ' + $counterWarnings[0].ToString()
        }
    }
} catch { $counterError = $_.Exception.Message }
$processError = $null
$procs = @{}
try {
    foreach ($p in Get-CimInstance -ClassName Win32_Process -Property ProcessId,ParentProcessId,Name,ExecutablePath -ErrorAction Stop) {
        $procs[[string]$p.ProcessId] = [ordered]@{
            ppid = [int]$p.ParentProcessId
            name = [string]$p.Name
            path = [string]$p.ExecutablePath
        }
    }
} catch { $processError = $_.Exception.Message }
$out = [ordered]@{
    counter_error = $counterError
    process_error = $processError
    valid_counter_samples = $validSamples
    invalid_counter_samples = $invalidSamples
    gpu_percent_by_pid = $byPid
    processes = $procs
}
$out | ConvertTo-Json -Depth 5 -Compress
"""


class ProcessLoad(NamedTuple):
    """One process's measured GPU share at one instant."""

    pid: int
    gpu_percent: float
    name: str
    path: str
    ours: bool

    def describe(self) -> str:
        return (
            f"pid {self.pid:>7}  {self.gpu_percent:6.1f}% GPU  {self.name or '<unknown>'}"
            f"  [{self.path or 'image path unavailable'}]"
        )

    def as_dict(self) -> Dict[str, object]:
        return {
            "pid": self.pid,
            "gpu_percent": round(self.gpu_percent, 2),
            "name": self.name,
            "path": self.path,
            "ours": self.ours,
        }


class Sample(NamedTuple):
    """A single point in the occupancy series."""

    at: float
    #: Foreign processes over :data:`FOREIGN_GPU_BUSY_PERCENT`, worst first.
    contenders: Tuple[ProcessLoad, ...]
    #: Every process with any measurable GPU share, ours included. Recorded so a
    #: verdict can be re-read later against a different threshold without
    #: re-running the job.
    all_loads: Tuple[ProcessLoad, ...]
    #: The driver's own aggregate rows (`nvidia-smi`), or an empty tuple.
    gpu_rows: Tuple[str, ...]
    #: Set when the sample could not be taken at all. A sample with an error is
    #: never evidence of an idle machine.
    error: Optional[str]

    @property
    def usable(self) -> bool:
        return self.error is None

    @property
    def busy(self) -> bool:
        return bool(self.contenders)

    def as_dict(self) -> Dict[str, object]:
        return {
            "at": round(self.at, 3),
            "error": self.error,
            "contenders": [load.as_dict() for load in self.contenders],
            "loads": [load.as_dict() for load in self.all_loads],
            "gpu_rows": list(self.gpu_rows),
        }


def _encoded_command(script: str) -> List[str]:
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    return [
        "powershell.exe",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-EncodedCommand",
        encoded,
    ]


def run_sample_script() -> Tuple[Optional[Dict[str, object]], Optional[str]]:
    """`(parsed payload, error)` from one PowerShell round trip."""
    try:
        completed = subprocess.run(
            _encoded_command(_SAMPLE_PS),
            capture_output=True,
            text=True,
            errors="replace",
            timeout=_PS_TIMEOUT_SEC,
        )
    except OSError as exc:
        return None, f"could not run powershell.exe: {exc}"
    except subprocess.TimeoutExpired:
        return None, f"the GPU occupancy probe did not finish in {_PS_TIMEOUT_SEC}s"
    if completed.returncode != 0:
        stderr = (completed.stderr or "").strip().replace("\n", " ")[:400]
        return None, f"the GPU occupancy probe exited {completed.returncode}: {stderr}"
    text = (completed.stdout or "").strip()
    if not text:
        return None, "the GPU occupancy probe exited 0 but printed nothing"
    try:
        payload = json.loads(text)
    except ValueError as exc:
        return None, f"the GPU occupancy probe printed unparseable JSON: {exc}"
    if not isinstance(payload, dict):
        return None, "the GPU occupancy probe printed JSON that is not an object"
    return payload, None


def query_nvidia_smi() -> Tuple[Tuple[str, ...], Optional[str]]:
    """The driver's aggregate view. Never gates on its own; always recorded."""
    import shutil  # noqa: PLC0415 -- only needed on the measurement path

    smi = shutil.which("nvidia-smi")
    if not smi:
        return (), "nvidia-smi is not on PATH"
    try:
        completed = subprocess.run(
            [smi, f"--query-gpu={_GPU_QUERY}", "--format=csv,noheader"],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=_NVIDIA_SMI_TIMEOUT_SEC,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return (), f"nvidia-smi failed: {exc}"
    if completed.returncode != 0:
        stderr = (completed.stderr or "").strip()
        return (), f"nvidia-smi exited {completed.returncode}" + (f": {stderr}" if stderr else "")
    rows = tuple(row.strip() for row in (completed.stdout or "").splitlines() if row.strip())
    if not rows:
        # A clean exit that names no GPU is not an occupancy measurement -- same
        # rule the environment preflight settled on in #878.
        return (), "nvidia-smi exited 0 but reported no GPU rows"
    return rows, None


# --------------------------------------------------------------------------
# Attribution
# --------------------------------------------------------------------------


def ci_roots() -> Tuple[str, ...]:
    """Directories whose executables belong to the job."""
    roots = []
    for name in CI_ROOT_ENV_VARS:
        value = os.environ.get(name)
        if value:
            roots.append(os.path.normcase(os.path.abspath(value)))
    repo_root = Path(__file__).resolve().parents[2]
    roots.append(os.path.normcase(str(repo_root)))
    return tuple(sorted(set(roots)))


def resolve_job_root_pid(
    processes: Dict[int, Dict[str, object]], start_pid: Optional[int] = None
) -> int:
    """The pid whose descendants are "ours".

    Walks up from `start_pid` (this process by default) to the nearest ancestor
    that is a runner process, and stops there. If the walk never meets one -- a
    local run, or a runner that renamed its worker -- the answer is `start_pid`
    itself, so the classification degrades to "our own subtree" rather than
    silently widening to the whole machine.
    """
    pid = os.getpid() if start_pid is None else start_pid
    seen = set()
    while pid in processes and pid not in seen:
        seen.add(pid)
        name = str(processes[pid].get("name") or "").lower()
        if name in RUNNER_PROCESS_NAMES:
            return pid
        parent = int(processes[pid].get("ppid") or 0)
        if parent in (0, pid) or parent not in processes:
            break
        pid = parent
    return os.getpid() if start_pid is None else start_pid


def descendants_of(processes: Dict[int, Dict[str, object]], root_pid: int) -> set:
    """`root_pid` and every process below it.

    Windows recycles pids and a `ppid` can point at a slot whose original owner
    exited, so a parent link is followed only downwards from a known-good root --
    never used to prove that an arbitrary process is unrelated.
    """
    children: Dict[int, List[int]] = {}
    for pid, entry in processes.items():
        children.setdefault(int(entry.get("ppid") or 0), []).append(pid)
    out = {root_pid}
    stack = [root_pid]
    while stack:
        current = stack.pop()
        for child in children.get(current, ()):
            if child not in out:
                out.add(child)
                stack.append(child)
    return out


def classify(
    gpu_percent_by_pid: Dict[int, float],
    processes: Dict[int, Dict[str, object]],
    ours_pids: set,
    roots: Sequence[str],
) -> List[ProcessLoad]:
    loads: List[ProcessLoad] = []
    for pid, percent in gpu_percent_by_pid.items():
        entry = processes.get(pid, {})
        name = str(entry.get("name") or "")
        path = str(entry.get("path") or "")
        ours = pid in ours_pids
        if not ours and path:
            normalised = os.path.normcase(os.path.abspath(path))
            ours = any(normalised.startswith(root) for root in roots)
        loads.append(ProcessLoad(pid, float(percent), name, path, ours))
    loads.sort(key=lambda load: load.gpu_percent, reverse=True)
    return loads


def take_sample(
    job_root_pid: Optional[int] = None, busy_percent: Optional[float] = None
) -> Sample:
    """One point in the series: who is using the GPU, and are they ours."""
    threshold = FOREIGN_GPU_BUSY_PERCENT if busy_percent is None else busy_percent
    now = time.time()
    gpu_rows, smi_error = query_nvidia_smi()
    payload, error = run_sample_script()
    if payload is None:
        return Sample(now, (), (), gpu_rows, error)

    counter_error = payload.get("counter_error")
    process_error = payload.get("process_error")
    if counter_error:
        # Without the per-pid counters there is no attribution, and this module's
        # entire value is attribution. Aggregate-only is recorded but must not be
        # reported as a clean sample: "we could not see who" is not "nobody".
        detail = f"per-process GPU counters unavailable: {counter_error}"
        if smi_error:
            detail += f"; aggregate also unavailable: {smi_error}"
        return Sample(now, (), (), gpu_rows, detail)
    if process_error:
        return Sample(now, (), (), gpu_rows, f"process table unavailable: {process_error}")

    raw_counts = payload.get("gpu_percent_by_pid") or {}
    raw_procs = payload.get("processes") or {}
    processes: Dict[int, Dict[str, object]] = {}
    for key, entry in raw_procs.items():
        try:
            processes[int(key)] = entry if isinstance(entry, dict) else {}
        except (TypeError, ValueError):
            continue
    by_pid: Dict[int, float] = {}
    for key, value in raw_counts.items():
        try:
            by_pid[int(key)] = float(value)
        except (TypeError, ValueError):
            continue

    root = job_root_pid if job_root_pid is not None else resolve_job_root_pid(processes)
    ours_pids = descendants_of(processes, root)
    # The sampler is detached, so once the preflight step ends its parent slot is
    # gone and the ancestry walk cannot reach the runner from it. Its own pid is
    # therefore added explicitly rather than left to be classified as foreign --
    # it does no GPU work, so this changes no verdict, but a monitor that could
    # in principle accuse itself is a bad monitor.
    ours_pids.add(os.getpid())
    loads = classify(by_pid, processes, ours_pids, ci_roots())
    contenders = tuple(
        load for load in loads if not load.ours and load.gpu_percent >= threshold
    )
    return Sample(now, contenders, tuple(loads), gpu_rows, None)


# --------------------------------------------------------------------------
# Records
# --------------------------------------------------------------------------


def record_dir(explicit: Optional[Path] = None) -> Path:
    if explicit is not None:
        return explicit
    configured = os.environ.get(ENV_RECORD_DIR)
    if configured:
        return Path(configured)
    runner_temp = os.environ.get("RUNNER_TEMP")
    if runner_temp:
        return Path(runner_temp) / "gs-gpu-contention"
    return Path(__file__).resolve().parents[2] / "artifacts" / "gpu-contention"


def _float_env(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if not raw:
        return default
    try:
        value = float(raw)
    except ValueError:
        return default
    return value if value > 0 else default


def append_sample(path: Path, sample: Sample) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(sample.as_dict(), sort_keys=True) + "\n")


def read_samples(path: Path) -> List[Dict[str, object]]:
    if not path.is_file():
        return []
    out: List[Dict[str, object]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if isinstance(entry, dict):
            out.append(entry)
    return out


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------


def annotate(title: str, message: str) -> None:
    """A GitHub Actions error annotation, so the verdict is visible without
    opening the log. Harmless noise anywhere else."""
    flat = message.replace("\r", "").replace("\n", "%0A")
    print(f"::error title={title}::{flat}")


def write_step_summary(lines: Sequence[str]) -> None:
    target = os.environ.get("GITHUB_STEP_SUMMARY")
    if not target:
        return
    try:
        with open(target, "a", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
    except OSError:
        pass  # a summary we could not write must never change the verdict


def banner(lines: Sequence[str]) -> List[str]:
    width = 78
    out = ["", "=" * width]
    out.extend(lines)
    out.append("=" * width)
    out.append("")
    return out


def discriminator_note() -> List[str]:
    """The p95/avg reading, printed with every verdict.

    A reader who arrives at a bare `first_visible_exceeded` has no way to tell a
    slow renderer from a busy machine. This repository already measured the
    difference; repeating it next to the verdict is what turns "budget exceeded"
    into "ratio 1.94 -- this is contention".
    """
    return [
        "  How to read a timing failure from this runner:",
        f"    frame_p95_to_avg_ratio ~= {CLEAN_FRAME_P95_TO_AVG_RATIO:.2f} on a clean run of the",
        f"      streaming lane ({FRAME_RATIO_REFERENCE}). A ratio near 1.9 with residency 1.0,",
        "      fallback rate 0.0 and readiness READY is the signature of a contended",
        "      machine, not a renderer regression (#881).",
        "    Budgets are NOT relaxed for contention. A contended run is void, not passed.",
    ]


def describe_sample(
    sample: Sample, indent: str = "    ", busy_percent: float = FOREIGN_GPU_BUSY_PERCENT
) -> List[str]:
    lines: List[str] = []
    if sample.error:
        lines.append(f"{indent}UNMEASURED: {sample.error}")
    for row in sample.gpu_rows:
        lines.append(f"{indent}nvidia-smi [{_GPU_QUERY}] = {row}")
    if sample.contenders:
        lines.append(f"{indent}foreign processes on the GPU:")
        for load in sample.contenders:
            lines.append(f"{indent}  {load.describe()}")
    elif sample.usable:
        ours = [load for load in sample.all_loads if load.ours]
        lines.append(
            f"{indent}no foreign process at or above {busy_percent:.0f}% GPU"
            f" ({len(sample.all_loads)} process(es) with any GPU share, {len(ours)} ours)"
        )
        for load in sample.all_loads[:5]:
            lines.append(
                f"{indent}  {'ours   ' if load.ours else 'foreign'} {load.describe()}"
            )
    return lines


# --------------------------------------------------------------------------
# Phase: preflight -- wait for a free GPU, then leave a sampler running
# --------------------------------------------------------------------------


def wait_for_free_gpu(
    timeout_sec: float,
    poll_interval_sec: float,
    busy_percent: float,
    job_root_pid: Optional[int],
    sleep=time.sleep,
    now=time.monotonic,
) -> Tuple[bool, List[Sample], List[str]]:
    """Poll until the GPU is free of foreign load, or the bound expires.

    Returns `(free, samples, log lines)`. `free` is True only after
    :data:`IDLE_SAMPLES_REQUIRED` consecutive *usable* clean samples: a sample
    that could not be taken resets the streak rather than counting towards it.
    """
    deadline = now() + timeout_sec
    samples: List[Sample] = []
    lines: List[str] = []
    clean_streak = 0
    while True:
        sample = take_sample(job_root_pid, busy_percent)
        samples.append(sample)
        elapsed = timeout_sec - max(deadline - now(), 0.0)
        if sample.usable and not sample.busy:
            clean_streak += 1
            lines.append(
                f"  [{elapsed:6.0f}s] clean ({clean_streak}/{IDLE_SAMPLES_REQUIRED})"
            )
        else:
            clean_streak = 0
            if sample.busy:
                names = ", ".join(
                    f"{load.name or load.pid} {load.gpu_percent:.0f}%"
                    for load in sample.contenders
                )
                lines.append(f"  [{elapsed:6.0f}s] busy -- {names}")
            else:
                lines.append(f"  [{elapsed:6.0f}s] unmeasured -- {sample.error}")
        if clean_streak >= IDLE_SAMPLES_REQUIRED:
            return True, samples, lines
        if now() >= deadline:
            return False, samples, lines
        sleep(min(poll_interval_sec, max(deadline - now(), 0.0)))


def spawn_sampler(
    session_dir: Path, job_root_pid: int, sampler_interval_sec: float, busy_percent: float
) -> Tuple[Optional[int], Optional[str]]:
    """Start the detached background sampler that covers the rest of the job.

    Detached on purpose: it has to outlive the preflight *step*, which the runner
    ends as soon as its command returns. Two independent stops bound it -- the
    stop file the postflight writes, and its own absolute deadline -- because a
    stray sampler on a persistent runner is a defect of its own.
    """
    stop_path = session_dir / STOP_FILE
    if stop_path.exists():
        try:
            stop_path.unlink()
        except OSError as exc:
            return None, f"could not clear the previous stop file: {exc}"
    command = [
        sys.executable,
        str(Path(__file__).resolve()),
        "sample",
        "--record-dir",
        str(session_dir),
        "--job-root-pid",
        str(job_root_pid),
        "--poll-interval-sec",
        str(sampler_interval_sec),
        "--busy-percent",
        str(busy_percent),
        "--max-lifetime-sec",
        str(SAMPLER_MAX_LIFETIME_SEC),
    ]
    creationflags = 0
    if os.name == "nt":
        creationflags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(
            subprocess, "CREATE_NEW_PROCESS_GROUP", 0
        )
    try:
        process = subprocess.Popen(  # noqa: S603 -- fixed argv, no shell
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=creationflags,
            close_fds=True,
        )
    except OSError as exc:
        return None, f"could not start the background sampler: {exc}"
    return process.pid, None


def phase_preflight(args: argparse.Namespace) -> int:
    session_dir = record_dir(args.record_dir)
    session_dir.mkdir(parents=True, exist_ok=True)
    samples_path = session_dir / SAMPLES_FILE
    session_path = session_dir / SESSION_FILE
    # A stale series from a previous job on this persistent runner would be read
    # by the postflight as this job's own history.
    for stale in (samples_path, session_dir / STOP_FILE):
        if stale.exists():
            try:
                stale.unlink()
            except OSError:
                pass

    timeout_sec = args.timeout_sec
    poll_interval_sec = args.poll_interval_sec
    busy_percent = args.busy_percent

    print("RUNNER GPU CONTENTION -- preflight (#875)")
    print(f"  waiting up to {timeout_sec:.0f}s for the GPU to be free of foreign load")
    print(
        f"  a foreign process at or above {busy_percent:.0f}% GPU counts as contending; "
        f"{IDLE_SAMPLES_REQUIRED} consecutive clean samples release the wait"
    )

    bootstrap, _bootstrap_error = run_sample_script()
    processes: Dict[int, Dict[str, object]] = {}
    if bootstrap:
        for key, entry in (bootstrap.get("processes") or {}).items():
            try:
                processes[int(key)] = entry if isinstance(entry, dict) else {}
            except (TypeError, ValueError):
                continue
    job_root_pid = resolve_job_root_pid(processes)
    root_name = str(processes.get(job_root_pid, {}).get("name") or "<this process>")
    print(f"  job root pid for attribution: {job_root_pid} ({root_name})")

    free, samples, log_lines = wait_for_free_gpu(
        timeout_sec, poll_interval_sec, busy_percent, job_root_pid
    )
    for line in log_lines:
        print(line)

    last = samples[-1] if samples else None
    session: Dict[str, object] = {
        "started_at": samples[0].at if samples else time.time(),
        "job_root_pid": job_root_pid,
        "busy_percent": busy_percent,
        "poll_interval_sec": poll_interval_sec,
        "wait_timeout_sec": timeout_sec,
        "wait_samples": [sample.as_dict() for sample in samples],
        "start_verdict": VERDICT_CLEAN if free else VERDICT_BUSY,
        "github_run_id": os.environ.get("GITHUB_RUN_ID"),
        "github_job": os.environ.get("GITHUB_JOB"),
    }

    if not free:
        blockers = tuple(last.contenders) if last is not None else ()
        held_by = [f"    {load.describe()}" for load in blockers]
        if not held_by:
            why = last.error if last is not None else "no sample was taken"
            held_by = [f"    (unmeasurable) {why}"]
        tail = describe_sample(last, indent="    ", busy_percent=busy_percent) if last else []
        for line in banner(
            [
                "  RUNNER BUSY -- THIS RESULT IS VOID. RETRY WHEN THE MACHINE IS FREE.",
                "",
                f"  The GPU was still held by unrelated work after {timeout_sec:.0f}s of waiting.",
                "  Nothing was built and nothing was measured, so this is NOT a renderer",
                "  regression and NOT a test failure -- there is no result to interpret.",
                "",
                "  Holding the GPU right now:",
            ]
            + held_by
            + [""]
            + tail
        ):
            print(line)
        for line in discriminator_note():
            print(line)
        names = ", ".join(f"{load.name or load.pid} (pid {load.pid})" for load in blockers)
        annotate(
            "RUNNER BUSY - result void",
            "The self-hosted GPU runner was busy with unrelated work for the whole "
            f"{timeout_sec:.0f}s wait, so this job produced no valid measurement. "
            + (f"Holding the GPU: {names}. " if names else "")
            + "Retry when the machine is free. This is not a renderer regression (#875).",
        )
        write_step_summary(
            ["### RUNNER BUSY - this result is void (#875)", ""]
            + [f"- `{load.describe()}`" for load in blockers]
            + ["", "Nothing was built or measured. Retry when the machine is free."]
        )
        session["sampler_pid"] = None
        session_path.write_text(json.dumps(session, indent=2, sort_keys=True), encoding="utf-8")
        return EXIT_RUNNER_BUSY

    sampler_pid, sampler_error = spawn_sampler(
        session_dir, job_root_pid, args.sampler_interval_sec, busy_percent
    )
    session["sampler_pid"] = sampler_pid
    session["sampler_interval_sec"] = args.sampler_interval_sec
    session["sampler_error"] = sampler_error
    session_path.write_text(json.dumps(session, indent=2, sort_keys=True), encoding="utf-8")

    if last is not None:
        for line in describe_sample(last, busy_percent=busy_percent):
            print(line)
    if sampler_error:
        # Not fatal here -- the job can still run. But the postflight will find no
        # series and report UNMEASURED, which is void, so this cannot pass quietly.
        print(f"  WARNING: {sampler_error}")
        print("  Mid-run contention will not be observable; the postflight will say so.")
    else:
        print(
            f"  background sampler started (pid {sampler_pid}), sampling every "
            f"{args.sampler_interval_sec:.0f}s until the postflight stops it"
        )
    print("  PASS: the GPU was free of foreign load at job start.")
    print(f"  session record: {session_path}")
    return 0


# --------------------------------------------------------------------------
# Phase: sample -- the detached background monitor
# --------------------------------------------------------------------------


def phase_sample(args: argparse.Namespace) -> int:
    session_dir = record_dir(args.record_dir)
    session_dir.mkdir(parents=True, exist_ok=True)
    samples_path = session_dir / SAMPLES_FILE
    stop_path = session_dir / STOP_FILE
    deadline = time.monotonic() + args.max_lifetime_sec
    while True:
        sample = take_sample(args.job_root_pid, args.busy_percent)
        try:
            append_sample(samples_path, sample)
        except OSError:
            return 1
        if stop_path.exists() or time.monotonic() >= deadline:
            return 0
        time.sleep(args.poll_interval_sec)


# --------------------------------------------------------------------------
# Phase: postflight -- start vs. end, and everything in between
# --------------------------------------------------------------------------


class SeriesVerdict(NamedTuple):
    verdict: str
    reasons: List[str]
    #: Contended windows as `(first_at, last_at, [descriptions])`.
    windows: List[Tuple[float, float, List[str]]]
    largest_gap_sec: float
    sample_count: int


def evaluate_series(
    entries: Sequence[Dict[str, object]],
    started_at: float,
    ended_at: float,
    max_gap_sec: float = MAX_SAMPLE_GAP_SEC,
    contended_samples_required: int = CONTENDED_SAMPLES_REQUIRED,
) -> SeriesVerdict:
    """Turn the sampled series into a verdict about the *whole* job window.

    Three outcomes, and the order matters. Contention is decided first: a run
    that was demonstrably contended is void whether or not the record also has
    holes. Only a series with no observed contention has to prove it was
    *continuous* before it may be called clean -- because a gap is exactly where
    unobserved contention would hide, and "we did not see any" from a monitor
    that was not running is the vacuous pass this exists to remove.
    """
    reasons: List[str] = []
    if not entries:
        return SeriesVerdict(
            VERDICT_UNMEASURED,
            [
                "No samples were recorded between the preflight and now, so nothing is "
                "known about GPU contention during this job. The background sampler did "
                "not run or could not write its series."
            ],
            [],
            max(ended_at - started_at, 0.0),
            0,
        )

    ordered = sorted(entries, key=lambda entry: float(entry.get("at") or 0.0))
    stamps = [float(entry.get("at") or 0.0) for entry in ordered]
    boundaries = [started_at] + stamps + [ended_at]
    largest_gap = max(
        (later - earlier for earlier, later in zip(boundaries, boundaries[1:])),
        default=0.0,
    )

    windows: List[Tuple[float, float, List[str]]] = []
    streak: List[Dict[str, object]] = []

    def flush() -> None:
        if len(streak) < contended_samples_required:
            streak.clear()
            return
        described: List[str] = []
        seen = set()
        for entry in streak:
            for load in entry.get("contenders") or []:
                key = (load.get("pid"), load.get("name"))
                if key in seen:
                    continue
                seen.add(key)
                described.append(
                    f"pid {load.get('pid')}  peak {float(load.get('gpu_percent') or 0.0):.1f}%"
                    f" GPU  {load.get('name') or '<unknown>'}"
                    f"  [{load.get('path') or 'image path unavailable'}]"
                )
        windows.append(
            (
                float(streak[0].get("at") or 0.0),
                float(streak[-1].get("at") or 0.0),
                described,
            )
        )
        streak.clear()

    for entry in ordered:
        if entry.get("contenders"):
            streak.append(entry)
        else:
            flush()
    flush()

    if windows:
        for first, last, described in windows:
            reasons.append(
                f"Foreign GPU load from {_stamp(first)} to {_stamp(last)} "
                f"({max(last - first, 0.0):.0f}s):"
            )
            reasons.extend(f"    {line}" for line in described)
        return SeriesVerdict(
            VERDICT_CONTENDED_MID_RUN, reasons, windows, largest_gap, len(ordered)
        )

    unusable = [entry for entry in ordered if entry.get("error")]
    if len(unusable) == len(ordered):
        reasons.append(
            "Every sample taken during this job failed to measure GPU occupancy "
            f"(first: {unusable[0].get('error')}). Nothing is known about contention."
        )
        return SeriesVerdict(VERDICT_UNMEASURED, reasons, [], largest_gap, len(ordered))

    if largest_gap > max_gap_sec:
        reasons.append(
            f"The occupancy series has a {largest_gap:.0f}s gap, over the {max_gap_sec:.0f}s "
            "this guard accepts as continuous coverage. The background sampler stopped or "
            "was starved, so part of this job was unobserved -- and an unobserved window "
            "reported as clean is exactly the false green this check exists to prevent."
        )
        return SeriesVerdict(VERDICT_UNMEASURED, reasons, [], largest_gap, len(ordered))

    reasons.append(
        f"{len(ordered)} samples over {max(ended_at - started_at, 0.0):.0f}s, largest gap "
        f"{largest_gap:.0f}s, no foreign process sustained above the busy threshold."
    )
    return SeriesVerdict(VERDICT_CLEAN, reasons, [], largest_gap, len(ordered))


def _stamp(epoch: float) -> str:
    return time.strftime("%H:%M:%S", time.localtime(epoch))


def phase_postflight(args: argparse.Namespace) -> int:
    session_dir = record_dir(args.record_dir)
    session_path = session_dir / SESSION_FILE
    samples_path = session_dir / SAMPLES_FILE
    stop_path = session_dir / STOP_FILE

    print("RUNNER GPU CONTENTION -- postflight (#875)")

    session: Dict[str, object] = {}
    if session_path.is_file():
        try:
            loaded = json.loads(session_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                session = loaded
        except ValueError:
            pass

    # Stop the sampler first, so the final sample below is not racing it.
    try:
        stop_path.parent.mkdir(parents=True, exist_ok=True)
        stop_path.write_text("stop\n", encoding="utf-8")
    except OSError as exc:
        print(f"  WARNING: could not write the sampler stop file: {exc}")

    job_root_pid = session.get("job_root_pid")
    end_sample = take_sample(
        int(job_root_pid) if isinstance(job_root_pid, int) else None, args.busy_percent
    )
    try:
        append_sample(samples_path, end_sample)
    except OSError as exc:
        print(f"  WARNING: could not append the closing sample: {exc}")

    start_verdict = str(session.get("start_verdict") or VERDICT_UNMEASURED)
    started_at = float(session.get("started_at") or end_sample.at)
    entries = read_samples(samples_path)
    series = evaluate_series(entries, started_at, end_sample.at)

    print(f"  start verdict (preflight)  : {start_verdict}")
    print(f"  end sample                 : {'CLEAN' if end_sample.usable and not end_sample.busy else 'CONTENDED' if end_sample.busy else 'UNMEASURED'}")
    for line in describe_sample(end_sample, indent="    ", busy_percent=args.busy_percent):
        print(line)
    print(f"  samples covering the job   : {series.sample_count} (largest gap {series.largest_gap_sec:.0f}s)")

    # The end of the series has no "next" sample, so the consecutive-samples rule
    # that protects the middle of a run against blips would systematically drop
    # contention that starts near the end -- and "clean at start, contended by the
    # end" is precisely the case this postflight exists to report. So when the
    # closing sample is busy but formed no window, it is confirmed with one more
    # measurement rather than either ignored or trusted alone. That costs ~10s,
    # and only on runs where something was actually seen.
    if series.verdict == VERDICT_CLEAN and end_sample.busy:
        print("  closing sample shows foreign GPU load; taking one confirming sample")
        time.sleep(min(5.0, args.poll_interval_sec))
        confirm = take_sample(
            int(job_root_pid) if isinstance(job_root_pid, int) else None, args.busy_percent
        )
        try:
            append_sample(samples_path, confirm)
        except OSError:
            pass
        if confirm.busy:
            described = [
                f"pid {load.pid}  {load.gpu_percent:.1f}% GPU  {load.name or '<unknown>'}"
                f"  [{load.path or 'image path unavailable'}]"
                for load in confirm.contenders
            ]
            series = SeriesVerdict(
                VERDICT_CONTENDED_MID_RUN,
                [
                    "The job was clean at start and is contended NOW, at job end, "
                    "confirmed by two consecutive samples:"
                ]
                + [f"    {line}" for line in described],
                [(end_sample.at, confirm.at, described)],
                series.largest_gap_sec,
                series.sample_count + 1,
            )
        else:
            print(
                "  the confirming sample was clean, so the closing reading was a single-sample "
                "spike rather than sustained contention; not treated as a contended run"
            )

    if not session:
        series = SeriesVerdict(
            VERDICT_UNMEASURED,
            [
                f"No preflight session record at {session_path}. Without it there is no "
                "start-of-job measurement to compare against, so a clean end sample proves "
                "nothing about the job that just ran."
            ],
            [],
            series.largest_gap_sec,
            series.sample_count,
        )

    verdict = series.verdict
    if start_verdict == VERDICT_BUSY:
        verdict = VERDICT_BUSY

    record = {
        "verdict": verdict,
        "start_verdict": start_verdict,
        "end_sample": end_sample.as_dict(),
        "series_verdict": series.verdict,
        "series_reasons": series.reasons,
        "largest_gap_sec": round(series.largest_gap_sec, 3),
        "sample_count": series.sample_count,
        "clean_frame_p95_to_avg_ratio": CLEAN_FRAME_P95_TO_AVG_RATIO,
        "frame_ratio_reference": FRAME_RATIO_REFERENCE,
    }
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(record, indent=2, sort_keys=True), encoding="utf-8")
        print(f"  record written to {args.json}")

    if verdict == VERDICT_CLEAN:
        for line in series.reasons:
            print(f"  {line}")
        print(
            "  PASS: the GPU was free of foreign load at job start, at job end, and "
            "continuously in between. Timing results from this job are attributable."
        )
        return 0

    headline = {
        VERDICT_BUSY: "RUNNER BUSY -- THIS RESULT IS VOID.",
        VERDICT_CONTENDED_MID_RUN: "RUNNER BUSY MID-RUN -- THIS RESULT IS VOID.",
        VERDICT_UNMEASURED: "RUNNER CONTENTION UNMEASURED -- THIS RESULT IS VOID.",
    }[verdict]
    detail = {
        VERDICT_CONTENDED_MID_RUN: (
            "  The GPU was free when this job started and was taken over by unrelated",
            "  work while it ran. Any wall-clock budget, timeout or p95 in this job",
            "  measured a contended machine. Do NOT read a timing failure here as a",
            "  renderer regression, and do NOT raise the budget to make it pass.",
        ),
        VERDICT_BUSY: (
            "  The GPU never became free within the preflight's wait, so this job never",
            "  had a machine to measure on.",
        ),
        VERDICT_UNMEASURED: (
            "  GPU occupancy could not be observed for this job, so contention can be",
            "  neither shown nor ruled out. An unobserved run is treated as void rather",
            "  than clean: absence of a signal is not a passing signal.",
        ),
    }[verdict]
    lines = banner([f"  {headline}", ""] + list(detail) + [""] + [f"  {r}" for r in series.reasons])
    for line in lines:
        print(line)
    for line in discriminator_note():
        print(line)
    annotate(
        "RUNNER BUSY - result void",
        headline
        + " "
        + " ".join(reason.strip() for reason in series.reasons)[:600]
        + " Retry on a free machine; this is not a renderer regression (#875).",
    )
    write_step_summary(
        [f"### {headline} (#875)", ""]
        + [f"- {reason.strip()}" for reason in series.reasons]
        + [
            "",
            f"Clean-run `frame_p95_to_avg_ratio` on the streaming lane is ~"
            f"{CLEAN_FRAME_P95_TO_AVG_RATIO:.2f} ({FRAME_RATIO_REFERENCE}); a ratio near 1.9 "
            "with residency 1.0 is contention, not a regression.",
        ]
    )
    return EXIT_RUNNER_BUSY


# --------------------------------------------------------------------------
# Phase: probe -- one-shot, for a human at a terminal
# --------------------------------------------------------------------------


def phase_probe(args: argparse.Namespace) -> int:
    sample = take_sample(None, args.busy_percent)
    print("RUNNER GPU CONTENTION -- one-shot probe")
    for line in describe_sample(sample, indent="    ", busy_percent=args.busy_percent):
        print(line)
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(sample.as_dict(), indent=2, sort_keys=True), encoding="utf-8")
        print(f"  record written to {args.json}")
    if not sample.usable:
        return EXIT_RUNNER_BUSY
    return EXIT_RUNNER_BUSY if sample.busy else 0


# --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument(
        "phase", choices=("preflight", "postflight", "sample", "probe")
    )
    parser.add_argument("--record-dir", type=Path, default=None)
    parser.add_argument("--json", type=Path, default=None)
    parser.add_argument(
        "--timeout-sec",
        type=float,
        default=None,
        help=f"bounded wait for a free GPU (default {DEFAULT_WAIT_TIMEOUT_SEC:.0f}s, "
        f"override with {ENV_WAIT_TIMEOUT})",
    )
    parser.add_argument("--poll-interval-sec", type=float, default=None)
    parser.add_argument("--sampler-interval-sec", type=float, default=None)
    parser.add_argument("--busy-percent", type=float, default=None)
    parser.add_argument("--job-root-pid", type=int, default=None)
    parser.add_argument("--max-lifetime-sec", type=float, default=SAMPLER_MAX_LIFETIME_SEC)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.timeout_sec is None:
        args.timeout_sec = _float_env(ENV_WAIT_TIMEOUT, DEFAULT_WAIT_TIMEOUT_SEC)
    if args.poll_interval_sec is None:
        args.poll_interval_sec = _float_env(ENV_POLL_INTERVAL, POLL_INTERVAL_SEC)
    if args.sampler_interval_sec is None:
        args.sampler_interval_sec = _float_env(ENV_SAMPLER_INTERVAL, SAMPLER_INTERVAL_SEC)
    if args.busy_percent is None:
        args.busy_percent = _float_env(ENV_BUSY_PERCENT, FOREIGN_GPU_BUSY_PERCENT)
    handlers = {
        "preflight": phase_preflight,
        "postflight": phase_postflight,
        "sample": phase_sample,
        "probe": phase_probe,
    }
    return handlers[args.phase](args)


if __name__ == "__main__":
    sys.exit(main())
