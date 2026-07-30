#!/usr/bin/env ruby
# frozen_string_literal: true

require "yaml"

ROOT = File.expand_path("..", __dir__)
WORKFLOW_ROOT = File.join(ROOT, ".github", "workflows")
RELEASE_PATH = File.join(WORKFLOW_ROOT, "release.yml")

def fail_check(message)
  warn("workflow topology: #{message}")
  exit(1)
end

def load_workflow(path)
  YAML.safe_load(File.read(path), aliases: true)
rescue Psych::Exception => error
  fail_check("#{path}: #{error.message}")
end

def triggers(workflow)
  # YAML 1.1 treats the unquoted GitHub key `on` as boolean true.
  workflow["on"] || workflow[true] || {}
end

def needs(job)
  Array(job["needs"])
end

workflows = Dir[File.join(WORKFLOW_ROOT, "*.yml")].sort.to_h do |path|
  [File.basename(path), load_workflow(path)]
end
release = workflows.fetch("release.yml")
release_jobs = release.fetch("jobs")

workflows.each do |name, workflow|
  contents = workflow.fetch("permissions").fetch("contents")
  fail_check("#{name} must default to contents: read") unless contents == "read"
end

publish_permissions = release_jobs.fetch("publish").fetch("permissions")
fail_check("only the publish job may request contents: write") unless
  publish_permissions.fetch("contents") == "write"

parallel_workflows = {
  "quality-gate" => "quality.yml",
  "core-gate" => "core.yml",
  "native-gate" => "native-integration.yml",
  "godot-gate" => "godot-compatibility.yml",
  "android-gate" => "android.yml",
  "web-gate" => "web.yml",
  "ios-gate" => "ios.yml",
  "host-components" => "host-components.yml",
}.freeze

parallel_workflows.each do |job_name, workflow_name|
  workflow_triggers = triggers(workflows.fetch(workflow_name))
  fail_check("#{workflow_name} must support workflow_call") unless
    workflow_triggers.key?("workflow_call")
  fail_check("#{workflow_name} must support standalone workflow_dispatch") unless
    workflow_triggers.key?("workflow_dispatch")

  job = release_jobs.fetch(job_name)
  fail_check("#{job_name} must invoke #{workflow_name}") unless
    job["uses"] == "./.github/workflows/#{workflow_name}"
  fail_check("#{job_name} must start immediately after preflight") unless
    needs(job) == ["preflight"]
end

package_workflow = workflows.fetch("package-release.yml")
package_triggers = triggers(package_workflow)
fail_check("package-release.yml must support workflow_call") unless
  package_triggers.key?("workflow_call")
fail_check("package-release.yml cannot run without producer artifacts") if
  package_triggers.key?("workflow_dispatch")

package_job = release_jobs.fetch("packages")
fail_check("packages must invoke package-release.yml") unless
  package_job["uses"] == "./.github/workflows/package-release.yml"
expected_package_needs = parallel_workflows.keys.sort
fail_check("packages must wait for every test and component producer") unless
  needs(package_job).sort == expected_package_needs

package_smoke_workflow = workflows.fetch("release-package-smoke.yml")
package_smoke_triggers = triggers(package_smoke_workflow)
fail_check("release-package-smoke.yml must support workflow_call") unless
  package_smoke_triggers.key?("workflow_call")
fail_check("release-package-smoke.yml cannot run without assembled package artifacts") if
  package_smoke_triggers.key?("workflow_dispatch")
smoke_matrix = package_smoke_workflow.fetch("jobs").fetch("desktop")
  .fetch("strategy").fetch("matrix").fetch("include")
smoke_pairs = smoke_matrix.map do |entry|
  [entry.fetch("archive"), entry.fetch("os")]
end.sort
expected_smoke_pairs = [
  ["gdpp.zip", "macos-15"],
  ["gdpp.zip", "ubuntu-22.04"],
  ["gdpp.zip", "windows-2025"],
  ["gdpp-lite.zip", "macos-15"],
  ["gdpp-lite.zip", "windows-2025"],
].sort
fail_check("installed package smoke matrix is incomplete") unless
  smoke_pairs == expected_smoke_pairs

package_smoke_job = release_jobs.fetch("package-smoke")
fail_check("package-smoke must invoke release-package-smoke.yml") unless
  package_smoke_job["uses"] == "./.github/workflows/release-package-smoke.yml"
fail_check("package-smoke must wait only for assembled release packages") unless
  needs(package_smoke_job) == ["packages"]

readiness_job = release_jobs.fetch("readiness")
expected_readiness_needs = (parallel_workflows.keys + ["packages", "package-smoke"]).sort
fail_check("readiness must aggregate every delivery result") unless
  needs(readiness_job).sort == expected_readiness_needs
fail_check("publish must depend only on the aggregate readiness gate") unless
  needs(release_jobs.fetch("publish")) == ["readiness"]

pull_request_workflows = workflows.filter_map do |name, workflow|
  name if triggers(workflow).key?("pull_request")
end
fail_check("release.yml must be the only pull-request entrypoint") unless
  pull_request_workflows == ["release.yml"]

puts(
  "Validated #{parallel_workflows.length} parallel producers, " \
  "one gated package stage, installed macOS/Linux/Windows package smokes, and one publish stage.",
)
