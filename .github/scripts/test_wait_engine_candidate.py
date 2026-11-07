#!/usr/bin/env python3
"""Adversarial controls for upstream candidate assembly admission."""
import copy
import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "candidate_admission", Path(__file__).with_name("wait-engine-candidate.py"))
candidate_admission = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate_admission)


class CandidateAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.candidate = {"repository": "Sunrisepeak/mcppls-clangd", "run-id": 123,
                          "engine-commit": "a" * 40}
        self.run = {"id": 123, "head_sha": "a" * 40, "name": "build-test",
                    "event": "pull_request", "status": "in_progress", "conclusion": None}
        self.jobs = [dict(id=i + 1, name=name, run_attempt=1, status="completed",
                          conclusion="success", started_at="2026-11-04T10:00:00Z", steps=[])
                     for i, name in enumerate(sorted(candidate_admission.REQUIRED))]
        self.bundle = next(j for j in self.jobs if j["name"] == candidate_admission.BUNDLE)
        self.bundle["steps"] = [{"name": "The identity of clangd.okb", "conclusion": "success"}]
        self.artifacts = [dict(id=i + 1, name=name, expired=False, size_in_bytes=1024,
                               digest="sha256:" + "b" * 64, created_at="2026-11-04T10:01:00Z",
                               workflow_run={"id": 123, "head_sha": "a" * 40})
                          for i, name in enumerate(candidate_admission.PARTS)]

    def result(self, artifacts=True):
        return candidate_admission.admission(
            self.candidate, self.run, self.jobs, self.artifacts if artifacts else None)

    def test_complete_parts_admit_while_remaining_checks_run(self):
        self.jobs.append(dict(id=90, name="warm-prime linux-x64", status="in_progress",
                              conclusion=None, run_attempt=1))
        result = self.result()
        self.assertTrue(result["ready"])
        self.assertEqual(list(result["packaged-parts"]), ["clangd-openkal"])
        self.assertEqual(result["head"], self.candidate["engine-commit"])

    def test_pending_image_waits_without_admitting(self):
        job = next(j for j in self.jobs if j["name"] == "openkal / image aarch64-openkal")
        job.update(status="in_progress", conclusion=None)
        self.assertEqual(self.result(), {"ready": False, "pending": ["openkal / image aarch64-openkal"]})

    def test_native_engine_builds_are_not_waited_for(self):
        # A comparison run's native builds may still run, or a pull request's run may have none.
        self.jobs.append(dict(id=91, name="build win32-x64", status="in_progress",
                              conclusion=None, run_attempt=1))
        self.assertTrue(self.result()["ready"])

    def test_failure_outside_packaged_parts_refuses(self):
        for verdict in ("failure", "cancelled", "timed_out"):
            with self.subTest(verdict=verdict):
                self.jobs.append(dict(id=90, name="acceptance", status="completed",
                                      conclusion=verdict, run_attempt=1))
                with self.assertRaisesRegex(ValueError, "upstream jobs"):
                    self.result()
                self.jobs.pop()

    def test_wrong_run_or_head_refuses(self):
        for key, value in (("id", 124), ("head_sha", "c" * 40)):
            with self.subTest(key=key):
                run = dict(self.run, **{key: value})
                with self.assertRaisesRegex(ValueError, "run/head"):
                    candidate_admission.admission(self.candidate, run, self.jobs, self.artifacts)

    def test_skipped_or_missing_required_job_never_admits(self):
        self.jobs[0].update(status="completed", conclusion="skipped")
        self.assertFalse(self.result()["ready"])
        self.run.update(status="completed", conclusion="success")
        with self.assertRaisesRegex(ValueError, "ended without required parts"):
            self.result()

    def test_bundle_identity_cannot_be_replaced_by_green_job(self):
        self.bundle["steps"].clear()
        with self.assertRaisesRegex(ValueError, "identity check"):
            self.result()

    def test_bad_artifact_identity_digest_and_empty_bytes_refuse(self):
        for changes in ({"workflow_run": {"id": 124, "head_sha": "a" * 40}},
                        {"workflow_run": {"id": 123, "head_sha": "c" * 40}},
                        {"digest": "sha256:missing"}, {"size_in_bytes": 0}):
            with self.subTest(changes=changes):
                parts = copy.deepcopy(self.artifacts)
                parts[0].update(changes)
                with self.assertRaises(ValueError):
                    candidate_admission.admission(self.candidate, self.run, self.jobs, parts)

    def test_expired_or_earlier_attempt_artifact_waits_then_refuses_completed_run(self):
        for changes in ({"expired": True}, {"created_at": "2026-11-04T09:59:59Z"}):
            with self.subTest(changes=changes):
                parts = copy.deepcopy(self.artifacts)
                parts[0].update(changes)
                result = candidate_admission.admission(self.candidate, self.run, self.jobs, parts)
                self.assertFalse(result["ready"])
                with self.assertRaisesRegex(ValueError, "artifact absent"):
                    candidate_admission.admission(
                        self.candidate, dict(self.run, status="completed", conclusion="success"),
                        self.jobs, parts)

    def test_latest_successful_attempt_supersedes_old_cancellation(self):
        old = dict(self.jobs[0], id=0, run_attempt=0, conclusion="cancelled")
        self.jobs.append(old)
        self.assertTrue(self.result()["ready"])

    def test_ended_failed_run_refuses_even_with_all_parts(self):
        self.run.update(status="completed", conclusion="failure")
        with self.assertRaisesRegex(ValueError, "upstream run"):
            self.result()

    def test_malformed_candidate_cannot_select_another_api_path(self):
        for changes in ({"repository": "../.."}, {"repository": "owner/repo?other=1"},
                        {"run-id": True}, {"engine-commit": "not-a-commit"}):
            with self.subTest(changes=changes):
                candidate = dict(self.candidate, **changes)
                with self.assertRaises(ValueError):
                    candidate_admission.admission(candidate, self.run, self.jobs, self.artifacts)

    def test_metadata_is_requested_only_after_part_and_identity_gates(self):
        self.assertEqual(self.result(artifacts=False), {"ready": False, "need-artifacts": True})


if __name__ == "__main__":
    unittest.main()
