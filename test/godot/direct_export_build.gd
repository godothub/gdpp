extends SceneTree

func _init() -> void:
    call_deferred("_run")


func _run() -> void:
    var compiler := GDPPCompiler.new()
    var engine := Engine.get_version_info()
    var target_version := "%d.%d" % [int(engine.major), int(engine.minor)]
    var result: Dictionary = compiler.compile_project(
        "res://",
        "res://addons/gdpp/build/project",
        ProjectSettings.globalize_path("res://addons/gdpp/sdk"),
        compiler.get_default_compiler_executable(),
        target_version,
        "release",
        compiler.get_host_platform(),
        compiler.get_host_architecture(),
        "",
        "double" if OS.has_feature("double") else "single"
    )
    if not result.get("success", false):
        push_error("GDPP release planning failed: %s" % result.get("diagnostics", []))
        quit(1)
        return
    var execution: Dictionary = compiler.execute_project_build(result)
    if not execution.get("success", false):
        push_error("GDPP release compiler failed: %s" % execution.get("diagnostics", []))
        quit(1)
        return
    var library := str(result.get("output_library", ""))
    if not FileAccess.file_exists(library) or ".release." not in library:
        push_error("GDPP release library was not produced: %s" % library)
        quit(1)
        return
    var classes: Dictionary = result.get("script_classes", {})
    if not str(classes.get("res://hello.gd", "")).begins_with("GDPPNative_"):
        push_error("GDPP did not expose collision-free native script class names")
        quit(1)
        return
    print("GDPP_DIRECT_EXPORT_BUILD_OK")
    quit(0)
