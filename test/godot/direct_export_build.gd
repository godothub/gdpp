extends SceneTree

var _progress_events: Array[Dictionary] = []
var _progress_on_main_thread := true


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
    var native_build_job_script := load("res://addons/gdpp/native_build_job.gd")
    var native_build_job: Object = native_build_job_script.new()
    _progress_events.clear()
    _progress_on_main_thread = true
    var outcome: Dictionary = native_build_job.run(
        compiler,
        {
            "project_root": "res://",
            "output_directory": project_output,
            "sdk_root": "res://addons/gdpp/sdk",
            "compiler_executable": compiler.get_default_compiler_executable(),
            "target_version": target_version,
            "build_profile": "release",
            "target_platform": compiler.get_host_platform(),
            "target_architecture": compiler.get_host_architecture(),
            "target_variant": "",
            "target_precision": "double" if OS.has_feature("double") else "single",
        },
        Callable(self, "_on_progress")
    )
    var result: Dictionary = outcome.get("plan", {})
    var execution: Dictionary = outcome.get("execution", {})
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
    if not execution.get("success", false):
        push_error("GDPP release compiler failed: %s" % execution.get("diagnostics", []))
        quit(1)
        return
    if not _progress_on_main_thread:
        push_error("GDPP delivered native build progress outside the Godot main thread")
        quit(1)
        return
    if not _validate_progress(_progress_events, true):
        push_error("GDPP did not stream monotonic per-file Ninja progress: %s" % _progress_events)
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
    _progress_events.clear()
    var no_op: Dictionary = compiler.execute_project_build(result)
    _drain_progress(compiler)
    if not no_op.get("success", false):
        push_error("GDPP no-op incremental build failed: %s" % no_op.get("diagnostics", []))
        quit(1)
        return
    if _progress_events.any(
        func(event: Dictionary) -> bool:
            return str(event.get("phase", "")) in ["compile", "link"]
    ):
        push_error("GDPP reported compiler work for a no-op Ninja build: %s" % _progress_events)
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
    _progress_events.clear()
    var header_rebuild: Dictionary = compiler.execute_project_build(result)
    _drain_progress(compiler)
    var rebuilt_log := FileAccess.get_file_as_string(ninja_log_path)
    if not header_rebuild.get("success", false):
        push_error(
            "GDPP generated-header incremental build failed: %s"
            % header_rebuild.get("diagnostics", [])
        )
        quit(1)
        return
    if not _validate_progress(_progress_events, true):
        push_error("GDPP lost incremental Ninja progress: %s" % _progress_events)
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

    var generated_source := project_output.path_join("generated/hello_aot.gd.cpp")
    var original_source := FileAccess.get_file_as_string(generated_source)
    OS.delay_msec(1100)
    if not _write_text(generated_source, original_source + "\nGDPP_INVALID_CPP_PROBE\n"):
        push_error("GDPP could not prepare the Ninja failure fixture")
        quit(1)
        return
    var expected_failure: Dictionary = compiler.execute_project_build(result)
    var failure_diagnostics := "\n".join(
        expected_failure.get("diagnostics", PackedStringArray()) as PackedStringArray
    )
    if (
        expected_failure.get("success", false)
        or int(expected_failure.get("exit_code", 0)) == 0
        or "parallel Ninja build failed" not in failure_diagnostics
        or "hello_aot.gd.cpp" not in failure_diagnostics
        or "@@GDPP_STATUS@@" in failure_diagnostics
    ):
        push_error("GDPP lost structured Ninja failure diagnostics: %s" % expected_failure)
        quit(1)
        return
    if not _write_text(generated_source, original_source):
        push_error("GDPP could not restore the Ninja failure fixture")
        quit(1)
        return
    var recovered: Dictionary = compiler.execute_project_build(result)
    if not recovered.get("success", false):
        push_error("GDPP could not recover after a failed Ninja edge")
        quit(1)
        return
    print("GDPP_DIRECT_EXPORT_BUILD_OK")
    quit(0)


func _drain_progress(compiler: Object) -> void:
    for value: Variant in compiler.drain_project_build_progress():
        _progress_events.append(value as Dictionary)


func _on_progress(phase: String, completed: int, total: int) -> void:
    _progress_on_main_thread = _progress_on_main_thread and Thread.is_main_thread()
    _progress_events.append({
        "phase": phase,
        "completed": completed,
        "total": total,
    })


func _validate_progress(events: Array[Dictionary], require_link: bool) -> bool:
    var compile_seen := 0
    var compile_completed := -1
    var compile_total := -1
    var link_seen := false
    for event in events:
        var phase := str(event.get("phase", ""))
        var completed := int(event.get("completed", -1))
        var total := int(event.get("total", -1))
        if completed < 0 or total <= 0 or completed > total:
            return false
        if phase == "compile":
            if completed < compile_completed or (compile_total >= 0 and total != compile_total):
                return false
            compile_seen += 1
            compile_completed = completed
            compile_total = total
        elif phase == "link":
            link_seen = true
    return (
        compile_seen >= 2
        and compile_completed == compile_total
        and (link_seen or not require_link)
    )


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
