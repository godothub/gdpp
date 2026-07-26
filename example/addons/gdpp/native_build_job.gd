@tool
extends RefCounted

const POLL_INTERVAL_MSEC := 16

var _thread: Thread
var _progress_mutex := Mutex.new()
var _progress_events: Array[Dictionary] = []


func run(
    compiler: Object,
    request: Dictionary,
    progress_callback: Callable = Callable(),
    frame_callback: Callable = Callable()
) -> Dictionary:
    if compiler == null:
        return _startup_failure("compiler service is not available")
    if _thread != null:
        return _startup_failure("native build job is already running")

    # ClassDB belongs to the editor thread. Capture all third-party GDExtension
    # contracts before the worker starts so compilation never reflects the live
    # editor registry from a background thread.
    compiler.prepare_project_build()

    _thread = Thread.new()
    var start_error := _thread.start(
        Callable(self, "_worker_entry").bind(compiler, request.duplicate(true))
    )
    if start_error != OK:
        _thread = null
        return _startup_failure(
            "cannot start the native build worker (error %d)" % start_error
        )

    while _thread.is_alive():
        _dispatch_progress(progress_callback)
        _advance_frame(frame_callback, false)
        _pump_editor()
        OS.delay_msec(POLL_INTERVAL_MSEC)

    var result: Variant = _thread.wait_to_finish()
    _thread = null
    _dispatch_progress(progress_callback)
    _advance_frame(frame_callback, true)
    _pump_editor()
    if result is Dictionary:
        return result
    return _startup_failure("native build worker returned an invalid result")


func _worker_entry(compiler: Object, request: Dictionary) -> Dictionary:
    var progress_callback := Callable(self, "_record_progress")
    var plan: Dictionary = compiler.compile_project(
        str(request.get("project_root", "res://")),
        str(request.get("output_directory", "")),
        str(request.get("sdk_root", "")),
        str(request.get("compiler_executable", "")),
        str(request.get("target_version", "4.4")),
        str(request.get("build_profile", "release")),
        str(request.get("target_platform", "")),
        str(request.get("target_architecture", "")),
        str(request.get("target_variant", "")),
        str(request.get("target_precision", "single")),
        progress_callback
    )
    if not plan.get("success", false):
        return {
            "plan": plan,
            "execution": {
                "success": false,
                "exit_code": -1,
                "diagnostics": plan.get("diagnostics", PackedStringArray()),
            },
        }

    var execution: Dictionary = compiler.execute_project_build(plan, progress_callback)
    return {
        "plan": plan,
        "execution": execution,
    }


func _record_progress(phase: String, completed: int, total: int) -> void:
    _progress_mutex.lock()
    _progress_events.push_back({
        "phase": phase,
        "completed": completed,
        "total": total,
    })
    _progress_mutex.unlock()


func _dispatch_progress(progress_callback: Callable) -> void:
    _progress_mutex.lock()
    var pending := _progress_events.duplicate(true)
    _progress_events.clear()
    _progress_mutex.unlock()
    if not progress_callback.is_valid():
        return
    for event: Dictionary in pending:
        progress_callback.call(
            str(event.get("phase", "compile")),
            int(event.get("completed", 0)),
            int(event.get("total", 0))
        )


func _pump_editor() -> void:
    DisplayServer.process_events()
    RenderingServer.force_draw()


func _advance_frame(frame_callback: Callable, snap_to_target: bool) -> void:
    if frame_callback.is_valid():
        frame_callback.call(snap_to_target)


func _startup_failure(message: String) -> Dictionary:
    var diagnostics := PackedStringArray([message])
    return {
        "plan": {
            "success": false,
            "diagnostics": diagnostics,
        },
        "execution": {
            "success": false,
            "exit_code": -1,
            "diagnostics": diagnostics,
        },
    }
