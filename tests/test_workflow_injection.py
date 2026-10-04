"""Prove the injection is closed: same hostile report, fixed workflow.

The check reads the actual `Open the pull request` step out of the workflow,
substitutes a report containing an apostrophe and a command substitution, and runs
the step with `git` and `gh` stubbed so it reaches the body assembly.

A regression here means upstream prose can execute as shell in a job holding
contents:write. So this is a test, not a one-off: it lives in tests/ and runs in
the gate.
"""
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "rf-drift.yml"
MARKERS = ("pwned_by_upstream", "pwned_semicolon", "pwned_backtick")

HOSTILE_REPORTS = {
    # An apostrophe in the MeshCore region text, which the FAQ parser copies
    # verbatim out of upstream prose.
    "apostrophe only":
        "DRIFT  meshcore: CHANGED 'Bob's preset' : 910.525 -> 915.0",
    # What an apostrophe buys an attacker once the quoting breaks.
    "apostrophe then substitution":
        "DRIFT  meshcore: CHANGED '$(touch pwned_by_upstream)' preset : 915.0",
    "apostrophe then command chain":
        "DRIFT  x: CHANGED 'y'; touch pwned_semicolon : 915.0",
    "apostrophe then backtick":
        "DRIFT  x: CHANGED '`touch pwned_backtick`' : 915.0",
}


def _bash() -> str | None:
    for c in (r"C:\Program Files\Git\bin\bash.exe", "/bin/bash", "/usr/bin/bash"):
        if pathlib.Path(c).exists():
            return c
    return None


BASH = _bash()


def _step_script() -> str:
    """The `Open the pull request` step, dedented.

    Only the report placeholder is substituted: it is the one value the step must
    never receive through its own source. The version and run-number values now
    arrive as environment variables, so the harness supplies them that way rather
    than writing them into the script -- which is exactly the property under test.
    """
    text = WORKFLOW.read_text(encoding="utf-8")
    m = re.search(r"- name: Open the pull request.*?run: \|\n(.*?)\n\n      - name:", text, re.S)
    assert m, "the pull request step was renamed or removed; update this test"
    return "\n".join(line[10:] if line.startswith(" " * 10) else line
                     for line in m.group(1).splitlines())


@unittest.skipIf(BASH is None, "no POSIX shell available to run the step")
class NoScriptInjectionFromUpstreamReport(unittest.TestCase):
    """The drift report contains upstream prose and must never reach a shell."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="rfdrift-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        # Stubs, so the step gets past `git push` and `gh pr list`.
        bindir = self.tmp / "bin"
        bindir.mkdir()
        for name in ("git", "gh"):
            p = bindir / name
            p.write_text(f'#!/bin/sh\necho "[stub {name} $*]"\nexit 0\n',
                         encoding="utf-8", newline="\n")
            p.chmod(p.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
        self.env = dict(os.environ)
        # Supplied the way Actions supplies step-level `env:`, not spliced into the
        # script text.
        self.env["GH_TOKEN"] = "test-token"
        self.env["RUN_NUMBER"] = "1"
        self.env["NEW_VERSION"] = "0.2.0"
        self.env["OLD_VERSION"] = "0.1.0"
        self.bindir = bindir

    def _run(self, report: str):
        script = _step_script().replace("${{ needs.check.outputs.report }}", report)
        # Harness scaffolding, and the only line the harness adds: put the stubs
        # first. Exported with a colon because this is what the shell will read,
        # whatever separator the parent process used. The real job has a working
        # git and gh; these exist only so the step reaches the body assembly.
        script = f'export PATH="{self.bindir}:$PATH"\n' + script
        step = self.tmp / "step.sh"
        step.write_text(script, encoding="utf-8", newline="\n")
        self.env["DRIFT_REPORT"] = report
        res = subprocess.run([BASH, str(step)], capture_output=True, text=True,
                             cwd=str(self.tmp), env=self.env)
        fired = [m for m in MARKERS if (self.tmp / m).exists()]
        for m in fired:
            (self.tmp / m).unlink()
        return res, fired

    def test_no_hostile_report_executes_a_command(self):
        for name, report in HOSTILE_REPORTS.items():
            with self.subTest(report=name):
                res, fired = self._run(report)
                self.assertEqual(
                    fired, [],
                    f"upstream prose executed as a shell command: {fired}")

    def test_the_report_still_reaches_the_pull_request(self):
        # Closing the hole must not mean dropping the report. If the body quietly
        # lost it, the PR would claim a version change with no evidence in it.
        res, _ = self._run(HOSTILE_REPORTS["apostrophe only"])
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        body = self.tmp / "pr-body.md"
        self.assertTrue(body.is_file(), "the pull request body was not written")
        text = body.read_text(encoding="utf-8")
        self.assertIn("Bob's preset", text,
                      "the report is missing from the body; the quoting fix must "
                      "not have dropped it")
        self.assertIn("v0.2.0", text)
        self.assertIn("v0.1.0", text)

    def test_the_report_never_appears_in_the_script_source(self):
        # The structural property behind the fix: the value is passed through the
        # environment, so no line of the script contains it.
        script = _step_script()
        self.assertNotIn("${{ needs.check.outputs.report }}", script,
                         "the report is still spliced into the script")
        self.assertIn("DRIFT_REPORT", script,
                      "the body should be built from the environment variable")
        self.assertIn('"$DRIFT_REPORT"', script)
        self.assertIn("--body-file", script,
                      "the body belongs in a file, not a shell argument")


@unittest.skipIf(BASH is None, "no POSIX shell available")
class ReportIsNotSplicedIntoGithubScript(unittest.TestCase):
    """Same class of bug in JavaScript: a backtick in upstream prose would close
    the template literal and run as code."""

    def setUp(self):
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def test_github_script_blocks_read_the_report_from_the_environment(self):
        self.assertNotIn("'${{ needs.check.outputs.report }}'", self.text,
                         "the report is still spliced into a github-script body")
        self.assertNotIn('`${{ needs.check.outputs.report }}`', self.text)
        self.assertIn("process.env.DRIFT_REPORT", self.text)
        # Every DRIFT_REPORT reference that reaches a script body must be env-fed.
        self.assertEqual(self.text.count("DRIFT_REPORT: ${{"), 3,
                         "the three places that use the report should each declare it")


if __name__ == "__main__":
    unittest.main(verbosity=2)