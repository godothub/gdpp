extends SceneTree


func _init() -> void:
    call_deferred("_run")


func _run() -> void:
    var compiler := GDPPCompiler.new()
    var engine := Engine.get_version_info()
    var target_version := "%d.%d" % [int(engine.major), int(engine.minor)]
    var project_output := ProjectSettings.globalize_path("res://addons/gdpp/build/project")
    if DirAccess.dir_exists_absolute(project_output) and not _remove_tree(project_output):
        push_error("GDPP could not reset the direct-build integration fixture")
        quit(1)
        return
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
    if result.has("build_commands") or result.has("native_up_to_date"):
        push_error("GDPP exposed retired serial build state: %s" % result.keys())
        quit(1)
        return
    var build_directory := ProjectSettings.globalize_path(str(result.get("build_directory", "")))
    var build_file := ProjectSettings.globalize_path(str(result.get("build_file", "")))
    var executor := ProjectSettings.globalize_path(str(result.get("build_executor", "")))
    if (
        not FileAccess.file_exists(build_file)
        or build_file.get_file() != "build.ninja"
        or not FileAccess.file_exists(executor)
        or int(result.get("compile_edge_count", 0)) < 2
        or int(result.get("post_compile_edge_count", 0)) < 1
    ):
        push_error("GDPP did not produce a complete Ninja build contract: %s" % result)
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
    var ninja_log_path := build_directory.path_join(".ninja_log")
    var first_log := FileAccess.get_file_as_string(ninja_log_path)
    if first_log.is_empty() or not _has_parallel_compiles(first_log):
        push_error("GDPP did not execute independent translation units in parallel")
        quit(1)
        return
    var first_library_time := FileAccess.get_modified_time(
        ProjectSettings.globalize_path(library)
    )
    var no_op: Dictionary = compiler.execute_project_build(result)
    if not no_op.get("success", false):
        push_error("GDPP no-op incremental build failed: %s" % no_op.get("diagnostics", []))
        quit(1)
        return
    if (
        FileAccess.get_file_as_string(ninja_log_path) != first_log
        or FileAccess.get_modified_time(ProjectSettings.globalize_path(library))
        != first_library_time
    ):
        push_error("GDPP rebuilt an unchanged native project")
        quit(1)
        return

    var generated_header := project_output.path_join("generated/hello_aot.gd.hpp")
    var original_header := FileAccess.get_file_as_string(generated_header)
    if original_header.is_empty():
        push_error("GDPP incremental fixture header is missing")
        quit(1)
        return
    OS.delay_msec(1100)
    if not _write_text(generated_header, original_header + "\n// depfile invalidation probe\n"):
        push_error("GDPP could not update the incremental fixture header")
        quit(1)
        return
    var header_rebuild: Dictionary = compiler.execute_project_build(result)
    var rebuilt_log := FileAccess.get_file_as_string(ninja_log_path)
    if not header_rebuild.get("success", false):
        push_error(
            "GDPP generated-header incremental build failed: %s"
            % header_rebuild.get("diagnostics", [])
        )
        quit(1)
        return
    var appended_log := rebuilt_log.substr(first_log.length())
    if (
        "hello_aot_gd_cpp." not in appended_log
        or "register_types_cpp." not in appended_log
        or "variant_ops_cpp." in appended_log
    ):
        push_error("GDPP depfile invalidation rebuilt the wrong translation units: %s" % appended_log)
        quit(1)
        return
    if not _write_text(generated_header, original_header):
        push_error("GDPP could not restore the incremental fixture header")
        quit(1)
        return
    var restored: Dictionary = compiler.execute_project_build(result)
    if not restored.get("success", false):
        push_error("GDPP could not restore the incremental fixture build")
        quit(1)
        return
    print("GDPP_DIRECT_EXPORT_BUILD_OK")
    quit(0)


func _has_parallel_compiles(log_text: String) -> bool:
    var intervals: Array[Vector2i] = []
    for line in log_text.split("\n"):
        var fields := line.split("\t")
        if fields.size() < 4:
            continue
        var output := str(fields[3])
        if not (output.ends_with(".o") or output.ends_with(".obj")):
            continue
        intervals.append(Vector2i(int(fields[0]), int(fields[1])))
    intervals.sort_custom(func(left: Vector2i, right: Vector2i) -> bool: return left.x < right.x)
    var latest_end := -1
    for interval in intervals:
        if interval.x < latest_end:
            return true
        latest_end = maxi(latest_end, interval.y)
    return false


func _write_text(path: String, content: String) -> bool:
    var file := FileAccess.open(path, FileAccess.WRITE)
    if file == null:
        return false
    file.store_string(content)
    return file.get_error() == OK


func _remove_tree(absolute: String) -> bool:
    var directory: DirAccess = DirAccess.open(absolute)
    if directory == null:
        return false
    directory.include_hidden = true
    directory.list_dir_begin()
    while true:
        var name := directory.get_next()
        if name.is_empty():
            break
        if name in [".", ".."]:
            continue
        var child := absolute.path_join(name)
        if directory.current_is_dir():
            if not _remove_tree(child):
                directory.list_dir_end()
                return false
        elif DirAccess.remove_absolute(child) != OK:
            directory.list_dir_end()
            return false
    directory.list_dir_end()
    directory = null
    return DirAccess.remove_absolute(absolute) == OK
