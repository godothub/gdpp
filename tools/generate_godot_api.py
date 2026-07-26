#!/usr/bin/env python3
"""Generate deterministic C++ metadata from Godot's extension_api.json."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


# extension_api.json exposes C++ wrapper placeholders for vararg utilities rather than their
# runtime minimum arity. Keep this source-audited list exhaustive so upstream additions cannot
# silently inherit an unsafe argument contract.
VARARG_MINIMUM_ARGUMENTS = {
    "max": 2,
    "min": 2,
    "str": 0,
    "print": 0,
    "print_rich": 0,
    "printerr": 0,
    "printt": 0,
    "prints": 0,
    "printraw": 0,
    "print_verbose": 0,
    "push_error": 0,
    "push_warning": 0,
}

SUPPORTED_NATIVE_META = {
    "",
    "real_t",
    "float",
    "double",
    "int8",
    "int16",
    "int32",
    "int64",
    "uint8",
    "uint16",
    "uint32",
    "uint64",
    "char16",
    "char32",
    "required",
}


def checked_meta(value: str, context: str) -> str:
    if value not in SUPPORTED_NATIVE_META:
        raise ValueError(f"unreviewed Godot native meta {value!r} in {context}")
    return value


def cpp_string(value: str | None) -> str:
    return json.dumps(value or "", ensure_ascii=True)


def cpp_int64(value: int) -> str:
    if value == -(2**63):
        return "INT64_MIN"
    return f"INT64_C({value})"


def method_record(owner: str, method: dict, builtin: bool) -> tuple:
    arguments = method.get("arguments", [])
    required = sum("default_value" not in argument for argument in arguments)
    return_type = (
        method.get("return_type", "void")
        if builtin
        else method.get("return_value", {}).get("type", "void")
    )
    return_meta = checked_meta(
        "real_t" if builtin and return_type == "float"
        else method.get("return_value", {}).get("meta", ""),
        f"{owner}.{method['name']} return",
    )
    return (
        owner,
        method["name"],
        return_type,
        return_meta,
        required,
        len(arguments),
        bool(method.get("is_static", False)),
        bool(method.get("is_vararg", False)),
        bool(method.get("is_const", False)),
        bool(method.get("is_virtual", False)),
        tuple(
            (
                argument.get("name", ""),
                argument.get("type", "Variant"),
                checked_meta(
                    argument.get(
                        "meta", "real_t" if builtin and argument.get("type") == "float" else ""
                    ),
                    f"{owner}.{method['name']} argument {argument.get('name', '')}",
                ),
                "default_value" in argument,
            )
            for argument in arguments
        ),
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--namespace", required=True)
    arguments = parser.parse_args()

    data = json.loads(arguments.input.read_text(encoding="utf-8"))
    classes: list[tuple[str, str, bool, bool]] = []
    class_constants: list[tuple[str, str, str, int, bool]] = []
    methods: list[tuple] = []
    constructors: list[tuple[str, tuple[tuple[str, str, str, bool], ...]]] = []
    properties: list[tuple[str, str, str, str, str, bool, int]] = []
    signals: list[tuple[str, str, tuple[tuple[str, str, str, bool], ...]]] = []
    utility_functions: list[tuple] = []
    global_constants: list[tuple[str, int]] = []
    global_enum_values: list[tuple[str, str, int, bool]] = []
    builtin_operators: list[tuple[str, str, str, str]] = []
    builtin_constants: list[tuple[str, str, str, str]] = []
    singletons = sorted(
        ((item["name"], item.get("type", item["name"])) for item in data.get("singletons", [])),
        key=lambda value: value[0],
    )

    for item in data.get("classes", []):
        owner = item["name"]
        method_names = {method["name"] for method in item.get("methods", [])}
        classes.append((owner, item.get("inherits", ""), False, item.get("api_type") == "editor"))
        class_constants.extend(
            (owner, "", constant["name"], int(constant["value"]), False)
            for constant in item.get("constants", [])
        )
        for enumeration in item.get("enums", []):
            class_constants.extend(
                (
                    owner,
                    enumeration["name"],
                    value["name"],
                    int(value["value"]),
                    bool(enumeration.get("is_bitfield", False)),
                )
                for value in enumeration.get("values", [])
            )
        methods.extend(method_record(owner, method, False) for method in item.get("methods", []))
        signals.extend(
            (
                owner,
                signal["name"],
                tuple(
                    (
                        argument.get("name", ""),
                        argument.get("type", "Variant"),
                        checked_meta(
                            argument.get("meta", ""),
                            f"{owner}.{signal['name']} argument {argument.get('name', '')}",
                        ),
                        False,
                    )
                    for argument in signal.get("arguments", [])
                ),
            )
            for signal in item.get("signals", [])
        )
        for prop in item.get("properties", []):
            getter = prop.get("getter", "")
            setter = prop.get("setter", "")
            if getter.startswith("_") and getter[1:] in method_names:
                getter = getter[1:]
            if setter.startswith("_") and setter[1:] in method_names:
                setter = setter[1:]
            properties.append(
                (
                    owner,
                    prop["name"],
                    prop.get("type", "Variant"),
                    getter,
                    setter,
                    False,
                    int(prop.get("index", -1)),
                )
            )

    for item in data.get("builtin_classes", []):
        owner = item["name"]
        if not owner:
            continue
        classes.append((owner, "", True, False))
        methods.extend(method_record(owner, method, True) for method in item.get("methods", []))
        for constructor in item.get("constructors", []):
            constructors.append(
                (
                    owner,
                    tuple(
                        (
                            argument.get("name", ""),
                            argument.get("type", "Variant"),
                            checked_meta(
                                argument.get(
                                    "meta",
                                    "real_t" if argument.get("type") == "float" else "",
                                ),
                                f"{owner} constructor argument {argument.get('name', '')}",
                            ),
                            False,
                        )
                        for argument in constructor.get("arguments", [])
                    ),
                )
            )
        for member in item.get("members", []):
            properties.append(
                (owner, member["name"], member.get("type", "Variant"), "", "", True, -1)
            )
        for constant in item.get("constants", []):
            builtin_constants.append(
                (owner, constant["name"], constant.get("type", owner), constant["value"])
            )
        for operator in item.get("operators", []):
            builtin_operators.append(
                (
                    owner,
                    operator["name"],
                    operator.get("right_type", ""),
                    operator.get("return_type", "Variant"),
                )
            )

    for function in data.get("utility_functions", []):
        arguments_list = function.get("arguments", [])
        required = sum("default_value" not in argument for argument in arguments_list)
        is_vararg = bool(function.get("is_vararg", False))
        if is_vararg:
            try:
                required = VARARG_MINIMUM_ARGUMENTS[function["name"]]
            except KeyError as error:
                raise ValueError(
                    "unreviewed Godot vararg utility function: " + function["name"]
                ) from error
        utility_functions.append(
            (
                function["name"],
                function.get("return_type", "void"),
                required,
                len(arguments_list),
                is_vararg,
                function.get("category") == "math",
                tuple(
                    (
                        argument.get("name", ""),
                        argument.get("type", "Variant"),
                        checked_meta(
                            argument.get("meta", ""),
                            f"{function['name']} argument {argument.get('name', '')}",
                        ),
                        "default_value" in argument,
                    )
                    for argument in arguments_list
                ),
            )
        )

    for constant in data.get("global_constants", []):
        global_constants.append((constant["name"], int(constant["value"])))

    for enumeration in data.get("global_enums", []):
        for value in enumeration.get("values", []):
            global_enum_values.append(
                (
                    enumeration["name"],
                    value["name"],
                    int(value["value"]),
                    bool(enumeration.get("is_bitfield", False)),
                )
            )

    classes.sort(key=lambda value: value[0])
    class_constants.sort(key=lambda value: (value[0], value[1], value[2]))
    methods.sort(key=lambda value: (value[0], value[1]))
    constructors.sort(key=lambda value: (value[0], len(value[1]), value[1]))
    properties.sort(key=lambda value: (value[0], value[1]))
    signals.sort(key=lambda value: (value[0], value[1]))
    utility_functions.sort(key=lambda value: value[0])
    global_constants.sort(key=lambda value: value[0])
    global_enum_values.sort(key=lambda value: (value[0], value[1]))
    builtin_operators.sort(key=lambda value: (value[0], value[1], value[2]))
    builtin_constants.sort(key=lambda value: (value[0], value[1]))
    header = data["header"]
    version = f'{header["version_major"]}.{header["version_minor"]}.{header["version_patch"]}'

    lines = [
        "// Generated from extension_api.json. Do not edit.",
        f"namespace gdpp::{arguments.namespace} {{",
        f'inline constexpr const char* version = {cpp_string(version)};',
        "inline constexpr GodotClassRecord classes[] = {",
    ]
    for name, inherits, builtin, editor_only in classes:
        lines.append(
            f"    {{{cpp_string(name)}, {cpp_string(inherits)}, {str(builtin).lower()}, "
            f"{str(editor_only).lower()}}},"
        )
    lines.append("};")
    lines.append(f"inline constexpr std::size_t class_constant_count = {len(class_constants)};")
    lines.append("inline constexpr GodotClassConstantRecord class_constants[] = {")
    for owner, enum_name, name, value, bitfield in class_constants:
        lines.append(
            f"    {{{cpp_string(owner)}, {cpp_string(enum_name)}, {cpp_string(name)}, "
            f"{cpp_int64(value)}, {str(bitfield).lower()}}},"
        )
    if not class_constants:
        lines.append('    {"", "", "", INT64_C(0), false},')
    lines.append("};")
    lines.append("inline constexpr GodotSingletonRecord singletons[] = {")
    for name, type_name in singletons:
        lines.append(f"    {{{cpp_string(name)}, {cpp_string(type_name)}}},")
    lines.append("};")
    lines.append("inline constexpr GodotMethodRecord methods[] = {")
    generated_arguments: list[tuple[str, str, str, bool]] = []
    for record in methods:
        (
            owner,
            name,
            result,
            result_meta,
            required,
            maximum,
            static,
            vararg,
            is_const,
            is_virtual,
            args,
        ) = record
        first_argument = len(generated_arguments)
        generated_arguments.extend(args)
        lines.append(
            "    {"
            f"{cpp_string(owner)}, {cpp_string(name)}, {cpp_string(result)}, "
            f"{cpp_string(result_meta)}, "
            f"{required}, {maximum}, {first_argument}, {str(static).lower()}, {str(vararg).lower()}, "
            f"{str(is_const).lower()}, {str(is_virtual).lower()}"
            "},"
        )
    lines.append("};")
    lines.append("inline constexpr GodotConstructorRecord constructors[] = {")
    for owner, args in constructors:
        first_argument = len(generated_arguments)
        generated_arguments.extend(args)
        lines.append(
            f"    {{{cpp_string(owner)}, {first_argument}, {len(args)}}},"
        )
    lines.append("};")
    lines.append(f"inline constexpr std::size_t utility_function_count = {len(utility_functions)};")
    lines.append("inline constexpr GodotUtilityFunctionRecord utility_functions[] = {")
    for name, result, required, maximum, vararg, is_constant, args in utility_functions:
        first_argument = len(generated_arguments)
        generated_arguments.extend(args)
        lines.append(
            "    {"
            f"{cpp_string(name)}, {cpp_string(result)}, {required}, {maximum}, "
            f"{first_argument}, {str(vararg).lower()}, {str(is_constant).lower()}"
            "},"
        )
    if not utility_functions:
        lines.append('    {"", "void", 0, 0, 0, false, false},')
    lines.append("};")
    generated_signals: list[tuple[str, str, int, int]] = []
    for owner, name, args in signals:
        first_argument = len(generated_arguments)
        generated_arguments.extend(args)
        generated_signals.append((owner, name, first_argument, len(args)))
    lines.append("inline constexpr GodotArgumentRecord arguments[] = {")
    for name, type_name, meta, has_default in generated_arguments:
        lines.append(
            f"    {{{cpp_string(name)}, {cpp_string(type_name)}, {cpp_string(meta)}, "
            f"{str(has_default).lower()}}},"
        )
    lines.append("};")
    lines.append(f"inline constexpr std::size_t global_constant_count = {len(global_constants)};")
    lines.append("inline constexpr GodotGlobalConstantRecord global_constants[] = {")
    for name, value in global_constants:
        lines.append(f"    {{{cpp_string(name)}, {cpp_int64(value)}}},")
    if not global_constants:
        lines.append('    {"", INT64_C(0)},')
    lines.append("};")
    lines.append(
        f"inline constexpr std::size_t global_enum_value_count = {len(global_enum_values)};"
    )
    lines.append("inline constexpr GodotGlobalEnumValueRecord global_enum_values[] = {")
    for owner, name, value, bitfield in global_enum_values:
        lines.append(
            f"    {{{cpp_string(owner)}, {cpp_string(name)}, {cpp_int64(value)}, "
            f"{str(bitfield).lower()}}},"
        )
    if not global_enum_values:
        lines.append('    {"", "", INT64_C(0), false},')
    lines.append("};")
    lines.append(f"inline constexpr std::size_t builtin_operator_count = {len(builtin_operators)};")
    lines.append("inline constexpr GodotBuiltinOperatorRecord builtin_operators[] = {")
    for left, name, right, result in builtin_operators:
        lines.append(
            f"    {{{cpp_string(left)}, {cpp_string(name)}, {cpp_string(right)}, "
            f"{cpp_string(result)}}},"
        )
    if not builtin_operators:
        lines.append('    {"", "", "", "Variant"},')
    lines.append("};")
    lines.append(f"inline constexpr std::size_t builtin_constant_count = {len(builtin_constants)};")
    lines.append("inline constexpr GodotBuiltinConstantRecord builtin_constants[] = {")
    for owner, name, type_name, value in builtin_constants:
        lines.append(
            f"    {{{cpp_string(owner)}, {cpp_string(name)}, {cpp_string(type_name)}, "
            f"{cpp_string(value)}}},"
        )
    if not builtin_constants:
        lines.append('    {"", "", "Variant", "Variant()"},')
    lines.append("};")
    lines.append("inline constexpr GodotSignalRecord signals[] = {")
    for owner, name, first_argument, argument_count in generated_signals:
        lines.append(
            f"    {{{cpp_string(owner)}, {cpp_string(name)}, {first_argument}, "
            f"{argument_count}}},"
        )
    if not signals:
        lines.append('    {"", "", 0, 0},')
    lines.append("};")
    lines.append("inline constexpr GodotPropertyRecord properties[] = {")
    for owner, name, type_name, getter, setter, direct, index in properties:
        lines.append(
            "    {"
            f"{cpp_string(owner)}, {cpp_string(name)}, {cpp_string(type_name)}, "
            f"{cpp_string(getter)}, {cpp_string(setter)}, {str(direct).lower()}, {index}"
            "},"
        )
    lines.extend(["};", f"}} // namespace gdpp::{arguments.namespace}", ""])

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    content = "\n".join(lines)
    if not arguments.output.exists() or arguments.output.read_text(encoding="utf-8") != content:
        arguments.output.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
