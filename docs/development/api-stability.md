# API stability

GodotGS is an alpha. **There is no API stability promise.**

Every GDScript-facing name this module registers may change or be removed in any
release, including a patch release, with no deprecation period and no migration shim.
That covers the **33 registered classes**, their methods, properties and signals, the
**two engine singletons** (`GaussianSplatManager` and `GaussianSplatSceneDirector`), and
all **193 project settings** under `rendering/gaussian_splatting/`.

It also covers the on-disk formats. A project saved or imported with one release may not
open, may not import, or may not render the same way under the next.

This is a deliberate choice, not an oversight. A bounded public surface with a promise we
keep is a v1.0 deliverable; promising stability over a surface this size today would be a
promise we could not keep, and a broken promise is worse than none.

## What `publicness` in the settings manifest means right now

`modules/gaussian_splatting/config/project_settings_manifest.json` carries a `publicness`
field per setting. Most settings do not set it directly — it is inherited from the longest
matching family prefix, and 21 of the 23 families default to `public`. So of the 193
settings, **43 carry the field literally and 153 resolve to `public`**; only the `debug/`
and `lighting/` families inherit anything else.

**During the alpha that classification is an in-progress exercise and confers no stability
promise.** 139 of the 193 are recorded as `test_coverage: inventory_only` — present in the
inventory with nothing verifying they do anything. Classifying all 193 explicitly, and
giving every `public` one behavioural coverage, is v1.0 work.

Read the `publicness` field as "which bucket this setting is expected to land in", not as
"this setting is supported".

## What to do about it

- **Pin the release you build against.** Do not track nightlies in a project you care
  about.
- **Keep the source `.ply` for every asset you import.** Re-importing is the supported way
  to move an asset across releases; the cache sidecar is not a migration format.
- **Expect to fix scripts when you upgrade.** Breaking changes are listed in the release
  notes; there is no compatibility layer.

If you need a stable surface, wait for v1.0. The shape of the promise it will carry is
described in [the release acceptance bar](../governance/release-acceptance-bar.md), §7.

## Related

- [Known public alpha limitations](known-public-alpha-limitations.md) — what ships broken,
  deliberately.
- [Compatibility matrix](../reference/compatibility-matrix.md) — which platforms are
  validated, and how far.
- [Project settings reference](../reference/project-settings.md).
