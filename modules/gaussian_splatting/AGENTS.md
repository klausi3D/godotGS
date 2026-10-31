# AGENTS.md — `modules/gaussian_splatting`

Refines the root [`AGENTS.md`](../../AGENTS.md) for the Gaussian Splatting module.
For orientation, use the subsystem map in `ARCHITECTURE.md` (read the section you
need). `READING_ORDER.md` is an onboarding map: open only the entries your task
touches, because the files it lists add up to more than 1 MB. The lifetime and
ownership rules below are mandatory and recur in CI guards.

## Ownership and lifetime

- **`GaussianData` and GPU resources have a single owner.** A resource's owner
  creates it and is the only code that frees it. Do not free a resource you did
  not create; do not hand a raw RID/buffer to code that will outlive the owner.
- Every RID/buffer/uniform-set freed on teardown must also be freed on **every
  early-return and failure path**. The repo's lifetime-accounting guards exist
  because partial init used to leak — see `docs/architecture/renderer-lifetime-ownership.md`.
- Prefer the existing managers (e.g. the GPU buffer manager) over ad-hoc resource
  handles. New long-lived GPU state must have a documented free path.

## Threading and locking

- State touched from both the main thread and worker/streaming threads must be
  guarded by the established lock for that subsystem; do not invent a second lock
  over the same data. Document the lock that protects any new shared field.
- Never block the main thread on a GPU readback. Use the existing async-readback
  path; report timing honestly (see renderer `AGENTS.md`).

## Errors and partial failure

- Initialization is all-or-nothing: on failure, release everything acquired so
  far and surface the error — never leave a half-initialized node "running".
- No silent fallbacks that change visual/semantic output without telling the
  caller. A degraded path must be explicit and observable.

## Serialization and back-compat

- On-disk formats live in `persistence/` (scene serialization, incremental
  saver) and loaders/importers in `io/` (`.ply`, `.spz`, `.gsplatworld`).
- **Bumping a layout/format version is a breaking change** (risk class R3): keep
  read paths able to load the previous version, add the version to the serialized
  contract, and validate it on load. Do not change a format silently.

## Tests

- Guard + structural checks: `python tests/ci/run_module_tests.py --guard-only`.
- Full module tests need a built Godot binary: `python tests/ci/run_module_tests.py --godot-binary <bin>`.
- Renderer/streaming behavior is exercised by `tests/runtime/` — see `tests/AGENTS.md`.
- See the full command set in `docs/reference/build-test-ci.md`.

Sub-areas with their own rules: [`renderer/`](renderer/AGENTS.md),
[`shaders/`](shaders/AGENTS.md).
