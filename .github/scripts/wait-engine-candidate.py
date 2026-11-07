#!/usr/bin/env python3
"""Admit complete engine parts before CI assembles an unreleased payload."""
import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess
import time


# The engine is distributed only as the ok image (mcppls-clangd 0.0.21): a candidate is the images of both
# architectures and clangd.okb, the payload part every platform's entry starts.
REQUIRED = {f"openkal / image {arch}-openkal" for arch in ("x86_64", "aarch64")}
BUNDLE = "openkal / clangd.okb, the payload part"
REQUIRED.add(BUNDLE)
PARTS = {"clangd-openkal": BUNDLE}
BAD = {"failure", "cancelled", "timed_out", "action_required", "startup_failure"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def candidate_fields(candidate):
    repo = candidate.get("repository", "")
    run_id = candidate.get("run-id")
    head = candidate.get("engine-commit", "")
    require(isinstance(repo, str)
            and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_.-]+", repo)
            and repo.rsplit("/", 1)[-1] not in (".", ".."),
            "invalid candidate repository")
    require(type(run_id) is int and run_id > 0, "invalid candidate run-id")
    require(isinstance(head, str) and re.fullmatch(r"[0-9a-f]{40}", head),
            "invalid candidate engine-commit")
    return repo, run_id, head


def admission(candidate, run, jobs, artifacts=None):
    """Pending parts may wait; known failures and inconsistent identities refuse."""
    repo, run_id, head = candidate_fields(candidate)
    require(run.get("id") == run_id and run.get("head_sha") == head,
            "candidate run/head mismatch")
    require(run.get("name") == "build-test" and run.get("event") == "pull_request",
            "candidate must name the engine PR's build-test run")
    require(run.get("conclusion") in (None, "", "success"),
            f"upstream run ended with {run.get('conclusion')}")
    latest = {}
    for job in jobs:
        old = latest.get(job["name"])
        order = (job.get("run_attempt", 0), job["id"])
        if old is None or order > (old.get("run_attempt", 0), old["id"]):
            latest[job["name"]] = job
    failed = sorted(name for name, job in latest.items()
                    if job.get("conclusion") in BAD)
    require(not failed, f"upstream jobs failed or were cancelled: {failed}")
    pending = sorted(name for name in REQUIRED
                     if latest.get(name, {}).get("status") != "completed"
                     or latest.get(name, {}).get("conclusion") != "success")
    if pending:
        require(run.get("status") != "completed", "upstream ended without required parts")
        return {"ready": False, "pending": pending}
    require(any(step.get("name") == "The identity of clangd.okb"
                and step.get("conclusion") == "success"
                for step in latest[BUNDLE].get("steps", [])),
            "universal bundle identity check did not pass")
    if artifacts is None:
        return {"ready": False, "need-artifacts": True}
    parts = {}
    for name, producer in PARTS.items():
        matching = [a for a in artifacts if a.get("name") == name
                    and not a.get("expired", True)
                    and a.get("created_at", "") >= latest[producer]["started_at"]]
        if not matching:
            require(run.get("status") != "completed", f"required artifact absent: {name}")
            return {"ready": False, "pending-artifacts": [name]}
        part = max(matching, key=lambda a: (a["created_at"], a["id"]))
        origin = part.get("workflow_run", {})
        require(origin.get("id") == run_id and origin.get("head_sha") == head,
                f"artifact run/head mismatch: {name}")
        require(part.get("size_in_bytes", 0) > 0
                and re.fullmatch(r"sha256:[0-9a-f]{64}", part.get("digest", "")),
                f"artifact has no immutable digest or bytes: {name}")
        parts[name] = {key: part[key] for key in (
            "id", "name", "digest", "size_in_bytes", "created_at")}
    return {
        "ready": True, "repository": repo, "run": run_id, "head": head,
        "required-jobs": {name: latest[name]["id"] for name in sorted(REQUIRED)},
        "packaged-parts": parts,
        "scope": "CI assembly admission only. Complete engine and server qualification, "
                 "ledger promotion and published-byte verification remain required for release.",
    }


def gh(path, collection=None):
    command = ["gh", "api"]
    if collection:
        command += ["--paginate", "--slurp"]
    result = subprocess.run(command + [path], check=True, capture_output=True, text=True)
    data = json.loads(result.stdout)
    return [item for page in data for item in page[collection]] if collection else data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wait-seconds", type=int, default=5400)
    parser.add_argument("--poll-seconds", type=int, default=30)
    args = parser.parse_args()
    require(args.wait_seconds > 0 and args.poll_seconds > 0, "invalid wait interval")
    candidate = json.loads(args.candidate.read_text())
    repo, run_id, _ = candidate_fields(candidate)
    prefix = f"repos/{repo}/actions/runs/{run_id}"
    deadline = time.monotonic() + args.wait_seconds
    previous = None
    while time.monotonic() < deadline:
        try:
            run = gh(prefix)
            jobs = gh(prefix + "/jobs?filter=all&per_page=100", "jobs")
            state = admission(candidate, run, jobs)
            if state.get("need-artifacts"):
                artifacts = gh(prefix + "/artifacts?per_page=100", "artifacts")
                state = admission(candidate, run, jobs, artifacts)
        except subprocess.CalledProcessError as error:
            print(f"GitHub query failed; retrying: {error.stderr[-500:]}", flush=True)
            time.sleep(min(args.poll_seconds, max(0, deadline - time.monotonic())))
            continue
        if state != previous:
            print(json.dumps(state), flush=True)
            previous = state
        if state["ready"]:
            state["utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            args.output.write_text(json.dumps(state, indent=2) + "\n")
            return 0
        time.sleep(min(args.poll_seconds, max(0, deadline - time.monotonic())))
    raise ValueError("timed out waiting for complete, verified engine candidate parts")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, TypeError) as error:
        raise SystemExit(f"candidate admission refused: {error}")
