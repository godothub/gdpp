#!/usr/bin/env python3
"""Validate the public CI control plane and immutable private-source contract."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_ROOT = ROOT / ".github/workflows"
PORTABLE_RUNTIME_LOG = (
    'Path(log_path).write_text(result.stdout, encoding="utf-8", newline="\\n")'
)


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

    checkout_source = (ROOT / ".github/actions/checkout-source/action.yml").read_text(
        encoding="utf-8"
    )
    if "fetch-depth: 0" not in checkout_source:
        fail("private source checkout must retain complete history for release-range gates")
    if "HEAD:.github" not in checkout_source or "source_pipeline_tree" not in checkout_source:
        fail("private source checkout must validate the public control-plane tree")
    if "pipeline_sha" in checkout_source or (
        'test "$pipeline_sha" = "$GITHUB_SHA"' in checkout_source
    ):
        fail("private source checkout cannot bind release-file commits to a pipeline SHA")

    setup_godot = (ROOT / ".github/actions/setup-godot/action.yml").read_text(
        encoding="utf-8"
    )
    for template_set in ("host", "linux", "macos", "windows", "android", "web"):
        if f'$GODOT_TEMPLATES" != "{template_set}"' not in setup_godot:
            fail(f"official Godot setup must accept the {template_set} template set")

    quality_source = (WORKFLOW_ROOT / "quality.yml").read_text(encoding="utf-8")
    if (
        '-S "VERSION $version"' not in quality_source
        or '"$release_start^"' not in quality_source
    ):
        fail("quality formatting must cover the complete current product release range")
    if "git rev-parse HEAD^" in quality_source:
        fail("quality formatting cannot inspect only the final source commit")
    if ".github/tools/test_sync_release_files.py" not in quality_source:
        fail("quality.yml must validate the fixed release-file synchronizer")
    for test_tool in (
        "test_component_artifact.py",
        "test_publish_release_transaction.py",
        "test_verify_release_assets.py",
    ):
        if test_tool not in quality_source:
            fail(f"quality.yml must validate {test_tool}")

    for name, workflow in workflows.items():
        if workflow.get("permissions", {}).get("contents") != "read":
            fail(f"{name} must default to contents: read")
        if "pull_request" in triggers(workflow):
            fail(f"{name} must never execute for pull requests")

    permitted_artifact_jobs = {
        ("android.yml", "target-sdk"),
        ("host-components.yml", "build"),
        ("ios.yml", "target-sdk"),
        ("package-release.yml", "package"),
        ("web.yml", "target-sdk"),
    }
    expected_component_uploads = {
        ("android.yml", "target-sdk"): (
            "gdpp-android-arm64-${{ steps.source.outputs.sha }}",
            "source/build/component-artifacts/"
            "gdpp-android-arm64-${{ steps.source.outputs.sha }}",
        ),
        ("host-components.yml", "build"): (
            "gdpp-host-${{ matrix.host }}-${{ steps.source.outputs.sha }}",
            "source/build/component-artifacts/"
            "gdpp-host-${{ matrix.host }}-${{ steps.source.outputs.sha }}",
        ),
        ("ios.yml", "target-sdk"): (
            "gdpp-ios-${{ steps.source.outputs.sha }}",
            "source/build/component-artifacts/gdpp-ios-${{ steps.source.outputs.sha }}",
        ),
        ("web.yml", "target-sdk"): (
            "gdpp-web-${{ matrix.profile }}-${{ matrix.variant }}-"
            "${{ steps.source.outputs.sha }}",
            "source/build/component-artifacts/"
            "gdpp-web-${{ matrix.profile }}-${{ matrix.variant }}-"
            "${{ steps.source.outputs.sha }}",
        ),
    }
    observed_artifact_jobs: set[tuple[str, str]] = set()
    for workflow_name, workflow in workflows.items():
        for job_name, job in workflow["jobs"].items():
            uploads = [
                step
                for step in job.get("steps", [])
                if str(step.get("uses", "")).startswith("actions/upload-artifact@")
            ]
            if uploads and (workflow_name, job_name) not in permitted_artifact_jobs:
                fail(
                    f"{workflow_name}:{job_name} cannot upload private diagnostics or "
                    "unreleased source trees"
                )
            if uploads:
                observed_artifact_jobs.add((workflow_name, job_name))
            if len(uploads) > (1 if (workflow_name, job_name) in permitted_artifact_jobs else 0):
                fail(f"{workflow_name}:{job_name} declares unexpected artifact uploads")
            for upload in uploads:
                options = upload.get("with", {})
                name = str(options.get("name", ""))
                path = str(options.get("path", ""))
                if workflow_name == "package-release.yml":
                    if name != "gdpp-release-packages-${{ steps.source.outputs.sha }}":
                        fail("release package artifact must bind the private source SHA")
                    package_paths = [
                        line.strip() for line in path.splitlines() if line.strip()
                    ]
                    if package_paths != [
                        "source/build/release/gdpp.zip",
                        "source/build/release/SHA256SUMS",
                    ]:
                        fail("release package artifact must contain the exact public assets")
                else:
                    expected_name, expected_path = expected_component_uploads[
                        (workflow_name, job_name)
                    ]
                    if name != expected_name or path != expected_path:
                        fail(f"{workflow_name}:{job_name} component upload is not exact")
                    if options.get("include-hidden-files") is not True:
                        fail(
                            f"{workflow_name}:{job_name} must preserve its exact sealed topology"
                        )
    if observed_artifact_jobs != permitted_artifact_jobs:
        fail("required authenticated component/package artifact producers are incomplete")
    control_plane_source = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted(WORKFLOW_ROOT.parent.rglob("*.yml"))
    )
    if "audit_public_artifact" in control_plane_source:
        fail("the control plane cannot rely on a heuristic diagnostic source scanner")
    for workflow_name in (
        "android.yml",
        "host-components.yml",
        "ios.yml",
        "web.yml",
    ):
        source = (WORKFLOW_ROOT / workflow_name).read_text(encoding="utf-8")
        if "component_artifact.py\" seal" not in source:
            fail(f"{workflow_name} must seal every uploaded component artifact")
    package_source_text = (WORKFLOW_ROOT / "package-release.yml").read_text(
        encoding="utf-8"
    )
    if "component_artifact.py\" materialize" not in package_source_text:
        fail("package-release.yml must authenticate components before assembly")

    publish = release_jobs["publish"]
    if publish.get("permissions", {}).get("contents") != "write":
        fail("only the publish job may request contents: write")
    publish_steps = publish.get("steps", [])
    public_checkout = next(
        (step for step in publish_steps if step.get("name") == "Check out public pipeline"),
        None,
    )
    if not isinstance(public_checkout, dict):
        fail("publish must check out the public control plane")
    checkout_options = public_checkout.get("with", {})
    if checkout_options.get("persist-credentials") is not True:
        fail("publish checkout must expose its scoped token for the release-file commit")
    if checkout_options.get("ref") != "${{ github.sha }}":
        fail("publish must execute trusted local actions from the triggering control-plane SHA")
    public_worktree = next(
        (step for step in publish_steps if step.get("id") == "public-worktree"), None
    )
    if not isinstance(public_worktree, dict):
        fail("publish must authenticate a separate writable default-branch worktree")
    worktree_source = str(public_worktree.get("run", ""))
    for contract in (
        'trusted_pipeline_tree="$(git rev-parse "$GITHUB_SHA:.github")"',
        'default_pipeline_tree="$(git rev-parse "$default_sha:.github")"',
        'test "$default_pipeline_tree" = "$trusted_pipeline_tree"',
        "git worktree add --detach",
    ):
        if contract not in worktree_source:
            fail(f"publish default-branch worktree authentication is missing {contract}")
    release_files = next(
        (step for step in publish_steps if step.get("id") == "release-files"), None
    )
    public_metadata = next(
        (step for step in publish_steps if step.get("id") == "public-metadata"), None
    )
    if not isinstance(release_files, dict) or not isinstance(public_metadata, dict):
        fail("publish must synchronize and commit public release files")
    if ".github/tools/sync_release_files.py" not in str(release_files.get("run", "")):
        fail("publish must use the fixed release-file synchronizer")
    metadata_source = str(public_metadata.get("run", ""))
    for release_file in ("CHANGELOG.md", "README.md", "README-ZH.md"):
        if release_file not in metadata_source:
            fail(f"publish release-file allowlist is missing {release_file}")
    if "CHANGELOG-ZH.md" in metadata_source:
        fail("publish must not synchronize the private Chinese changelog")
    if "git config user.name 'moluopro'" not in metadata_source or (
        "git config user.email 'moluopro@qq.com'"
    ) not in metadata_source:
        fail("public release-file commits must use the product owner identity")
    if "github-actions[bot]" in metadata_source:
        fail("public release-file commits must not be attributed to an automation bot")
    release_transaction = next(
        (
            step
            for step in publish_steps
            if step.get("name") == "Commit the authenticated draft release transaction"
        ),
        None,
    )
    if not isinstance(release_transaction, dict):
        fail("publish must use the authenticated draft release transaction")
    transaction_source = str(release_transaction.get("run", ""))
    for contract in (
        ".github/tools/publish_release_transaction.py",
        '--source-sha "$SOURCE_SHA"',
        '--target-sha "$PUBLIC_SHA"',
        "--assets build/release",
    ):
        if contract not in transaction_source:
            fail(f"authenticated release transaction is missing {contract}")
    if release.get("permissions", {}).get("actions") != "read":
        fail("release.yml must allow called workflows to read package artifacts")
    if set(triggers(release)) != {"workflow_dispatch"}:
        fail("release.yml must expose only workflow_dispatch")
    release_inputs = triggers(release)["workflow_dispatch"].get("inputs", {})
    source_input = release_inputs.get("source_ref", {})
    if not source_input.get("required") or "default" in source_input:
        fail("release source_ref must be a required exact SHA without a mutable default")
    source_validation = next(
        (
            step
            for step in release_jobs["preflight"].get("steps", [])
            if step.get("name") == "Validate the immutable private source input"
        ),
        None,
    )
    if not isinstance(source_validation, dict) or "^[0-9a-f]{40}$" not in str(
        source_validation.get("run", "")
    ):
        fail("release preflight must require an exact 40-character private source SHA")
    preflight_steps = release_jobs["preflight"].get("steps", [])
    if not any(
        step.get("name") == "Verify the latest supported Godot stable frontend pin"
        for step in preflight_steps
    ):
        fail("release preflight must reject a stale supported Godot frontend pin")

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
    package_source = package_source_text
    if "gdpp-all.zip" in package_source or "gdpp-lite.zip" in package_source:
        fail("version-neutral Host ABI releases must publish only gdpp.zip")
    if "gdpp.zip" not in package_source:
        fail("package-release.yml must assemble and audit gdpp.zip")
    smoke_workflow = workflows["release-package-smoke.yml"]
    require_private_source_contract("release-package-smoke.yml", smoke_workflow)
    if smoke_workflow.get("permissions", {}).get("actions") != "read":
        fail("release-package-smoke.yml must read assembled package artifacts")
    if "workflow_dispatch" in triggers(smoke_workflow):
        fail("release package smoke cannot combine an arbitrary source with a cross-run artifact")
    smoke_source_text = (WORKFLOW_ROOT / "release-package-smoke.yml").read_text(
        encoding="utf-8"
    )
    if "artifact_run_id" in smoke_source_text:
        fail("release package smoke cannot accept an unauthenticated artifact run identifier")
    smoke_matrix = smoke_workflow["jobs"]["desktop"]["strategy"]["matrix"]["include"]
    actual_smokes = sorted((entry["archive"], entry["os"]) for entry in smoke_matrix)
    expected_smokes = sorted(
        (
            ("gdpp.zip", "macos-15"),
            ("gdpp.zip", "ubuntu-22.04"),
            ("gdpp.zip", "windows-2025"),
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

    compatibility_jobs = workflows["godot-compatibility.yml"]["jobs"]
    performance_steps = compatibility_jobs["external-release-performance"]["steps"]
    performance_build = next(
        (
            step
            for step in performance_steps
            if step.get("name")
            == "Build the macOS editor add-on and universal Release SDK"
        ),
        None,
    )
    if not isinstance(performance_build, dict) or (
        "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
        not in str(performance_build.get("run", ""))
    ):
        fail("external Release performance must build the macOS universal SDK")
    external_projects = compatibility_jobs["external-projects"]
    external_matrix = external_projects["strategy"]["matrix"]["include"]
    supplemental_projects = {
        entry["id"]: entry.get("supplemental_engine")
        for entry in external_matrix
        if entry.get("supplemental_engine") is not None
    }
    expected_supplemental_projects = {
        "konado": "4.7.2",
        "pixelorama": "4.7.2",
        "source-of-mana": "4.7.2",
        "dialogue-manager": "4.7.2",
    }
    if supplemental_projects != expected_supplemental_projects:
        fail(
            "the external-project matrix must supplement every declared 4.7 project "
            "with Godot 4.7.2"
        )
    external_steps = external_projects["steps"]
    steps_by_name = {
        step.get("name"): step for step in external_steps if step.get("name")
    }
    steps_by_id = {
        step.get("id"): step for step in external_steps if step.get("id")
    }
    primary_step = steps_by_name.get("Import, AOT build, export, audit and launch")
    supplemental_step = steps_by_name.get(
        "Run the source-identical supplemental complete-project lifecycle"
    )
    supplemental_rebuild = steps_by_name.get(
        "Rebuild the installed add-on for the supplemental Godot patch"
    )
    supplemental_setup = steps_by_id.get("supplemental_godot")
    if not all(
        isinstance(step, dict)
        for step in (
            primary_step,
            supplemental_step,
            supplemental_rebuild,
            supplemental_setup,
        )
    ):
        fail("external-project compatibility must declare primary and supplemental lifecycles")
    assert isinstance(primary_step, dict)
    assert isinstance(supplemental_step, dict)
    assert isinstance(supplemental_rebuild, dict)
    assert isinstance(supplemental_setup, dict)
    primary_command = str(primary_step.get("run", ""))
    supplemental_command = str(supplemental_step.get("run", ""))
    if "--mode primary" not in primary_command:
        fail("the declared-engine external-project lifecycle must be an explicit primary gate")
    for contract in (
        "--mode supplemental",
        "--primary-report build/external-e2e/${{ matrix.id }}/report.json",
        "--output build/external-e2e/${{ matrix.id }}-supplemental-",
    ):
        if contract not in supplemental_command:
            fail(f"the supplemental external-project lifecycle is missing {contract}")
    if external_steps.index(primary_step) >= external_steps.index(supplemental_setup):
        fail("the supplemental Godot patch cannot be installed before the primary lifecycle")
    supplemental_options = supplemental_setup.get("with", {})
    expected_supplemental_condition = (
        "${{ success() && matrix.supplemental_engine == '4.7.2' }}"
    )
    for step in (supplemental_setup, supplemental_rebuild, supplemental_step):
        if step.get("if") != expected_supplemental_condition:
            fail("the supplemental lifecycle must run only after a successful primary gate")
    if supplemental_options.get("version") != "${{ matrix.supplemental_engine }}" or (
        supplemental_options.get("edition")
        != "${{ steps.contract.outputs.edition }}"
    ):
        fail("the supplemental editor must preserve the matrix engine and declared edition")
    runtime_log_workflows = ("host-components.yml",)
    for name in runtime_log_workflows:
        source = (WORKFLOW_ROOT / name).read_text(encoding="utf-8")
        writes = source.count("Path(log_path).write_text(result.stdout")
        portable_writes = source.count(PORTABLE_RUNTIME_LOG)
        if writes == 0 or portable_writes != writes:
            fail(f"{name} must persist subprocess logs with portable LF line endings")

    smoke_source = smoke_source_text
    verifier = "$GITHUB_WORKSPACE/.github/tools/verify_release_assets.py"
    if smoke_source.count(verifier) != 1:
        fail("release package smoke must verify the downloaded package checksum once")
    release_source = (WORKFLOW_ROOT / "release.yml").read_text(encoding="utf-8")
    if release_source.count(verifier) != 1:
        fail("release publish must verify the downloaded package before opening its draft")
    if "Create checksums" in release_source or (
        "sha256sum -- gdpp.zip > SHA256SUMS" in release_source
    ):
        fail("publish must preserve rather than replace the package checksum manifest")
    publish_step_names = [step.get("name") for step in publish_steps]
    try:
        packaged_verification_index = publish_step_names.index(
            "Verify packaged release assets"
        )
        transaction_index = publish_steps.index(release_transaction)
    except ValueError as error:
        fail("publish must verify release assets before its draft transaction")
    if packaged_verification_index >= transaction_index:
        fail("release assets must be verified before the authenticated draft transaction")
    if smoke_source.count("$GITHUB_WORKSPACE/.github/tools/run_process.py") != 4:
        fail("release-package-smoke.yml must diagnose export and all runtime processes")
    if smoke_source.count("$GITHUB_WORKSPACE/.github/tools/check_log_contract.py") != 5:
        fail("release-package-smoke.yml must validate every portable log contract")
    if "GDPP_WINDOWS_PROCDUMP" in smoke_source or "procdump" in smoke_source.lower():
        fail("release package smoke must run customer binaries without an output-hiding wrapper")

    print(
        f"Validated {len(parallel)} parallel producers, one gated package stage, "
        "installed macOS/Linux/Windows package smokes, and one publish stage."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
