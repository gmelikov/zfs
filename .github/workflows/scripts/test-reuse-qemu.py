#!/usr/bin/env python3

"""Exercise QEMU result reuse without credentials or network access."""

import contextlib
import copy
import datetime
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import urllib.error


spec = importlib.util.spec_from_file_location(
    "reuse_qemu", Path(__file__).with_name("reuse-qemu.py"))
reuse = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reuse)
NOW = datetime.datetime(2026, 10, 13, tzinfo=datetime.timezone.utc)
SHA = "a" * 40
REPO = "contributor/zfs"


class ReuseTests(unittest.TestCase):
    def setUp(self):
        self.event = {"pull_request": {
            "head": {"sha": SHA, "repo": {"full_name": REPO}},
            "base": {"sha": "b" * 40,
                     "repo": {"full_name": "openzfs/zfs"}},
        }}
        self.run = {
            "id": 123, "head_sha": SHA, "event": "push",
            "status": "completed", "path": reuse.WORKFLOW,
            "created_at": "2026-10-12T00:00:00Z",
            "repository": {"full_name": REPO},
            "head_repository": {"full_name": REPO},
        }
        self.job = {
            "id": 456, "run_id": 123, "head_sha": SHA,
            "name": "qemu-x86 (ubuntu24)", "status": "completed",
            "conclusion": "success", "labels": ["ubuntu-24.04"],
            "steps": [{"name": name, "status": "completed",
                       "conclusion": "success"}
                      for name in reuse.REQUIRED_STEPS],
        }
        self.runs = [self.run]
        self.jobs = [self.job]
        self.calls = []

    def api(self, path):
        self.calls.append(path)
        if "/jobs?" in path:
            return {"total_count": len(self.jobs), "jobs": self.jobs}
        return {"workflow_runs": self.runs}

    def find(self, targets=None, forks=None):
        return reuse.reusable_jobs(
            self.event, [REPO] if forks is None else forks,
            ["ubuntu24"] if targets is None else targets,
            api=self.api, now=NOW)

    def test_success(self):
        self.assertEqual(self.find(), {
            "ubuntu24": f"https://github.com/{REPO}/actions/runs/123/job/456",
        })
        self.assertIn(f"head_sha={SHA}", self.calls[0])
        self.assertIn("filter=latest", self.calls[1])

    def test_untrusted_fork(self):
        self.assertEqual(self.find(forks=[]), {})
        self.assertEqual(self.calls, [])

    def test_fork_names_are_case_insensitive(self):
        self.assertTrue(self.find(forks=[REPO.upper()]))

    def test_same_repository(self):
        self.event["pull_request"]["base"]["repo"]["full_name"] = REPO
        self.assertEqual(self.find(), {})
        self.assertEqual(self.calls, [])

    def test_empty_matrix(self):
        self.assertEqual(self.find(targets=[]), {})
        self.assertEqual(self.calls, [])

    def test_invalid_allowlist(self):
        for forks in (REPO, [True], {REPO: True}):
            with self.subTest(forks=forks), self.assertRaises(ValueError):
                self.find(forks=forks)

    def test_no_runs(self):
        self.runs = []
        self.assertEqual(self.find(), {})

    def test_ineligible_run(self):
        for key, value in (
                ("head_sha", "c" * 40), ("event", "workflow_dispatch"),
                ("event", "pull_request"), ("status", "in_progress"),
                ("path", ".github/workflows/other.yml"),
                ("created_at", "2026-10-05T00:00:00Z"),
                ("created_at", "2026-10-14T00:00:00Z"),
                ("repository", {"full_name": "other/zfs"}),
                ("head_repository", {"full_name": "other/zfs"})):
            with self.subTest(key=key, value=value):
                self.runs = [{**self.run, key: value}]
                self.assertEqual(self.find(), {})

    def test_ineligible_job(self):
        for key, value in (
                ("head_sha", "c" * 40), ("run_id", 999),
                ("status", "in_progress"), ("conclusion", "failure"),
                ("conclusion", "cancelled"), ("conclusion", "skipped"),
                ("labels", ["self-hosted", "ubuntu-24.04"]),
                ("name", "qemu-x86 (debian13)")):
            with self.subTest(key=key, value=value):
                self.jobs = [{**self.job, key: value}]
                self.assertEqual(self.find(), {})

    def test_required_steps(self):
        for name in reuse.REQUIRED_STEPS:
            for conclusion in ("skipped", "failure", None):
                with self.subTest(name=name, conclusion=conclusion):
                    self.jobs = [copy.deepcopy(self.job)]
                    for step in self.jobs[0]["steps"]:
                        if step["name"] == name:
                            step["conclusion"] = conclusion
                    self.assertEqual(self.find(), {})
            with self.subTest(missing=name):
                self.jobs = [copy.deepcopy(self.job)]
                self.jobs[0]["steps"] = [
                    step for step in self.job["steps"] if step["name"] != name]
                self.assertEqual(self.find(), {})

    def test_partial_matrix_and_failed_workflow(self):
        self.run["conclusion"] = "failure"
        self.jobs.append({**self.job, "name": "qemu-x86 (debian13)",
                          "conclusion": "failure"})
        self.assertEqual(set(self.find(targets=["ubuntu24", "debian13"])),
                         {"ubuntu24"})

    def test_latest_attempt_failure_is_not_replaced_by_an_older_run(self):
        self.runs.append({**self.run, "id": 100})
        self.job["conclusion"] = "failure"
        self.assertEqual(self.find(), {})
        self.assertEqual(len(self.calls), 2)

    def test_truncated_jobs(self):
        def truncated(path):
            result = self.api(path)
            if "jobs" in result:
                result["total_count"] = 101
            return result

        self.assertEqual(reuse.reusable_jobs(
            self.event, [REPO], ["ubuntu24"], api=truncated, now=NOW), {})

    def test_main_falls_back_on_api_and_config_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            event_path = Path(directory) / "event.json"
            event_path.write_text(json.dumps(self.event))
            env = {"GITHUB_EVENT_NAME": "pull_request",
                   "GITHUB_EVENT_PATH": str(event_path),
                   "ZTS_REUSE_FORKS": json.dumps([REPO]),
                   "ZTS_TARGETS": '["ubuntu24"]'}
            for error in (urllib.error.URLError("unavailable"),
                          ValueError("invalid JSON"), KeyError("jobs"),
                          RuntimeError("Unexpected workflow data")):
                with self.subTest(error=error), patch.dict(os.environ, env), \
                        patch.object(reuse, "reusable_jobs",
                                     side_effect=error), \
                        contextlib.redirect_stdout(io.StringIO()) as output, \
                        contextlib.redirect_stderr(io.StringIO()):
                    reuse.main()
                    self.assertEqual(json.loads(output.getvalue()), {})

    def test_push_never_reuses(self):
        with patch.dict(os.environ, {"GITHUB_EVENT_NAME": "push"}), \
                patch.object(reuse, "reusable_jobs") as find, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            reuse.main()
            find.assert_not_called()
            self.assertEqual(json.loads(output.getvalue()), {})


class SummaryTests(unittest.TestCase):
    def summarize(self, directory):
        script = Path(__file__).with_name("qemu-9-summary-page.sh").resolve()
        summary = Path(directory) / "job-summary.md"
        summary.touch()
        env = {**os.environ, "GITHUB_STEP_SUMMARY": str(summary)}
        for index in range(1, 20):
            subprocess.run(["bash", str(script), str(index)],
                           cwd=directory, env=env, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return summary.read_text()

    def test_all_results_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.summarize(directory), "")
            self.assertEqual(list(Path(directory).glob("out-*.md")), [])

    def test_partial_reuse_keeps_local_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "data"
            data.mkdir()
            files = {"env.txt": "OSNAME='Ubuntu test'\n",
                     "uname.txt": "test kernel\n", "summary.txt": "PASS\n",
                     "failed.txt": ""}
            archive_dir = root / "qemu-ubuntu24.tar.bz2"
            archive_dir.mkdir()
            with tarfile.open(archive_dir / "qemu-ubuntu24.tar.bz2",
                              "w:bz2") as archive:
                for name, content in files.items():
                    (data / name).write_text(content)
                    archive.add(data / name, arcname=name)
            summary = self.summarize(directory)
            self.assertIn("Functional Tests: Ubuntu test", summary)
            self.assertIn("PASS", summary)
            self.assertNotIn("unknown", summary)


if __name__ == "__main__":
    unittest.main()
