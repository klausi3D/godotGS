#!/usr/bin/env python3
"""Discrimination tests for `check_overrides_no_inline_script.py`.

The guard passing on the committed tree only proves the tree is clean today.
Each case below drives a synthetic `overrides/` tree the guard must reject, or is
a control: clean markup must pass, and an empty subject must not.
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_overrides_no_inline_script as guard  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

CLEAN_TEMPLATE = """{% extends "main.html" %}
{% block container %}
<main class="md-main" data-md-component="main">
  <a href="{{ 'getting-started/downloads/' | url }}" class="gsm-btn" data-onboarding="1">Download</a>
  <button type="button" aria-label="Copy">Copy</button>
  <canvas id="gsm-splat" style="width:100%;display:block;"></canvas>
  <p>Read the description; once done, continue. Questions = welcome.</p>
</main>
{% endblock %}
"""


def _run_guard(root: Path) -> tuple[int, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = guard.main(["--root", str(root)])
    return code, out.getvalue() + err.getvalue()


class SyntheticOverridesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.overrides = self.root / "overrides"
        self.overrides.mkdir()

    def _write(self, rel: str, text: str) -> None:
        path = self.overrides / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def _assert_rejected(self, body: str, expected_fragment: str) -> None:
        self._write("home.html", CLEAN_TEMPLATE.replace("</main>", body + "\n</main>"))
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn(expected_fragment, output)

    # Controls.

    def test_clean_markup_passes(self) -> None:
        self._write("home.html", CLEAN_TEMPLATE)
        self._write("partials/nested/footer.html", "<footer>{{ super() }}</footer>\n")
        self._write("assets/extra.css", ".gsm-btn { color: red; }\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)

    def test_missing_overrides_directory_is_not_a_pass(self) -> None:
        self.overrides.rmdir()
        code, _ = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_EMPTY_SUBJECT)

    def test_empty_overrides_directory_is_not_a_pass(self) -> None:
        code, _ = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_EMPTY_SUBJECT)

    # Script tags.

    def test_script_tag_fails(self) -> None:
        self._assert_rejected("<script>alert(1)</script>", "script tag")

    def test_script_tag_any_case_and_spacing_fails(self) -> None:
        for body in ("<SCRIPT src=x.js></SCRIPT>", "<ScRiPt>1</ScRiPt>", "< script>1</script>"):
            with self.subTest(body=body):
                self._assert_rejected(body, "script tag")

    def test_script_tag_in_a_comment_still_fails(self) -> None:
        for body in ("<!-- <script>1</script> -->", "{# <script>1</script> #}"):
            with self.subTest(body=body):
                self._assert_rejected(body, "script tag")

    def test_script_tag_in_a_nested_partial_fails(self) -> None:
        self._write("home.html", CLEAN_TEMPLATE)
        self._write("partials/nested/footer.html", "<footer><script>1</script></footer>\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("overrides/partials/nested/footer.html", output)

    # Inline event handlers.

    def test_onclick_fails(self) -> None:
        self._assert_rejected('<a href="#" onclick="go()">x</a>', "inline event-handler")

    def test_event_handler_variants_fail(self) -> None:
        for body in (
            "<img src=x ONERROR=go()>",
            '<body onload = "go()">',
            "<svg/onload=go()>",
            '<a href="x"onmouseover="go()">x</a>',
            '<div\n  onfocus="go()">x</div>',
        ):
            with self.subTest(body=body):
                self._assert_rejected(body, "inline event-handler")

    # javascript: URLs.

    def test_javascript_url_fails(self) -> None:
        self._assert_rejected('<a href="javascript:alert(1)">x</a>', "javascript: URL")

    def test_obfuscated_javascript_url_fails(self) -> None:
        for body in (
            '<a href="JavaScript:go()">x</a>',
            '<a href="  javascript :go()">x</a>',
            '<a href="java\tscript:go()">x</a>',
            '<a href="java&#x09;script:go()">x</a>',
            '<a href="&#106;avascript:go()">x</a>',
            '<a href="javascript&colon;go()">x</a>',
        ):
            with self.subTest(body=body):
                self._assert_rejected(body, "javascript: URL")

    # Nested documents, plugins and refresh redirects (PR #1207 round 5).

    def test_srcdoc_iframe_fails(self) -> None:
        # The browser entity-decodes srcdoc and runs the nested document.
        self._assert_rejected(
            '<iframe srcdoc="&lt;script&gt;parent.location=1&lt;/script&gt;"></iframe>',
            "srcdoc attribute",
        )

    def test_srcdoc_on_any_element_fails(self) -> None:
        for body in ('<div SRCDOC="x"></div>', "<x-frame srcdoc='x'></x-frame>", "<p srcdoc>x</p>"):
            with self.subTest(body=body):
                self._assert_rejected(body, "srcdoc attribute")

    def test_embedding_elements_fail(self) -> None:
        for body in (
            '<iframe src="https://example.com/"></iframe>',
            '<IFRAME src="x"></IFRAME>',
            '<object data="x.svg"></object>',
            '<embed src="x.swf">',
            '<frameset><frame src="x"></frameset>',
            '< frame src="x">',
        ):
            with self.subTest(body=body):
                self._assert_rejected(body, "nested-document/plugin element")

    def test_meta_refresh_with_url_fails(self) -> None:
        for body in (
            '<meta http-equiv="refresh" content="0; url=https://example.com/">',
            "<meta content='0;https://example.com/' http-equiv=Refresh>",
            '<META HTTP-EQUIV="refresh" CONTENT="5,URL=x">',
            '<meta http-equiv="refresh" content="{{ delay }}; url={{ target }}">',
            '<meta content="0;url=a>b" http-equiv="refresh">',
        ):
            with self.subTest(body=body):
                self._assert_rejected(body, "meta refresh with a URL")

    def test_meta_refresh_without_url_and_other_meta_pass(self) -> None:
        self._write(
            "home.html",
            CLEAN_TEMPLATE.replace(
                "</main>",
                '<meta http-equiv="refresh" content="30">\n'
                '<meta name="description" content="0; url=not-a-refresh">\n'
                '<meta http-equiv="content-type" content="text/html">\n</main>',
            ),
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)

    # Entity-encoded payloads in any attribute.

    def test_entity_encoded_payloads_fail(self) -> None:
        for body, fragment in (
            ('<div title="&lt;script&gt;x&lt;/script&gt;"></div>', "script tag"),
            ('<div title="&#60;img src=x &#111;&#110;error=go()&#62;"></div>', "inline event-handler"),
            ('<div data-x="x&#x20;&#x6F;nclick&#61;go()"></div>', "inline event-handler"),
            ('<div title="&lt;iframe src=x&gt;"></div>', "nested-document/plugin element"),
            ('<meta http-equiv="&#114;efresh" content="0;url=x">', "meta refresh with a URL"),
        ):
            with self.subTest(body=body):
                self._assert_rejected(body, fragment)
                self._assert_rejected(body, "(after HTML entity decoding)")

    def test_raw_hit_is_not_reported_twice_after_decoding(self) -> None:
        self._write("home.html", CLEAN_TEMPLATE.replace("</main>", "<script>a &amp; b</script>\n</main>"))
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertEqual(output.count("script tag"), 1, output)
        self.assertNotIn("after HTML entity decoding", output)

    def test_clean_markup_with_entities_passes(self) -> None:
        self._write(
            "home.html",
            CLEAN_TEMPLATE.replace(
                "</main>",
                "<p>Splats &amp; meshes &mdash; 2 &lt; 3, &quot;online&quot; &#169;</p>\n</main>",
            ),
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)

    # File types and encodings.

    def test_script_file_under_overrides_fails(self) -> None:
        self._write("home.html", CLEAN_TEMPLATE)
        self._write("assets/javascripts/extra.js", "document.title = 'x';\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("is not allowed", output)

    def test_non_utf8_file_is_uninspectable(self) -> None:
        self._write("home.html", CLEAN_TEMPLATE)
        (self.overrides / "other.html").write_bytes("<script>".encode("utf-16"))
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)


class RepositoryOverridesTest(unittest.TestCase):
    def test_repository_overrides_are_a_non_empty_subject(self) -> None:
        # Non-vacuity on the real tree: the guard must actually see home.html.
        files = [p.relative_to(ROOT).as_posix() for p in guard.iter_override_files(ROOT)]
        self.assertIn("overrides/home.html", files)


if __name__ == "__main__":
    unittest.main()
