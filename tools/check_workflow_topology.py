#!/usr/bin/env python3
"""Validate the public CI control plane and immutable private-source contract."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import yaml


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW_ROOT = ROOT / ".github/workflows"


def fail(message: str) -> None:
    raise SystemExit(f"workflow topology: {message}")


def load_workflow(path: Path) -> dict[str, Any]:
    try:
        workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as error:
        fail(f"{path}: {error}")
    if not isinstance(workflow, dict):
        fail(f"{path}: workflow root must be a mapping")
    return workflow


def triggers(workflow: dict[str, Any]) -> dict[str, Any]:
    value = workflow.get("on", workflow.get(True, {}))
    return value if isinstance(value, dict) else {}


def needs(job: dict[str, Any]) -> list[str]:
    value = job.get("needs", [])
    return value if isinstance(value, list) else [value]


def require_private_source_contract(name: str, workflow: dict[str, Any]) -> None:
    call = triggers(workflow).get("workflow_call")
    if not isinstance(call, dict):
        fail(f"{name} must support workflow_call")
    inputs = call.get("inputs", {})
    secrets = call.get("secrets", {})
    if "source_ref" not in inputs:
        fail(f"{name} must accept source_ref")
    if not secrets.get("GDPP_TOKEN", {}).get("required"):
        fail(f"{name} must require GDPP_TOKEN")


def main() -> int:
    workflows = {
        path.name: load_workflow(path)
        for path in sorted(WORKFLOW_ROOT.glob("*.yml"))
    }
    release = workflows["release.yml"]
    release_jobs = release["jobs"]

    for name, workflow in workflows.items():
        if workflow.get("permissions", {}).get("contents") != "read":
            fail(f"{name} must default to contents: read")
        if "pull_request" in triggers(workflow):
            fail(f"{name} must never execute for pull requests")

    publish = release_jobs["publish"]
    if publish.get("permissions", {}).get("contents") != "write":
        fail("only the publish job may request contents: write")
    if set(triggers(release)) != {"workflow_dispatch"}:
        fail("release.yml must expose only workflow_dispatch")

    parallel = {
        "quality-gate": "quality.yml",
        "core-gate": "core.yml",
        "native-gate": "native-integration.yml",
        "godot-gate": "godot-compatibility.yml",
        "android-gate": "android.yml",
        "web-gate": "web.yml",
        "ios-gate": "ios.yml",
        "host-components": "host-components.yml",
    }
    for job_name, workflow_name in parallel.items():
        workflow = workflows[workflow_name]
        require_private_source_contract(workflow_name, workflow)
        if "workflow_dispatch" not in triggers(workflow):
            fail(f"{workflow_name} must support standalone workflow_dispatch")
        job = release_jobs[job_name]
        if job.get("uses") != f"./.github/workflows/{workflow_name}":
            fail(f"{job_name} must invoke {workflow_name}")
        if needs(job) != ["preflight"]:
            fail(f"{job_name} must start immediately after preflight")
        if job.get("with", {}).get("source_ref") != (
            "${{ needs.preflight.outputs.source_sha }}"
        ):
            fail(f"{job_name} must use the immutable preflight source SHA")
        if job.get("secrets", {}).get("GDPP_TOKEN") != "${{ secrets.GDPP_TOKEN }}":
            fail(f"{job_name} must pass only the explicit source token")

    package_workflow = workflows["package-release.yml"]
    require_private_source_contract("package-release.yml", package_workflow)
    if "workflow_dispatch" in triggers(package_workflow):
        fail("package-release.yml cannot run without producer artifacts")
    package_job = release_jobs["packages"]
    if package_job.get("uses") != "./.github/workflows/package-release.yml":
        fail("packages must invoke package-release.yml")
    expected_package_needs = sorted([*parallel, "preflight"])
    if sorted(needs(package_job)) != expected_package_needs:
        fail("packages must wait for preflight and every producer")

    smoke_workflow = workflows["release-package-smoke.yml"]
    require_private_source_contract("release-package-smoke.yml", smoke_workflow)
    smoke_dispatch = triggers(smoke_workflow).get("workflow_dispatch", {})
    smoke_dispatch_inputs = smoke_dispatch.get("inputs", {})
    if not smoke_dispatch_inputs.get("source_ref", {}).get("required"):
        fail("release-package-smoke.yml dispatch must require an immutable source_ref")
    if not smoke_dispatch_inputs.get("artifact_run_id", {}).get("required"):
        fail("release-package-smoke.yml dispatch must require an assembled artifact run")
    smoke_matrix = smoke_workflow["jobs"]["desktop"]["strategy"]["matrix"]["include"]
    actual_smokes = sorted((entry["archive"], entry["os"]) for entry in smoke_matrix)
    expected_smokes = sorted(
        (
            ("gdpp.zip", "macos-15"),
            ("gdpp.zip", "ubuntu-22.04"),
            ("gdpp.zip", "windows-2025"),
            ("gdpp-lite.zip", "macos-15"),
            ("gdpp-lite.zip", "windows-2025"),
        )
    )
    if actual_smokes != expected_smokes:
        fail("installed package smoke matrix is incomplete")
    smoke_job = release_jobs["package-smoke"]
    if smoke_job.get("uses") != "./.github/workflows/release-package-smoke.yml":
        fail("package-smoke must invoke release-package-smoke.yml")
    if sorted(needs(smoke_job)) != ["packages", "preflight"]:
        fail("package-smoke must retain source identity and wait for packages")

    readiness_needs = sorted([*parallel, "packages", "package-smoke"])
    if sorted(needs(release_jobs["readiness"])) != readiness_needs:
        fail("readiness must aggregate every delivery result")
    if sorted(needs(publish)) != ["preflight", "readiness"]:
        fail("publish must retain source identity and depend on readiness")

    for name, workflow in workflows.items():
        if name == "release.yml":
            continue
        source_checkouts = sum(
            step.get("uses") == "./.github/actions/checkout-source"
            for job in workflow["jobs"].values()
            for step in job.get("steps", [])
        )
        if source_checkouts == 0:
            fail(f"{name} must check out private source")

    print(
        f"Validated {len(parallel)} parallel producers, one gated package stage, "
        "installed macOS/Linux/Windows package smokes, and one publish stage."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
