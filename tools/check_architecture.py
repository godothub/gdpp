#!/usr/bin/env python3
"""Enforce GDPP's module layout and compile-time dependency direction."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC_ROOT = ROOT / "include" / "gdpp"
SOURCE_ROOT = ROOT / "src"

PUBLIC_MODULES = {
    "codegen",
    "compiler",
    "core",
    "frontend",
    "ir",
    "numeric",
    "project",
    "runtime",
    "semantic",
    "support",
}
SOURCE_MODULES = (PUBLIC_MODULES - {"numeric"}) | {"cli", "integration"}

# A module may depend only on itself and layers to its left in the compiler pipeline. Project and
# host integration are orchestration layers, while the generated-code runtime remains isolated
# from the compiler implementation.
ALLOWED_DEPENDENCIES = {
    "core": {"core"},
    "numeric": {"numeric"},
    "support": {"support"},
    "frontend": {"core", "frontend", "numeric"},
    "semantic": {"core", "frontend", "semantic"},
    "ir": {"core", "frontend", "semantic", "ir", "numeric"},
    "codegen": {"core", "semantic", "ir", "codegen"},
    "compiler": {"core", "frontend", "semantic", "ir", "codegen", "compiler"},
    "project": {
        "core",
        "frontend",
        "semantic",
        "ir",
        "codegen",
        "compiler",
        "project",
        "support",
    },
    "runtime": {"runtime", "numeric"},
    "integration": PUBLIC_MODULES,
    "cli": PUBLIC_MODULES,
    "test": PUBLIC_MODULES,
}

INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s+[<"]gdpp/([^>"]+)[>"]')
DIRECT_GENERATED_VARIANT_PATTERN = re.compile(r"godot::Variant\((?!\))")


def source_module(path: Path) -> str | None:
    relative = path.relative_to(ROOT)
    if relative.parts[0] == "include" and relative.parts[1] == "gdpp":
        return relative.parts[2] if len(relative.parts) > 3 else None
    if relative.parts[0] == "src":
        return relative.parts[1] if len(relative.parts) > 2 else None
    if relative.parts[0] == "test":
        return "test"
    return None


def validate_layout(errors: list[str]) -> None:
    public_directories = {path.name for path in PUBLIC_ROOT.iterdir() if path.is_dir()}
    if public_directories != PUBLIC_MODULES:
        errors.append(
            "include/gdpp module set differs: "
            f"expected {sorted(PUBLIC_MODULES)}, got {sorted(public_directories)}"
        )

    flat_headers = sorted(
        path.relative_to(ROOT).as_posix()
        for path in PUBLIC_ROOT.iterdir()
        if path.is_file() and path.name != "version.hpp.in"
    )
    if flat_headers:
        errors.append("public headers must belong to a module: " + ", ".join(flat_headers))

    source_directories = {path.name for path in SOURCE_ROOT.iterdir() if path.is_dir()}
    if source_directories != SOURCE_MODULES:
        errors.append(
            "src module set differs: "
            f"expected {sorted(SOURCE_MODULES)}, got {sorted(source_directories)}"
        )

    flat_sources = sorted(
        path.relative_to(ROOT).as_posix() for path in SOURCE_ROOT.iterdir() if path.is_file()
    )
    if flat_sources:
        errors.append("source files must belong to a module: " + ", ".join(flat_sources))


def validate_dependencies(errors: list[str]) -> None:
    candidates = []
    for root in (PUBLIC_ROOT, SOURCE_ROOT, ROOT / "test"):
        candidates.extend(path for path in root.rglob("*") if path.suffix in {".cpp", ".hpp"})

    for path in sorted(candidates):
        owner = source_module(path)
        if owner is None or owner not in ALLOWED_DEPENDENCIES:
            errors.append(f"cannot determine module owner for {path.relative_to(ROOT)}")
            continue
        allowed = ALLOWED_DEPENDENCIES[owner]
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = INCLUDE_PATTERN.match(line)
            if match is None:
                continue
            include_path = match.group(1)
            if include_path == "version.hpp" or include_path.startswith("godot_api_data_"):
                continue
            if "/" not in include_path:
                errors.append(
                    f"{path.relative_to(ROOT)}:{line_number}: flat GDPP include {include_path}"
                )
                continue
            dependency = include_path.split("/", 1)[0]
            if dependency not in PUBLIC_MODULES:
                errors.append(
                    f"{path.relative_to(ROOT)}:{line_number}: unknown module {dependency}"
                )
            elif dependency not in allowed:
                errors.append(
                    f"{path.relative_to(ROOT)}:{line_number}: {owner} must not depend on "
                    f"{dependency}"
                )


def validate_generated_variant_boundaries(errors: list[str]) -> None:
    generator = SOURCE_ROOT / "codegen" / "cpp_generator.cpp"
    for line_number, line in enumerate(generator.read_text(encoding="utf-8").splitlines(), 1):
        if DIRECT_GENERATED_VARIANT_PATTERN.search(line):
            errors.append(
                f"{generator.relative_to(ROOT)}:{line_number}: generated native-to-Variant "
                "boundaries must use gdpp::runtime::to_variant"
            )


def validate_packed_conversion_contract(errors: list[str]) -> None:
    header = PUBLIC_ROOT / "runtime" / "reference_semantics.hpp"
    source = header.read_text(encoding="utf-8")
    required = (
        "explicit operator PackedArray&()",
        "explicit operator const PackedArray&()",
        "packed_native_argument(Value&& value)",
        "variant_constructor_argument(Value&& value)",
    )
    for contract in required:
        if contract not in source:
            errors.append(
                f"{header.relative_to(ROOT)}: missing PackedArray ABI contract {contract!r}"
            )


def validate_packed_subscript_contract(errors: list[str]) -> None:
    header = PUBLIC_ROOT / "runtime" / "variant_ops.hpp"
    source = header.read_text(encoding="utf-8")
    required = (
        "checked_typed_array_get",
        "using PackedArrayElement =",
        "PackedArrayElement<PackedArray>",
        "return target.native()[normalized];",
    )
    for contract in required:
        if contract not in source:
            errors.append(
                f"{header.relative_to(ROOT)}: missing native PackedArray subscript contract "
                f"{contract!r}"
            )
    if "return to_variant(target.native()[normalized]);" in source:
        errors.append(
            f"{header.relative_to(ROOT)}: statically typed PackedArray reads must not round-trip "
            "through Variant"
        )
    generator = SOURCE_ROOT / "codegen" / "cpp_generator.cpp"
    generator_source = generator.read_text(encoding="utf-8")
    packed_branch_start = generator_source.find(
        "} else if (container.is_packed_array()) {", generator_source.find("emit_subscript_read")
    )
    dictionary_branch_start = generator_source.find(
        "} else if (container.kind == TypeKind::dictionary)", packed_branch_start
    )
    packed_branch = generator_source[packed_branch_start:dictionary_branch_start]
    if (
        packed_branch_start < 0
        or dictionary_branch_start < 0
        or "return emit_conversion(result, result, std::move(value));" not in packed_branch
    ):
        errors.append(
            f"{generator.relative_to(ROOT)}: generated PackedArray reads must retain their "
            "native element type after bounds validation"
        )


def validate_local_signal_contract(errors: list[str]) -> None:
    header = PUBLIC_ROOT / "runtime" / "variant_ops.hpp"
    runtime = SOURCE_ROOT / "runtime" / "variant_ops.cpp"
    generator = SOURCE_ROOT / "codegen" / "cpp_generator.cpp"
    contracts = {
        header: (
            "emit_local_signal_variants",
            "emit_local_signal(godot::Object* owner, const godot::Variant& signal_name",
        ),
        runtime: (
            "classdb_get_method_bind(",
            "object_method_bind_call(",
        ),
        generator: (
            "gdpp::runtime::engine_lifetime_static_ptr([] { return godot::Variant{",
            '"gdpp::runtime::emit_local_signal_at("',
            "script_location(expression.span)",
        ),
    }
    for path, required in contracts.items():
        source = path.read_text(encoding="utf-8")
        for contract in required:
            if contract not in source:
                errors.append(
                    f"{path.relative_to(ROOT)}: missing local-signal hot-path contract "
                    f"{contract!r}"
                )


def validate_attached_method_dispatch_contract(errors: list[str]) -> None:
    header = PUBLIC_ROOT / "runtime" / "attached_script.hpp"
    registry = SOURCE_ROOT / "runtime" / "attached_script_registry.cpp"
    instance = SOURCE_ROOT / "runtime" / "attached_script_instance.cpp"
    generator = SOURCE_ROOT / "codegen" / "cpp_generator.cpp"
    contracts = {
        header: (
            "struct AttachedScriptMethodDispatch",
            "GDExtensionClassMethodCall call",
            "std::vector<AttachedScriptMethodDispatch> method_dispatches",
        ),
        registry: (
            "descriptor.method_dispatches.size() != descriptor.methods.size()",
            "append_unique(resolved.method_dispatches",
        ),
        instance: (
            "find_method_dispatch(",
            "dispatch->call(",
        ),
        generator: ("descriptor.method_dispatches.push_back({",),
    }
    for path, required in contracts.items():
        source = path.read_text(encoding="utf-8")
        for contract in required:
            if contract not in source:
                errors.append(
                    f"{path.relative_to(ROOT)}: missing attached-method direct-dispatch "
                    f"contract {contract!r}"
                )
    instance_source = instance.read_text(encoding="utf-8")
    if "target.callp(method" in instance_source:
        errors.append(
            f"{instance.relative_to(ROOT)}: attached callbacks must not repeat ClassDB dispatch "
            "through a temporary behavior Variant"
        )


def validate_attached_script_resource_contract(errors: list[str]) -> None:
    registry = SOURCE_ROOT / "runtime" / "attached_script_registry.cpp"
    source = registry.read_text(encoding="utf-8")
    required = (
        "std::mutex& script_resource_materialization_mutex()",
        "std::shared_mutex& script_resource_lifecycle_mutex()",
    )
    for contract in required:
        if contract not in source:
            errors.append(
                f"{registry.relative_to(ROOT)}: canonical attached Script resources must "
                f"retain the threaded single-publication contract {contract!r}"
            )
    shutdown = source.find("void unregister_all_attached_scripts()")
    if shutdown < 0 or "script_resource_lifecycle_mutex()" not in source[shutdown : shutdown + 900]:
        errors.append(
            f"{registry.relative_to(ROOT)}: attached Script shutdown must exclude concurrent "
            "resource publication"
        )
    for anchor in (
        "attached_script_resource(const godot::String& source_path",
        "attached_container_script_resource(const godot::String& source_path)",
    ):
        start = source.find(anchor)
        body = source[start : start + 2400] if start >= 0 else ""
        if (
            start < 0
            or "script_resource_lifecycle_mutex()" not in body
            or "script_resource_materialization_mutex()" not in body
        ):
            errors.append(
                f"{registry.relative_to(ROOT)}: {anchor!r} must synchronize canonical Script "
                "materialization with publication and shutdown"
            )


def validate_compile_time_branch_contract(errors: list[str]) -> None:
    header = PUBLIC_ROOT / "runtime" / "variant_ops.hpp"
    source = header.read_text(encoding="utf-8")
    required = (
        (
            "static godot::Ref<godot::Script> materialize()",
            "} else {\n            return {};\n        }",
        ),
        (
            "local_callable_argument(const LocalCallableArguments<Values...>& arguments)",
            "} else {\n        return {};\n    }",
        ),
        (
            "argument_count < RequiredArguments",
            "} else {\n            LocalCallableArguments<std::decay_t<Arguments>...> values",
        ),
    )
    for anchor, contract in required:
        start = source.find(anchor)
        if start < 0 or contract not in source[start : start + 2500]:
            errors.append(
                f"{header.relative_to(ROOT)}: compile-time branch {anchor!r} must keep "
                "returning alternatives structurally exclusive"
            )


def validate_performance_contract(errors: list[str]) -> None:
    path = ROOT / "test" / "performance" / "runtime_matrix.json"
    config = json.loads(path.read_text(encoding="utf-8"))
    thresholds = {
        "startup": config["startup"]["maximum_aot_regression_percent"],
        "frame": config["frame"]["maximum_aot_workload_regression_percent"],
    }
    thresholds.update(
        {
            f"case {name}": settings["maximum_aot_regression_percent"]
            for name, settings in config["cases"].items()
        }
    )
    for name, maximum in thresholds.items():
        if float(maximum) > 10.0:
            errors.append(
                f"{path.relative_to(ROOT)}: {name} permits {maximum}% AOT regression; "
                "the commercial ceiling is 10%"
            )


def main() -> int:
    errors: list[str] = []
    validate_layout(errors)
    validate_dependencies(errors)
    validate_generated_variant_boundaries(errors)
    validate_packed_conversion_contract(errors)
    validate_packed_subscript_contract(errors)
    validate_local_signal_contract(errors)
    validate_attached_method_dispatch_contract(errors)
    validate_attached_script_resource_contract(errors)
    validate_compile_time_branch_contract(errors)
    validate_performance_contract(errors)
    if errors:
        print("GDPP architecture validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("GDPP module layout and dependency direction are valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
