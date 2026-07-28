extends SceneTree


func _init() -> void:
    call_deferred("_run")


func _run() -> void:
    var compiler := GDPPCompiler.new()
    var sequence_path := ProjectSettings.globalize_path(
        "res://addons/gdpp/build/toolchain-execution-order.txt"
    )
    var commands: Array[Dictionary] = []
    if compiler.get_host_platform() == "windows":
        var escaped_path := sequence_path.replace("'", "''")
        commands = [
            _command(
                "powershell.exe",
                PackedStringArray([
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    (
                        "Set-Content -LiteralPath '%s' -Value 'first'; "
                        + "Start-Sleep -Milliseconds 500; "
                        + "Add-Content -LiteralPath '%s' -Value 'done'"
                    ) % [escaped_path, escaped_path],
                ])
            ),
            _command(
                "powershell.exe",
                PackedStringArray([
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "Add-Content -LiteralPath '%s' -Value 'second'" % escaped_path,
                ])
            ),
            _command(
                "powershell.exe",
                PackedStringArray([
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "[Console]::Error.WriteLine('GDPP_CAPTURE_SENTINEL'); exit 23",
                ])
            ),
        ]
    else:
        var escaped_path := sequence_path.replace('"', '\\"')
        commands = [
            _command(
                "/bin/sh",
                PackedStringArray([
                    "-c",
                    (
                        'printf "first\\n" > "%s"; sleep 0.5; '
                        + 'printf "done\\n" >> "%s"'
                    ) % [escaped_path, escaped_path],
                ])
            ),
            _command(
                "/bin/sh",
                PackedStringArray([
                    "-c",
                    'printf "second\\n" >> "%s"' % escaped_path,
                ])
            ),
            _command(
                "/bin/sh",
                PackedStringArray([
                    "-c",
                    'printf "GDPP_CAPTURE_SENTINEL\\n" >&2; exit 23',
                ])
            ),
        ]

    var result: Dictionary = compiler.execute_project_build({
        "success": true,
        "build_commands": commands,
        "output_library": "res://addons/gdpp/build/unreachable-native-library",
        "diagnostics": PackedStringArray([
            "GDPP_PLAN_WARNING_SENTINEL",
            "GDPP_PLAN_NOTE_SENTINEL",
        ]),
        "errors": PackedStringArray(),
        "warnings": PackedStringArray(["GDPP_PLAN_WARNING_SENTINEL"]),
        "notes": PackedStringArray(["GDPP_PLAN_NOTE_SENTINEL"]),
    })
    if result.get("success", false) or int(result.get("exit_code", -1)) != 23:
        push_error("GDPP accepted a deliberately failing toolchain command: %s" % result)
        quit(1)
        return
    var sequence_text := FileAccess.get_file_as_string(sequence_path)
    sequence_text = sequence_text.replace("\r\n", "\n").replace("\r", "\n")
    var sequence := sequence_text.strip_edges().split("\n")
    if sequence != PackedStringArray(["first", "done", "second"]):
        push_error("GDPP native toolchain commands were not strictly serialized: %s" % sequence)
        quit(1)
        return
    var diagnostic_text := "\n".join(
        result.get("diagnostics", PackedStringArray()) as PackedStringArray
    )
    if "failed with exit code 23" not in diagnostic_text:
        push_error("GDPP toolchain diagnostics lost the failing exit code: %s" % diagnostic_text)
        quit(1)
        return
    if "GDPP_CAPTURE_SENTINEL" not in diagnostic_text:
        push_error("GDPP toolchain diagnostics lost stderr: %s" % diagnostic_text)
        quit(1)
        return
    var error_text := "\n".join(
        result.get("errors", PackedStringArray()) as PackedStringArray
    )
    if "GDPP_CAPTURE_SENTINEL" not in error_text:
        push_error("GDPP toolchain errors lost stderr: %s" % error_text)
        quit(1)
        return
    if "GDPP_PLAN_WARNING_SENTINEL" in error_text:
        push_error("GDPP promoted a compiler warning after a toolchain failure: %s" % error_text)
        quit(1)
        return
    if (
        result.get("warnings", PackedStringArray())
        != PackedStringArray(["GDPP_PLAN_WARNING_SENTINEL"])
    ):
        push_error("GDPP lost structured compiler warnings: %s" % result)
        quit(1)
        return
    if (
        result.get("notes", PackedStringArray())
        != PackedStringArray(["GDPP_PLAN_NOTE_SENTINEL"])
    ):
        push_error("GDPP lost structured compiler notes: %s" % result)
        quit(1)
        return
    DirAccess.remove_absolute(sequence_path)
    print("GDPP_TOOLCHAIN_EXECUTION_OK")
    quit(0)


func _command(executable: String, arguments: PackedStringArray) -> Dictionary:
    return {
        "executable": executable,
        "arguments": arguments,
        "stage": 0,
    }
