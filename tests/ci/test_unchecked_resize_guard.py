"""Self-tests for tests/ci/check_unchecked_resize.py.

Every case here corresponds to a way the guard could be EVADED -- i.e. a way a new
unchecked `resize()` feeding a raw write could be added while the ratchet still
passed. A guard with no self-test is an assertion about the codebase that nobody
checks; this file makes each evasion a failing test instead.

Each case is written so it FAILS against the pre-fix guard:

  test_distinct_functions_get_distinct_keys      <- key omitted the function name
  test_regenerate_refuses_to_add_a_key           <- --regenerate used a NET delta
  test_function_scope_is_not_window_truncated    <- MAX_FUNCTION_SPAN = 200 cap
  test_multiline_resize_is_detected              <- line-anchored regex
  test_unreadable_source_fails_the_guard         <- `except OSError: continue`

plus a second review round, where three more evasions were reported against the fixed
guard -- all of them the same root cause, the guard parsing C++ with a regex and a raw
brace count, so ordinary formatting defeated it:

  test_trailing_comment_does_not_hide_a_site     <- RESIZE_RE required ';' at end of line
  test_block_comment_does_not_hide_a_site        <- ditto, /* */ form
  test_brace_in_a_comment_does_not_end_the_span  <- a '}' in a comment truncated the span
  test_brace_in_a_string_does_not_end_the_span   <- ditto, inside a string literal
  test_comment_only_consumer_is_not_a_consumer   <- a ptrw() in a comment read as code
  test_swapped_duplicate_is_reported_as_new      <- an occurrence ordinal COUNTS sites, it
                                                    does not identify them, so fixing one
                                                    duplicate while adding another was
                                                    invisible

Measured, not assumed: those six FAIL against the pre-fix guard (29f4df42603) and pass
against the fixed one. Two further cases here are NOT evasion tests and pass on both
sides -- said plainly so nobody reads them as proof of a fix:

  test_duplicate_sites_have_stable_identities    invariant: duplicates stay distinct.
                                                 The ordinal already satisfied it, which
                                                 is exactly why counting looked adequate.
  test_unique_keys_carry_no_context_suffix       confines the new context digest to real
                                                 duplicates, so it cannot churn the 80
                                                 unique baseline keys.

plus two regression tests for defects found while fixing the above:

  test_consumer_does_not_leak_across_functions   <- namespace-as-span over-scanned
  test_namespaced_functions_resolve_by_name      <- `namespace {` swallowed a file

and a third round, five more evasions reported against the masked/context-keyed guard.
Three are the same shape yet again -- a BOUND on how far the scanner looks, which reads
as "found nothing" when it means "stopped looking":

  test_prefixed_char_literal_does_not_hide_a_site  <- U'}' read as a digit separator
  test_declaration_beyond_the_old_lookback_is_resolved  <- 70-line type lookback
  test_long_statement_is_assembled                 <- 12-line statement assembly cap
  test_unterminated_resize_fails_the_guard         <- ...and the residue must fail closed
  test_fixing_one_duplicate_keeps_the_survivor     <- a repair read as a NEW site, and
                                                      --regenerate refused to record it,
                                                      so the ratchet could not shrink
  test_identical_blocks_are_separated_without_an_ordinal  <- the ordinal that survived
                                                      round 2 still renumbered with its
                                                      group, so fix-one-add-one passed

and the P1 that made all of the above conditional -- the baseline was read out of the
commit being graded, so a change could add a site and its own key together:

  test_baseline_growth_relative_to_base_is_rejected
  test_growth_without_a_detector_change_is_rejected_outright
  test_ledger_entry_without_a_reason_is_rejected
  test_unresolvable_review_base_fails_closed
  test_documented_detector_change_is_accepted     <- the sanctioned route must WORK, or
                                                     it gets bypassed rather than obeyed
  test_baseline_absent_at_base_is_reported_not_failed

Measured: all fourteen of the round-3 cases (including the tightened assertion in
test_swapped_duplicate_is_reported_as_new) are RED against 2642f8405fc and green here.
One control passes on both sides and is named as such:

  test_digit_separators_are_still_not_char_literals  pins the OTHER direction of the
                                                     masking decision above.

and a fourth round on what a base-key claim may consume -- the fallback that lets a
repaired duplicate group shrink matched on the base key ALONE, with nothing checking
that a group was there to shrink:

  test_a_lone_recorded_duplicate_cannot_be_claimed_by_base_key  <- a single recorded
                                                      `k@d1` was consumed by a plain
                                                      `k`, and `no_longer_present` came
                                                      back empty too, so the site left
                                                      no trace in the output at all
  test_two_plain_sites_cannot_each_claim_a_duplicate_entry  <- two plain sites consumed
                                                      one recorded entry each
  test_a_replaced_duplicate_group_is_reported_not_silent  <- repairing the WHOLE group
                                                      and adding a site at the same base
                                                      key is indistinguishable from a
                                                      real repair, so it is reported
                                                      rather than decided silently

Measured: those three are RED against 1c71501dba6 and green here. Its control passes on
both sides and is named as such:

  test_an_exactly_matched_run_reports_no_claim       keeps the new note attached to the
                                                     undecided case; pre-fix it passes
                                                     trivially, since no note existed.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest

GUARD_PATH = pathlib.Path(__file__).resolve().parent / "check_unchecked_resize.py"


def _load_guard():
    spec = importlib.util.spec_from_file_location("check_unchecked_resize", GUARD_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules["check_unchecked_resize"] = module
    spec.loader.exec_module(module)
    return module


class UncheckedResizeGuardTest(unittest.TestCase):
    def setUp(self):
        self.guard = _load_guard()
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        (self.root / "modules" / "gaussian_splatting").mkdir(parents=True)
        self.guard.REPO_ROOT = self.root
        self.guard.MODULE_ROOT = self.root / "modules" / "gaussian_splatting"
        self.guard.BASELINE_PATH = self.root / "baseline.json"
        self.addCleanup(self._tmp.cleanup)

    def _write(self, name: str, body: str) -> None:
        (self.guard.MODULE_ROOT / name).write_text(body, encoding="utf-8")

    def _sites(self) -> list[str]:
        sites, errors = self.guard.find_sites()
        self.assertEqual(errors, [], "fixture sources must all be scannable")
        return sites

    def _main(self, *argv: str) -> int:
        saved = sys.argv
        sys.argv = ["check_unchecked_resize.py", *argv]
        try:
            return self.guard.main()
        finally:
            sys.argv = saved

    # -- P1: key collision -------------------------------------------------
    def test_distinct_functions_get_distinct_keys(self):
        """Two statements identical except for their enclosing function.

        Pre-fix the key was file::symbol.resize(count), so these collapsed to ONE
        entry: fixing one, or adding the other, left the key set unchanged and the
        ratchet passed silently. Measured on the real tree, this folded 80
        statements into 69 keys.
        """
        self._write("a.cpp", """
Error First::build() {
    Vector<float> result;
    result.resize(splat_count);
    float *w = result.ptrw();
    return OK;
}

Error Second::build() {
    Vector<float> result;
    result.resize(splat_count);
    float *w = result.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 2, f"expected two independently-tracked sites, got {sites}")
        self.assertEqual(len(set(sites)), 2, f"keys collapsed onto each other: {sites}")

    # -- P1: regenerate blessing a new site --------------------------------
    def test_regenerate_refuses_to_add_a_key(self):
        """Fixing one site while adding another nets zero.

        Pre-fix, --regenerate wrote the baseline unconditionally and only compared
        len(), so the net-zero case printed no warning at all and returned 0 -- and
        the committed baseline then made normal CI pass over a real new defect.
        """
        self.guard.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 2, "sites": ["modules/gaussian_splatting/old.cpp::Gone::v.resize(n)"]}),
            encoding="utf-8",
        )
        before = self.guard.BASELINE_PATH.read_text(encoding="utf-8")
        self._write("a.cpp", """
Error Fresh::build() {
    Vector<float> values;
    values.resize(runtime_count);
    float *w = values.ptrw();
    return OK;
}
""")
        argv = sys.argv
        sys.argv = ["check_unchecked_resize.py", "--regenerate"]
        try:
            code = self.guard.main()
        finally:
            sys.argv = argv
        self.assertEqual(code, 1, "regeneration that ADDS a key must fail, not warn")
        self.assertEqual(
            self.guard.BASELINE_PATH.read_text(encoding="utf-8"),
            before,
            "a refused regeneration must not have written the baseline",
        )

    # -- P2: window truncation ---------------------------------------------
    def test_function_scope_is_not_window_truncated(self):
        """Consumer far below the resize, still inside the same function.

        Pre-fix a fixed 200-line cap was returned as the function end, so this site
        was dropped -- recreating the very window-length evasion that motivated
        scanning by function scope. The real defect that motivated it sat ~38 lines
        down; nothing stops an evader from using 250.
        """
        filler = "\n".join(f"    int pad{i} = {i};" for i in range(400))
        self._write("a.cpp", f"""
Error Wide::build() {{
    Vector<float> values;
    values.resize(runtime_count);
{filler}
    float *w = values.ptrw();
    return OK;
}}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a consumer 400 lines below must still be found: {sites}")

    # -- P2: multiline statement -------------------------------------------
    def test_multiline_resize_is_detected(self):
        """A normally wrapped call is one statement across two physical lines.

        Pre-fix the line-anchored regex never matched it, so the ptrw() below was
        never examined and the site passed the guard entirely.
        """
        self._write("a.cpp", """
Error Wrapped::build() {
    Vector<float> values;
    values.resize(
            runtime_count);
    float *w = values.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a wrapped resize() must still be detected: {sites}")

    # -- P2: unreadable source ---------------------------------------------
    def test_unreadable_source_fails_the_guard(self):
        """An unreadable source must fail, not silently shrink the scan.

        Pre-fix `except OSError: continue` dropped every site in that file and the
        run still exited 0 -- reporting the file's baseline entries as "fixed",
        which is an incomplete scan presented as evidence of safety.
        """
        missing = self.guard.MODULE_ROOT / "vanished.cpp"
        self.guard._module_sources = lambda: [missing]
        sites, errors = self.guard.find_sites()
        self.assertEqual(sites, [])
        self.assertTrue(errors, "an unreadable source must be reported, not skipped")

        self.guard.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 2, "sites": []}), encoding="utf-8"
        )
        argv = sys.argv
        sys.argv = ["check_unchecked_resize.py"]
        try:
            code = self.guard.main()
        finally:
            sys.argv = argv
        self.assertEqual(code, 1, "an incomplete scan must fail the guard")

    # -- P1 (round 2): comments hiding a statement -------------------------
    def test_trailing_comment_does_not_hide_a_site(self):
        """A trailing comment must not make an unchecked resize invisible.

        Pre-fix RESIZE_RE required the ';' to end the assembled statement, so any call
        with a trailing comment was dropped before the consumer was ever looked for.
        This is not a hypothetical formatting: it hid a live site at
        renderer/render_resource_orchestrator.cpp:267 whose ptrw() sits two lines below.
        """
        self._write("a.cpp", """
Error Commented::build() {
    Vector<uint8_t> vertex_data;
    vertex_data.resize(splat_count * 40); // 3 pos + 4 color + 3 scale
    uint8_t *w = vertex_data.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a trailing comment must not hide a site: {sites}")

    def test_block_comment_does_not_hide_a_site(self):
        """Same hole, /* */ form -- fixed by masking rather than by widening the regex."""
        self._write("a.cpp", """
Error Blocked::build() {
    Vector<uint8_t> payload;
    payload.resize(runtime_count); /* sized from the file header */
    uint8_t *w = payload.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a block comment must not hide a site: {sites}")

    # -- P2 (round 2): braces in comments and strings -----------------------
    def test_brace_in_a_comment_does_not_end_the_span(self):
        """A '}' inside a comment must not close the enclosing function early.

        Pre-fix the raw brace count closed the span at the comment; the resize still
        fell INSIDE that truncated span, so _consumer_scan_end() returned the premature
        end and the unbounded fallback -- the thing the docstring claimed made
        miscounting safe -- was never reached. The consumer below was simply lost.
        """
        self._write("a.cpp", """
Error Braced::build() {
    Vector<float> values;
    values.resize(runtime_count);
    // closing the loop below with } would be wrong
    float *w = values.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a brace in a comment must not truncate scope: {sites}")

    def test_brace_in_a_string_does_not_end_the_span(self):
        """Same hole via a string literal."""
        self._write("a.cpp", """
Error Stringy::build() {
    Vector<float> values;
    values.resize(runtime_count);
    print_line("}");
    float *w = values.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a brace in a string must not truncate scope: {sites}")

    def test_comment_only_consumer_is_not_a_consumer(self):
        """Masking must cut BOTH ways: commented-out code is not a raw write.

        The same masking that stops comments hiding a site also stops a commented
        `ptrw()` from inventing one. Asserted so a later "just strip // for matching"
        shortcut cannot quietly reintroduce the false positive.
        """
        self._write("a.cpp", """
Error OnlyInAComment::build() {
    Vector<float> values;
    values.resize(runtime_count);
    // float *w = values.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(sites, [], f"a consumer inside a comment must not count: {sites}")

    # -- P1 (round 2): duplicate identity ----------------------------------
    def test_duplicate_sites_have_stable_identities(self):
        """Two identical statements in one function must get DISTINCT identities."""
        self._write("a.cpp", """
Error Dup::build() {
    Vector<float> v;
    int head = 1;
    v.resize(n);
    float *a = v.ptrw();
    int tail = 2;
    v.resize(n);
    float *b = v.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 2, f"both duplicates must be tracked: {sites}")
        self.assertEqual(len(set(sites)), 2, f"duplicates collapsed onto one key: {sites}")

    def test_swapped_duplicate_is_reported_as_new(self):
        """Fix one duplicate, add another in the same function -> the ratchet must fire.

        This is the case an occurrence ORDINAL cannot see. Pre-fix the two sites were
        `key` and `key#2`; fixing the first and adding a third produced the SAME two
        keys, so the count and the exact set were unchanged and CI passed over a real
        new unchecked site. Identity has to come from context, not from counting.
        """
        both = """
Error Dup::build() {
    Vector<float> v;
    int head = 1;
    v.resize(n);
    float *a = v.ptrw();
    int tail = 2;
    v.resize(n);
    float *b = v.ptrw();
    return OK;
}
"""
        self._write("a.cpp", both)
        baseline = self._sites()

        swapped = """
Error Dup::build() {
    Vector<float> v;
    int head = 1;
    ERR_FAIL_COND_V(gs_resize_or_fail(v, n) != OK, ERR_OUT_OF_MEMORY);
    float *a = v.ptrw();
    int tail = 2;
    v.resize(n);
    float *b = v.ptrw();
    int extra = 3;
    v.resize(n);
    float *c = v.ptrw();
    return OK;
}
"""
        self._write("a.cpp", swapped)
        after = self._sites()
        self.assertEqual(len(after), 2, f"expected two live sites after the swap: {after}")
        new_sites, _ = self.guard.partition_sites(after, baseline)
        self.assertTrue(
            new_sites,
            f"the newly added duplicate must read as NEW against the baseline: "
            f"baseline={baseline} after={after}",
        )

        # The survivor is re-identified here too, and that is deliberate rather than a
        # regression against the round-2 assertion that it kept its identity. The
        # context window is three statements per side (CONTEXT_STATEMENTS), so the
        # in-place fix above the survivor and the block added below it are both INSIDE
        # its context -- one neighbour per side was not enough, because for repeated
        # blocks the immediate neighbours belong to the repeated block itself and match
        # by construction.
        #
        # It costs nothing in the case that actually matters. A plain FIX shrinks the
        # group to one, the survivor becomes a unique key, and partition_sites() matches
        # it against its recorded duplicate entry -- asserted in
        # test_fixing_one_duplicate_keeps_the_survivor. The survivor is only ever
        # re-identified when the group size was HELD UP by an addition, which is exactly
        # when the guard is supposed to fire.
        self.assertEqual(
            len(new_sites), 2,
            f"a rearranged duplicate group must be reported whole: "
            f"baseline={baseline} after={after} new={new_sites}",
        )

    def test_unique_keys_carry_no_context_suffix(self):
        """Context sensitivity must be confined to actual duplicates.

        A digest on every key would make an edit NEXT TO a normal site look like a new
        site, and --regenerate refuses to add keys, so that would deadlock the guard on
        an unrelated change. Only repeated keys carry the suffix.
        """
        self._write("a.cpp", """
Error Solo::build() {
    Vector<float> values;
    values.resize(runtime_count);
    float *w = values.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"expected one site: {sites}")
        self.assertNotIn("@", sites[0], f"a unique key must stay context-free: {sites}")

    # -- P2 (round 3): prefixed character literals --------------------------
    def test_prefixed_char_literal_does_not_hide_a_site(self):
        """`U'}'` is a char literal, not a digit separator.

        Pre-fix the mask asked only whether the character BEFORE the quote was
        alphanumeric, which is true of `1` in `1'000` and equally true of the `U`, `L`,
        `u` and `u8` prefixes. The literal was left unmasked, its `}` closed the
        function span early in _function_spans(), and the ptrw() below the resize was
        never reached.
        """
        self._write("a.cpp", """
Error Prefixed::build() {
    Vector<float> values;
    values.resize(runtime_count);
    char32_t brace = U'}';
    float *w = values.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a prefixed char literal must not truncate scope: {sites}")

    def test_digit_separators_are_still_not_char_literals(self):
        """The control for the case above: `1'000'000` must stay a number.

        Masking a digit separator as a literal would blank the rest of the line, so this
        pins both directions of the same decision rather than only the one that was
        broken.
        """
        self.assertFalse(
            self.guard._is_digit_separator("    char32_t brace = U'}';", 22),
            "a prefixed char literal must not read as a digit separator",
        )
        self.assertTrue(
            self.guard._is_digit_separator("    int limit = 1'000'000;", 21),
            "a digit separator must not read as a char literal",
        )

    # -- P2 (round 3): declaration lookback ---------------------------------
    def test_declaration_beyond_the_old_lookback_is_resolved(self):
        """A Vector declared far above its resize() must still resolve as CoW.

        Pre-fix the type lookback stopped after 70 lines and returned "unknown", which
        find_sites() DISCARDS -- so a long function could carry the exact unchecked
        allocation this guard ratchets and produce no finding at all. Same shape as the
        forward window that _function_spans() removed, pointing the other way.
        """
        filler = "\n".join(f"    int pad{i} = {i};" for i in range(120))
        self._write("a.cpp", f"""
Error Long::build() {{
    Vector<float> values;
{filler}
    values.resize(runtime_count);
    float *w = values.ptrw();
    return OK;
}}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a declaration 120 lines up must still resolve: {sites}")

    # -- P2 (round 3): statement assembly cap -------------------------------
    def test_long_statement_is_assembled(self):
        """A resize() call spanning more than a dozen lines is still one statement.

        Pre-fix MAX_STATEMENT_LINES = 12 returned nothing for it, the consumer below was
        never examined, and the guard passed with no signal that anything was skipped --
        the same silent-window defect one size larger.
        """
        terms = "\n".join(f"            + term{i}" for i in range(20))
        self._write("a.cpp", f"""
Error Wide::build() {{
    Vector<float> values;
    values.resize(runtime_count
{terms}
            );
    float *w = values.ptrw();
    return OK;
}}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a 20-line resize() call must be assembled: {sites}")

    def test_unterminated_resize_fails_the_guard(self):
        """A resize( that never closes is a PARSE failure, not an absence of findings.

        Removing the assembly cap leaves exactly one way to fail: input the scanner
        cannot parse. That must fail closed for the same reason an unreadable file does
        -- "could not look" and "looked and found nothing" are different answers.
        """
        self._write("a.cpp", """
Error Broken::build() {
    Vector<float> values;
    values.resize(runtime_count
    float *w = values.ptrw();
""")
        sites, errors = self.guard.find_sites()
        self.assertEqual(sites, [])
        self.assertTrue(errors, "an unassemblable resize() must be reported, not skipped")

        self.guard.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 2, "sites": []}), encoding="utf-8"
        )
        self.assertEqual(self._main(), 1, "an incomplete scan must fail the guard")

    # -- P2 (round 3): duplicate identity across a fix ----------------------
    def test_fixing_one_duplicate_keeps_the_survivor(self):
        """Fixing one of two duplicates must let the ratchet SHRINK.

        The suffix is attached only while a key repeats, so fixing one duplicate leaves
        a unique key that is emitted plain. Compared by exact string that read as a NEW
        site -- the guard failed on a repair -- and --regenerate then refused to record
        the replacement key, so the ratchet could not shrink at all.
        """
        both = """
Error Dup::build() {
    Vector<float> v;
    int head = 1;
    v.resize(n);
    float *a = v.ptrw();
    int tail = 2;
    v.resize(n);
    float *b = v.ptrw();
    return OK;
}
"""
        self._write("a.cpp", both)
        baseline = self._sites()
        self.assertEqual(len(baseline), 2, f"fixture must produce two duplicates: {baseline}")

        self._write("a.cpp", both.replace(
            "    v.resize(n);\n    float *a = v.ptrw();",
            "    ERR_FAIL_COND_V(gs_resize_or_fail(v, n) != OK, ERR_OUT_OF_MEMORY);\n"
            "    float *a = v.ptrw();", 1))
        after = self._sites()
        self.assertEqual(len(after), 1, f"one site must remain after the fix: {after}")
        new_sites, fixed = self.guard.partition_sites(after, baseline)
        self.assertEqual(new_sites, [], f"a repair must not read as a new site: {after} vs {baseline}")
        self.assertEqual(len(fixed), 1, f"the ratchet must record one fix: {fixed}")

    def test_identical_blocks_are_separated_without_an_ordinal(self):
        """Duplicates whose whole context matches must still be told apart.

        The residual an occurrence ordinal left behind: two blocks that are identical
        including their surroundings were keyed `k@d` and `k@d#2`, so fixing one and
        adding another renumbered to the same pair and the ratchet passed. The
        replacement discriminator is the statement OFFSET in the function, which the
        added block cannot share with the one it replaced.
        """
        block = """    for (int i = 0; i < 2; i++) {
        v.resize(n);
        float *w = v.ptrw();
    }
"""
        head = "\nError Dup::build() {\n    Vector<float> v;\n"
        self._write("a.cpp", head + block + block + "    return OK;\n}\n")
        baseline = self._sites()
        self.assertEqual(len(set(baseline)), 2, f"identical blocks must stay distinct: {baseline}")
        self.assertTrue(all("#" not in s for s in baseline), f"ordinal fallback used: {baseline}")

        fixed_block = block.replace(
            "v.resize(n);", "ERR_FAIL_COND_V(gs_resize_or_fail(v, n) != OK, ERR_OUT_OF_MEMORY);")
        self._write("a.cpp", head + fixed_block + block + block + "    return OK;\n}\n")
        after = self._sites()
        self.assertEqual(len(after), 2, f"two live sites expected: {after}")
        new_sites, _ = self.guard.partition_sites(after, baseline)
        self.assertTrue(
            new_sites,
            f"fixing one identical block and adding another must fire: "
            f"baseline={baseline} after={after}",
        )

    # -- P1 (round 4): what a base-key claim may and may not consume ---------
    def test_a_lone_recorded_duplicate_cannot_be_claimed_by_base_key(self):
        """A claim models a duplicate group SHRINKING. One entry is not a group.

        A suffix is emitted only while a key REPEATS in its file, so a baseline holding
        exactly one `k@d1` at that base key cannot have come from a scan of it -- there
        is no recorded sibling that could have been repaired. Pre-fix the plain scan
        site claimed it anyway, on the base key alone, and `no_longer_present` came back
        EMPTY too: the run printed "no new ones" and the site left no trace anywhere in
        the output.
        """
        new_sites, missing = self.guard.partition_sites(["k"], ["k@d1"])
        self.assertEqual(new_sites, ["k"], "a lone recorded duplicate is not a group to shrink")
        self.assertEqual(missing, ["k@d1"], "the unclaimed entry must still be reported")

    def test_two_plain_sites_cannot_each_claim_a_duplicate_entry(self):
        """The claim's premise is that the key now occurs ONCE. Enforce it.

        The plain form is emitted only for a key that is unique in its file, so a second
        scan entry sharing the base key contradicts the premise the claim rests on.
        Pre-fix nothing checked it and each plain site consumed one recorded entry, so
        two sites disappeared into a `([], [])` -- the quietest possible pass.
        """
        new_sites, missing = self.guard.partition_sites(["k", "k"], ["k@d1", "k@d2"])
        self.assertEqual(new_sites, ["k", "k"], "neither site is the sole entry at its base key")
        self.assertEqual(missing, ["k@d1", "k@d2"], "no entry may be consumed by an ambiguous claim")

    def test_a_replaced_duplicate_group_is_reported_not_silent(self):
        """Repair the WHOLE group and add a new site at the same base key.

        This is the residual the claim cannot decide: the scan is a single plain `k`
        against `[k@d1, k@d2]`, byte for byte identical to a legitimate repair of one
        duplicate. The context digest cannot separate them either -- repairing a sibling
        rewrites the survivor's context window, which is why the survivor is emitted
        plain in the first place. So the guard accepts the claim (rejecting it would
        fail every real repair) but must not accept it SILENTLY: pre-fix the entire
        output was "1 baseline entry no longer present", i.e. a report that the ratchet
        got BETTER, on a change that introduced an unchecked site.

        Note the narrower path is already covered and does fire: repairing one duplicate
        and adding another leaves two live sites at the key, so both are suffixed and
        both are reported (test_fixing_one_duplicate_adds_another).
        """
        both = """
Error Dup::build() {
    Vector<float> v;
    int head = 1;
    v.resize(n);
    float *a = v.ptrw();
    int tail = 2;
    v.resize(n);
    float *b = v.ptrw();
    return OK;
}
"""
        self._write("a.cpp", both)
        baseline = self._sites()
        self.assertEqual(len(baseline), 2, f"fixture must produce two duplicates: {baseline}")

        fix = "ERR_FAIL_COND_V(gs_resize_or_fail(v, n) != OK, ERR_OUT_OF_MEMORY);"
        self._write("a.cpp", """
Error Dup::build() {
    Vector<float> v;
    int head = 1;
    %s
    float *a = v.ptrw();
    int tail = 2;
    %s
    float *b = v.ptrw();
    int extra = 3;
    v.resize(n);
    float *c = v.ptrw();
    return OK;
}
""" % (fix, fix))
        after = self._sites()
        self.assertEqual(len(after), 1, f"the added site is the only live one: {after}")
        self.assertNotIn("@", after[0], "a unique key is emitted plain, which is what enables the claim")

        new_sites, missing = self.guard.partition_sites(after, baseline)
        self.assertEqual(new_sites, [], "undecidable: this is byte-identical to a real repair")
        self.assertEqual(len(missing), 1, f"one entry stays unclaimed: {missing}")

        # The assertion that fails against the pre-fix guard: the run must SAY that it
        # decided this by base key. Pre-fix its entire output was "1 baseline entry no
        # longer present" -- a report that the ratchet got better.
        self.guard.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 2, "sites": baseline}), encoding="utf-8")
        self._stub_base(baseline, detector_differs=False)
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = self._main()
        output = buffer.getvalue()
        self.assertEqual(code, 0, f"a shrunk group must still pass: {output}")
        self.assertIn("matched a recorded DUPLICATE entry by base key", output,
                      f"the claim was accepted silently: {output}")
        self.assertIn(after[0], output, f"the claiming site must be named: {output}")

    def test_an_exactly_matched_run_reports_no_claim(self):
        """Control: the note must DISCRIMINATE, not decorate every run.

        Passes on both sides of the fix and is named as such -- pre-fix trivially,
        because no note existed. Its job is to keep the note attached to the undecided
        case, since a warning printed unconditionally is one nobody reads.
        """
        self._write("a.cpp", """
Error Solo::build() {
    Vector<float> v;
    v.resize(n);
    float *w = v.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.guard.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 2, "sites": sites}), encoding="utf-8")
        self._stub_base(sites, detector_differs=False)
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = self._main()
        output = buffer.getvalue()
        self.assertEqual(code, 0, output)
        self.assertNotIn("by base key", output, f"an exact match is not a claim: {output}")

    # -- P1 (round 3): the baseline itself must be anchored -----------------
    def _stub_base(self, base_sites, detector_differs=True, base_sha="feedfacecafe"):
        """Answer the three git questions the base comparison asks, without a repo."""
        self.guard.resolve_review_base = lambda base_ref=None: (base_sha, [])
        self.guard.detector_differs_from_base = lambda sha: (detector_differs, [])

        def _blob(sha, path):
            if base_sites is None:
                return self.guard.ABSENT_AT_BASE, []
            return json.dumps({"schema_version": 2, "sites": base_sites}), []

        self.guard._blob_at_base = _blob

    def test_baseline_growth_relative_to_base_is_rejected(self):
        """Adding a site AND its baseline key in one change must not pass.

        The hole this closes: every check above reads the baseline out of the PROPOSED
        commit, so a change that writes the new key into the file it is grading itself
        against produces an empty new_sites and exits 0. Both sides moved together,
        which is not a ratchet.
        """
        self._stub_base(["modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)"])
        document = {"sites": [
            "modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)",
            "modules/gaussian_splatting/a.cpp::Fresh::build::values.resize(runtime_count)",
        ]}
        failures = self.guard.check_baseline_against_base(document)
        self.assertTrue(failures, "a baseline that grew against the review base must fail")
        self.assertIn("values.resize(runtime_count)", "\n".join(failures))

    def test_growth_without_a_detector_change_is_rejected_outright(self):
        """The ledger cannot license growth when the DETECTOR did not change.

        A reason is self-attested and only a reviewer can grade it; "this script differs
        from the base" is a fact git settles. It denies the whole route to a change that
        touches no detector code, which is what stops the ledger being a second way to
        write anything into the baseline.
        """
        self._stub_base(["modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)"],
                        detector_differs=False)
        document = {
            "sites": [
                "modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)",
                "modules/gaussian_splatting/a.cpp::Fresh::build::values.resize(n)",
            ],
            "detector_change_ledger": [
                {"site": "modules/gaussian_splatting/a.cpp::Fresh::build::values.resize(n)",
                 "reason": "claims a widened predicate"},
            ],
        }
        failures = self.guard.check_baseline_against_base(document)
        self.assertTrue(failures, "a ledger entry must not license growth on an unchanged detector")
        self.assertIn("byte-identical", "\n".join(failures))

    def test_documented_detector_change_is_accepted(self):
        """The one sanctioned route must actually work, or it gets bypassed."""
        self._stub_base(["modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)"])
        document = {
            "sites": [
                "modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)",
                "modules/gaussian_splatting/a.cpp::Fresh::build::values.resize(n)",
            ],
            "detector_change_ledger": [
                {"site": "modules/gaussian_splatting/a.cpp::Fresh::build::values.resize(n)",
                 "reason": "revealed by comment masking; predates this change"},
            ],
        }
        self.assertEqual(self.guard.check_baseline_against_base(document), [])

    def test_ledger_entry_without_a_reason_is_rejected(self):
        """An addition with no stated reason is indistinguishable from a blessed defect."""
        self._stub_base(["modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)"])
        document = {
            "sites": [
                "modules/gaussian_splatting/a.cpp::Fresh::build::old.resize(n)",
                "modules/gaussian_splatting/a.cpp::Fresh::build::values.resize(n)",
            ],
            "detector_change_ledger": [
                {"site": "modules/gaussian_splatting/a.cpp::Fresh::build::values.resize(n)",
                 "reason": "   "},
            ],
        }
        self.assertTrue(self.guard.check_baseline_against_base(document))

    def test_unresolvable_review_base_fails_closed(self):
        """No base means no reference. It must never degrade to grading HEAD."""
        self.guard.resolve_review_base = lambda base_ref=None: (None, ["no base"])
        failures = self.guard.check_baseline_against_base({"sites": []})
        self.assertTrue(failures, "an unresolvable review base must fail, not pass")

    def test_baseline_absent_at_base_is_reported_not_failed(self):
        """A change that INTRODUCES the baseline has no shrink reference; say so."""
        self._stub_base(None)
        self.assertEqual(
            self.guard.check_baseline_against_base({"sites": ["anything"]}), [])

    # -- regressions found while fixing the above --------------------------
    def test_consumer_does_not_leak_across_functions(self):
        """A ptrw() in a LATER function must not satisfy an earlier resize.

        While making scope real, treating `namespace X {` as the enclosing span made
        the scan run to the end of the namespace, so an unrelated consumer in another
        function produced a false positive.
        """
        self._write("a.cpp", """
namespace GaussianSplatting {

Error NoConsumer::build() {
    Vector<float> values;
    values.resize(runtime_count);
    return OK;
}

Error Other::use() {
    Vector<float> values;
    int n = values.size();
    float *w = values.ptrw();
    return OK;
}

}
""")
        sites = self._sites()
        self.assertEqual(sites, [], f"consumer in a different function must not count: {sites}")

    def test_namespaced_functions_resolve_by_name(self):
        """Functions inside a namespace must key by their own name.

        `namespace {` opening a span collapsed every function in the file back onto
        one identity, which re-created the P1 collision behind an ordinal suffix.
        """
        self._write("a.cpp", """
namespace GaussianSplatting {
namespace {

Error Alpha::build() {
    Vector<float> payload;
    payload.resize(len);
    uint8_t *w = payload.ptrw();
    return OK;
}

Error Beta::build() {
    Vector<float> payload;
    payload.resize(len);
    uint8_t *w = payload.ptrw();
    return OK;
}

}
}
""")
        sites = self._sites()
        self.assertEqual(len(set(sites)), 2, f"namespaced functions must key separately: {sites}")
        self.assertTrue(all("<anonymous>" not in s for s in sites), f"unresolved span: {sites}")
        self.assertTrue(all("#" not in s for s in sites), f"ordinal fallback used instead of names: {sites}")


if __name__ == "__main__":
    unittest.main()
