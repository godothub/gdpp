extends SceneTree


func _init() -> void:
    call_deferred("_run")


func _run() -> void:
    var compiler := GDPPCompiler.new()
    var sentinel_path := ProjectSettings.globalize_path(
        "res://addons/gdpp/build/legacy-toolchain-command-ran.txt"
    )
    DirAccess.remove_absolute(sentinel_path)
    var executable := "cmd.exe" if compiler.get_host_platform() == "windows" else "/bin/sh"
    var arguments := (
        PackedStringArray(["/c", "echo unsafe>\"%s\"" % sentinel_path])
        if compiler.get_host_platform() == "windows"
        else PackedStringArray(["-c", 'printf unsafe > "%s"' % sentinel_path])
    )
    var result: Dictionary = compiler.execute_project_build({
        "success": true,
        "build_commands": [{
            "executable": executable,
            "arguments": arguments,
            "stage": 0,
        }],
        "output_library": "res://addons/gdpp/build/unreachable-native-library",
        "diagnostics": PackedStringArray(),
        "errors": PackedStringArray(),
        "warnings": PackedStringArray(),
        "notes": PackedStringArray(),
    })
    if result.get("success", false):
        push_error("GDPP accepted a legacy arbitrary-command build plan: %s" % result)
        quit(1)
        return
    if FileAccess.file_exists(sentinel_path):
        push_error("GDPP executed a legacy arbitrary toolchain command")
        quit(1)
        return
    var diagnostics := "\n".join(
        result.get("diagnostics", PackedStringArray()) as PackedStringArray
    )
    if "required bundled Ninja executor" not in diagnostics:
        push_error("GDPP lost the Ninja-only build-plan diagnostic: %s" % diagnostics)
        quit(1)
        return
    print("GDPP_TOOLCHAIN_EXECUTION_OK")
    quit(0)
