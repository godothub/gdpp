extends SceneTree

const TRANSFORMER := preload("res://addons/gdpp/export_plugin.gd")


func _initialize() -> void:
    var arguments := OS.get_cmdline_user_args()
    if arguments.size() != 2:
        push_error("GDPP isolated resource transformer requires state and result paths")
        quit(2)
        return
    call_deferred(&"_run_worker", arguments)


func _run_worker(arguments: PackedStringArray) -> void:
    # SceneTree main scripts start before the editor finishes constructing its filesystem and
    # server state. Exiting from _initialize() aborts that startup and produces false leak/scan
    # diagnostics. Wait for the editor-owned scan to settle, then give deferred initialization one
    # complete frame before loading customer resources.
    var filesystem := EditorInterface.get_resource_filesystem()
    while filesystem != null and filesystem.is_scanning():
        await process_frame
    await process_frame

    var state_path := arguments[0]
    var result_path := arguments[1]
    var state_file := FileAccess.open(state_path, FileAccess.READ)
    if state_file == null:
        push_error("GDPP isolated resource transformer cannot open its state")
        quit(3)
        return
    var state: Variant = state_file.get_var(true)
    var state_error := state_file.get_error()
    state_file = null
    if state_error != OK or not (state is Dictionary):
        push_error("GDPP isolated resource transformer received malformed state")
        quit(4)
        return
    var worker_state: Dictionary = state
    if int(worker_state.get("schema", 0)) != 1:
        push_error("GDPP isolated resource transformer received an unsupported state")
        quit(4)
        return

    var compiler := GDPPCompiler.new()
    var transformer := TRANSFORMER.new()
    var setup: Dictionary = transformer.prepare_isolated_transform_worker(
        compiler,
        worker_state
    )
    var result: Dictionary
    if bool(setup.get("success", false)):
        result = transformer.run_isolated_transform_worker(worker_state)
    else:
        result = {
            "schema": 1,
            "success": false,
            "error": str(setup.get("error", "isolated transformer setup failed")),
        }

    var result_file := FileAccess.open(result_path, FileAccess.WRITE)
    if result_file == null:
        push_error("GDPP isolated resource transformer cannot create its result")
        quit(5)
        return
    result_file.store_var(result, true)
    result_file.flush()
    var result_error := result_file.get_error()
    result_file = null
    if result_error != OK:
        push_error("GDPP isolated resource transformer cannot commit its result")
        quit(6)
        return
    transformer.finish_isolated_transform_worker()
    transformer = null
    compiler = null
    state = null
    worker_state.clear()
    # Release RefCounted resources and queued server state before the editor shutdown path begins.
    await process_frame
    await process_frame
    print("GDPP_EXPORT_TRANSFORM_WORKER_COMMITTED")
    quit(0)
