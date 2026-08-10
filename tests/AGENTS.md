# AGENTS.md — `tests`

Refines the root [`AGENTS.md`](../AGENTS.md) for the test and CI harness.
Full command reference: `docs/reference/build-test-ci.md`.

## Layout

- `tests/ci/` — guard checks, contract validators, baseline QA, GPU harness,
  and Python `unittest` tests (`test_*.py`). Entry point:
  `python tests/ci/run_module_tests.py` (`--guard-only` for the GPU-free lane).
- `tests/runtime/` — runtime validation harness, streaming/residency/GPU-stress
  GDScript scenarios, benchmark runner. Entry point:
  `python tests/runtime/run_runtime_validation.py --profile <profile>`.
- `tests/agentic/` — `unittest` tests for the `scripts/agentic` validators.

## Rules

- **Separate the tiers.** Structural/guard tests must stay deterministic and run
  without a GPU or a built binary where possible; runtime and performance tests
  are separate and may require hardware. Do not fold a hardware-dependent check
  into the guard lane.
- **Deterministic fixtures.** Use fixed seeds and the synthetic-asset helpers
  (`tests/runtime/prepare_synthetic_assets.py`); do not depend on wall-clock,
  network, or machine-specific paths. Real-scan visual validation is required for
  rendering-math changes but lives in its own lane, not the unit tests.
- **`REQUIRE` does not abort in this build.** `disable_exceptions` defaults to
  `True`, so both `tests/SCsub` and `modules/gaussian_splatting/SCsub` define
  `DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS`; doctest's abort path
  (`throwException()`) compiles to nothing and `REQUIRE` becomes a louder `CHECK`.
  So `REQUIRE(ptr != nullptr); ptr->f();` does not fail one case — it crashes the
  whole test binary and every case after it never runs. The same is true of
  `CHECK(ptr != nullptr); ptr->f();` in *every* build, and of the `WARN` family;
  the guard reads the accepted macro names out of doctest's own header, so all
  three spellings are covered. Guard a precondition explicitly instead:

  ```cpp
  if (!ptr) {
      FAIL("what was missing and why the case cannot continue");
      return;
  }
  ```

  The same applies to a **cardinality** precondition. `LocalVector::operator[]`
  and `CowData::get` abort unconditionally, so `REQUIRE(v.size() == 2); v[0];`
  kills the process before doctest prints its summary — the batch then reports
  `cases=0/0`, not a red test (#844, measured on #843). `CHECK` is worse: it
  never aborts under *any* doctest configuration. Guard those explicitly too:

  ```cpp
  if (v.size() != 2) {
      FAIL("expected 2 entries, got ", v.size());
      return;   // or an `else` branch, where independent assertions follow
  }
  ```

  `tests/ci/check_require_null_deref.py` carries two detectors — the null-ish
  one and the size-then-index one (#844) — but both are deliberately narrow (see
  its docstring): neither catches a dereference through an alias, and the
  size detector's window is a few statements. Write the guard, do not rely on
  the checker.
- **A green check must be able to fail.** Governing rules and the catalogue of
  recurring shapes: [evidence integrity](../docs/governance/evidence-integrity.md).
  In this directory specifically:
  - **Mutation-prove it.** Revert the fix, show the test goes RED, restore, show
    GREEN. Include a mutation that deletes the guard's **wiring**, not only its
    logic — a check invoked by no lane is the most common way one becomes
    decorative. This repo has shipped an enforced-but-unmatchable regex, and a
    pin test that ran in no lane.
  - **Never hand-author a fixture that claims to model a real producer's
    output.** Capture it from that producer. A parser/format contract test whose
    input was invented certifies a fiction and reads as proof the guard works —
    this repo shipped a skip-marker gate whose only test fabricated a column-0
    marker doctest never emits. This applies to *producer-format* fixtures only:
    synthetic corpora built by `tests/runtime/prepare_synthetic_assets.py`,
    malformed-input cases, and other constructed data remain the correct choice,
    because there the deterministic construction *is* the point. The test is
    whether the fixture asserts something about a format someone else produces.
  - **A ratchet compares against an immutable reference outside the change.**
    Not `HEAD` (in CI, `HEAD` *is* the change), not the document being checked.
    Fail closed when the reference cannot be resolved.
  - **Derive coverage lists; enumerate only policy.** A list that *describes*
    what exists — macro names, fields, lanes, workflow events — must be derived;
    hand-maintained ones drift. A list that *decides* policy, like
    `REQUIRED_BATCHES` in `tests/ci/run_gpu_harness.py`, is the contract itself
    and must stay explicit — deriving it would let the corpus redefine what is
    required. Ask whether a new item means the list is stale, or means someone
    must decide.
  - **Assert properties, not mechanisms.** *"No route can increase the set"*
    outlives *"this flag refuses"*.
  - **Say which mode a result came from.** Enforcement often sits behind
    `_is_ci()`; a local green can say nothing about CI.
- **No unjustified baseline/threshold updates.** Never edit a golden baseline,
  performance threshold, or release-gate manifest just to make a test pass.
  Changing a baseline requires its own justification and review; treat it as a
  contract change, not a fix.
- **No generated artifacts in commits.** Reports, logs, `output/`, captured
  images, and `__pycache__` are build outputs, not source — keep them untracked.
- **Document what you ran.** PRs must list the exact test commands and their
  results. Where GPU/Windows hardware was unavailable, write "not run", never
  "passed".
- Python tests use the standard-library `unittest` style already in the repo
  (e.g. `tests/ci/test_renderer_release_gates.py`): load the module under test
  with `importlib.util.spec_from_file_location` and use `tempfile` fixtures. Do
  not add a new test-framework dependency.
