# Real SH basis evaluation through degree four

Status: proposed for v1; R3 rendering mathematics, human merge disposition required.

## Problem

The signed GPU storage preserves 24 non-DC terms. The active binning evaluator
still stops at degree three. Its degree-three terms 11 and 13 reverse the
Condon-Shortley sign, and term 14 doubles the normalized coefficient compared
with the Inria reference. Higher-order colour cannot be qualified with this basis.

## Decision

Use one pure polynomial GLSL include for the 25 real normalized basis functions
ordered by degree, then m=-l..l, with the Condon-Shortley phase. Unit directions
are a caller precondition. Compute only requested degrees and zero the remaining
terms. The active binning evaluator consumes it and caps packed accesses by the
encoded word count and physical capacity. The full degree-four math is available
without changing the current SH3 default or public quality/LOD selection in this
focused change; enabling SH4 throughout selection is the next dependent task.

Compile the same polynomial source into native tests. Validate every term against
an independent associated-Legendre recurrence, including poles and deterministic
spherical directions, all degree cutoffs, and independent analytic SH3 regressions.
This verifies the actual production polynomials on CPU, not GPU execution.

Reference: [Inria SH evaluator](https://github.com/graphdeco-inria/gaussian-splatting/blob/main/utils/sh_utils.py).
Mathematical polynomials are implemented from the normalized recurrence; the
reference is used for convention/ordering verification, not vendored source.

## Compatibility and evidence

No record, file format, or public API layout changes. SH3 images intentionally
change where the defective terms contribute. Require shader matrix compilation,
native recurrence tests, meaningful mutations, two independent fixed-diff reviews
and uncontended GPU/real-scan comparison before merge. Keep existing thresholds.
No speed, VRAM, or visual-quality superiority claim follows from CPU agreement.

## Rollback

Revert basis/evaluator changes together and rebuild shaders. Preserve full stored
SH data. Keep the mathematical regression tests as evidence of the unresolved bug.
