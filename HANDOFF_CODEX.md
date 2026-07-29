# Handoff — GodotGS renderer, session 2026-07-29

For the next agent (Codex or otherwise). Written to be actionable without the
transcript. **Everything marked VERIFIED was executed and observed; everything
marked ASSUMED was not.** That distinction is the single most important habit
in this repo — see §7.

---

## 0. START HERE

- **master**: `a9e28990daa`
- **Repo rules**: `AGENTS.md`, then the nearest nested `AGENTS.md`. Process:
  `docs/governance/`. Build/test: `docs/reference/build-test-ci.md`.
- **One PR is open and mine**: #758, §1. Everything else merged.
- **Roadmap tracks are now labels**: `track:T0-proof-spine` (10 open),
  `track:T1-release-critical` (6), `track:T2-perf` (5). Filter with
  `gh issue list --label track:T0-proof-spine`. They were added as labels, not
  milestones, because an issue can hold only ONE milestone and most already sit
  in *Architecture Audit 2026-07-17* — assigning track milestones would have
  silently removed them from it.

---

## 1. In flight

### PR #758 — make the .gsplatworld importer copy crash-atomic (#714)
Branch `gs/p0-714-gsplatworld-atomic-copy`, worktree `C:/Projects/wt-p0`.
Last of the Phase-0 batch; the other six (#753/#754/#755/#756/#757/#759) merged.

**State (VERIFIED)**: all real checks green (`qlty` fails as always, non-blocking
per #775). **BLOCKED on unresolved threads** — Codex has now run **six review
rounds and found a genuine defect in every one**, five of them in fixes I wrote.

The rounds, in order, each one uncovering the next layer:
1. Doc contradiction — coverage text said the copy was atomic while `Known gaps`
   still said it was not. Fixed.
2. **Windows regression**: `_get_thread_file()` caches `FileAccess::READ` handles
   and `file_access_windows.cpp:217` opens via `_wfsopen` with
   `_SH_DENYNO`/`_SH_DENYWR`/`_SH_DENYRW` — **none grant `FILE_SHARE_DELETE`** —
   so a cached reader blocks both `MoveFileExW` and the backup-swap rename. The
   old truncating write never needed delete access. Added a live-source registry
   + handle release.
3. Releasing then dropping the lock only *narrowed* the window; readers reopened
   mid-copy. Added `ScopedReaderSuspend` holding `file_mutex`, scoped to the
   rename only via a new optional third argument to `gs_atomic_file_write()`.
4. Holding `file_mutex` across the drain was **self-defeating**:
   `capture_chunk_snapshot()` calls `_record_io_counters()` (which takes
   `file_mutex`) while its `Ref` is still alive, so the reader could never finish
   and the drain was *guaranteed* to time out. Replaced with a lock-free
   `SafeFlag`.
5. Check-then-act race: the flag was sampled before taking `file_mutex` and never
   rechecked. Now rechecked under the mutex.

**Residual, and it is inherent**: a reader that never finishes cannot be replaced
under, because Godot never opens `FileAccess` with `FILE_SHARE_DELETE`. Closing
that means moving `FileAccessWindows` onto `CreateFileW` — upstream engine
surface, highest risk class. Out of scope here.

**Owner instruction (2026-07-29)**: keep iterating until Codex is quiet.

---

## 2. What this session established that is worth not re-deriving

### #787 was misdiagnosed, and is now fixed and verified in production
Filed as `STATUS_STACK_BUFFER_OVERRUN` with fixed-size stack arrays suspected.
It is neither. VERIFIED:

- Fault RVA `0x171016a` maps byte-exactly onto the `cd29` (`int 29h`) fastfail
  opcode in `VectorWriteProxy<PackedGaussian>::operator[]`, `vector.h:54` —
  `CRASH_BAD_INDEX`, i.e. an out-of-bounds **write index**.
- WER `P9 = 7` = `FAST_FAIL_FATAL_APP_EXIT` (what `GENERATE_TRAP()` emits). A
  stack-cookie trip would be `2`. `0xC0000409`'s name and the `BEX64` bucket are
  Windows' generic labels, not evidence.
- Cause: `CowData::resize()` returns `ERR_OUT_OF_MEMORY` **without changing
  `size()`**, and every pack site ignored the return then wrote `dst.write[i]`.
- `CRASH_BAD_INDEX` is NOT gated on `DEBUG_ENABLED`, so it traps in every build
  — never a silent corruption.
- Fixed in #793. **The nightly Release-CI lane is now `10 passed / 0 failed`,
  with `GPU Streaming Stress (ok) in 159.7s`** where it previously died at
  `exit=3221226505` after 134.4 s.

Technique worth reusing: symbolize a crash RVA against a *different* build's PDB
with `cdb -c "ln <mod>+<rva>; u <mod>+<rva>"`, and **validate the mapping by the
instruction it lands on**. If it lands on the opcode the crash class implies, the
`.text` layout matched.

### #763 cannot be armed as written — measured, not argued
The issue says to calibrate the frame-scaling gate on an optimized build. Both
premises fail (VERIFIED, 5 clean runs on an idle machine, optimized
`speed_trace` build):

- **No CI lane builds optimized.** The only `optimize=` in `.github/workflows/`
  is a Linux fast-math *guard*; every lane running the stress scenario is
  `dev_build=yes`, i.e. **-O0**.
- **On an optimized build the tiers do not separate.** Mean p95: `tier_250k`
  263.4 ms, `tier_1m` 271.2 ms, `tier_2_5m` 257.2 ms. A 10× splat-count increase
  moves p95 by nothing; 2.5M is on average *faster*. 4 of 5 runs returned
  `inconclusive` with a **negative** marginal cost.

So the cited "~4.97× separation" was a -O0 artifact: at -O0 per-splat CPU work
dominates, and optimizing it away removes the signal. **The gate measures the
debug build's CPU cost, not renderer scaling.** Armed unchanged it has 10.2×
headroom over the observed max — it would not catch a 10× regression.

Full evidence is on issue #763. **Owner decision (2026-07-29): change the
signal** — gate on a per-splat GPU quantity (sort/bin time) that scales in both
build configurations. Not yet started.

### Perf-gate failures are partly configuration, not only contention
Optimized runs sit at 250-290 ms against the 325 ms `tier_1m` ceiling; the -O0
CI runs that keep failing sit at 337-394 ms. The absolute ceiling is tuned to the
-O0 envelope too. Contention is real (identical binaries measured 220-344 ms by
machine load alone) but it is not the whole story.

---

## 3. Traps that cost real time

- **This box runs TWO `Runner.Listener` processes**, so GPU QA / GPU Harness /
  Module Build can overlap. Always check `Get-Process Runner.Worker` before any
  perf or GPU measurement. I contaminated one p95 run by ignoring this.
- **A repo-wide git breakage**: a WSL-written `core.worktree` in the *shared*
  `.git/config` made every git command fail with `Invalid path '/mnt'`, including
  `git config --global --list` — which makes it look like an installation
  problem. It is repo-local and cwd-dependent. It also silently emptied the
  build's version hash. Repaired this session.
- **Backticks in a `git commit -m` string** get shell-substituted and silently
  eat text. Use `-F <file>`. (Heredocs with long markdown also break here.)
- Verify against `origin/master`, never a worktree — the main checkout was 4
  commits behind for most of this session.

---

## 4. What to do next, ranked

1. **#758** — keep answering Codex rounds until quiet (owner instruction).
2. **#763** — implement the changed signal (per-splat GPU quantity). This is
   Sprint-1 step 0's blocker: until it lands, the only required merge check
   (`agentic-pr-gate`) still runs **zero GPU/visual evidence**, so every oracle
   and golden landed this week protects less than it appears to.
3. **#794** — 16 remaining `unchecked resize() -> write[]` sites, derived list in
   the issue. Same class as #787.
4. **#790 remainder** — enforcement gaps a-d. Item (c) turns 20 lanes red; the
   owner wants to be asked before it is armed.
5. Tier-1 gates #519 / #521 / #523 / #778.

---

## 5. Build discipline

- Optimized build for perf work:
  `python -m SCons platform=windows target=editor dev_build=yes tests=yes gs_native_arch=no optimize=speed_trace -j12`
  — 18m56s cold, 160.8 MB binary. Plain `dev_build=yes` is **-O0** and inflates
  CPU roughly 12×.
- **Do NOT pass `cache_path`** for a cold build here; `C:/godotgs-scons-cache` is
  already at its 50 GB cap and a differing-flags build would churn it.
- Check free disk before starting. It swung from 8.9 GB to 104 GB this session as
  the CI runner cleaned its `_work` tree.
- Warm worktrees give ~30 s incremental builds — mutation proofs are cheap.

---

## 6. Useful commands

```bash
# guard gate that must pass before pushing (static, no binary needed)
python tests/ci/run_module_tests.py --guard-only

# the streaming stress scenario, exactly as CI runs it
bin/godot.windows.editor.dev.x86_64.exe --render-thread safe \
  --display-driver windows --rendering-driver vulkan \
  --script tests/runtime/test_gpu_streaming_stress.gd

# roadmap tracks
gh issue list --label track:T0-proof-spine
```

---

## 7. How this repo expects you to work

- **Mutation-prove every fix.** Green → neutralise → must go **RED with values**
  → restore → green. #787's proof reproduced the exact production crash on
  demand: `EXITCODE=-1073740791` with `FATAL: Index p_index = 0 is out of bounds`
  at `vector.h:54`.
- **Do not trust issue text.** #787's stated cause was wrong; #763's premise was
  wrong; #522/#790 are broader than the PRs that closed part of them. Verify
  against `origin/master` first, every time.
- **Fail-closed is not enough on its own — check EVERY caller.** #793's first
  revision made the pack functions fail closed, but three of four callers then
  consumed the empty result: one handed `buffer_update()` a null pointer with a
  nonzero byte count. The fix was `[[nodiscard]] bool`, which turns "did every
  caller handle this?" into a compile error.
- **A green test that asserts nothing is the dominant failure mode here.**
  `test_runtime_validation_proof_contract.py` existed but was wired into no lane
  and had never executed; adding tests to it would have been vacuous. It is now
  enrolled in `--guard-only`.
- **Never weaken a guard, baseline or threshold to make a check pass** — and
  never *arm* one on uncalibrated numbers either. See #763.
- **Reviewers do not implement; humans own the merge.** Every Codex thread must
  be answered *and* resolved before merge.
