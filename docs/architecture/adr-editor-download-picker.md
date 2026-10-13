# ADR: Direct editor downloads on the documentation site

- **Status:** Proposed; recorded before implementation.
- **Risk class:** R3. The download picker executes JavaScript in readers' browsers
  (`docs/assets/javascripts/latest-nightlies.js`).
- **Scope:** Download-page components and instructions, with entry links from the
  README and Quick Start. Release packaging and publication are unchanged.

## Context

Trying godotGS requires a fork editor. The download page first sends readers to
GitHub release assets; its live table uses archive extensions as download labels
and mixes editors with export templates. Readers must identify an executable
package before they can follow the tutorial. The newest nightly can lack a
Windows build, so the newest available editor must be selected per platform.

The current Windows package also lacks the console wrapper described by the
page ([#829](https://github.com/klausi3D/godotGS/issues/829)). Download instructions
must describe the published GUI executable without promising a missing file.

## Decision

Present Windows and Linux editor downloads first, as compact platform cards with
named direct archive links. Show architecture, archive type, size, publication
date, release tag and checksum alongside the action. Keep the Linux `-O0` warning
at the action and retain the complete build-flavor warning in the page.
Show extract-and-run instructions next. Export templates remain a separate,
secondary action in the exporting section; macOS directs readers to a source
build. The README links to the rendered download page, where the picker works.

Reuse the existing release-selection, credential-free API request, ten-minute
session cache and Material instant-navigation lifecycle. Preserve the repository
download-URL restriction and create API-derived text with `textContent`, never
HTML. Render no actionable archive link for a rejected URL. A cached list is
identified as cached. Missing platforms, newer releases without their binaries,
API failure and disabled JavaScript retain explicit manual-download guidance.
Validate the parsed, decoded path as well as the literal repository prefix:
the existing prefix-only check accepts dot-segment paths that a browser resolves
outside the repository. Reject traversal, ambiguous encoded separators and
malformed escapes before rendering any editor, checksum or template link.
Do not redirect based on OS detection or use GitHub's latest-release shortcut,
which excludes the nightly prereleases.

## Validation and review

Compare rendered before/after pages at desktop, narrow and mobile widths, in
light and dark themes and at native 200% browser zoom. Exercise live downloads,
cached results, older platform builds, missing assets, rejected URLs, API errors,
timeout, disabled JavaScript and instant navigation. Check keyboard focus and
page overflow, alongside the strict docs build and existing download guards.
Verify published archive contents and checksums independently of CI job labels.

R3 requires two independent reviews plus CODEOWNER and human approval. The
policy's generic runtime/GPU check remains a requirement; unavailable hardware
is reported as not run, with no claim of renderer or performance validation.
Local screenshots and test reports stay outside the repository.

## Alternatives and rollback

Static links to today's nightly would go stale. A CSS-only treatment would leave
extension-only accessible labels and the editor/template ambiguity. Changing
release automation is a separate task. Revert this scoped change to restore the
existing live table and manual instructions; release assets are unaffected.
