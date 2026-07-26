extends SceneTree

const AWAIT_DEFAULT_PROBE := preload("res://await_default_probe.gd")


func _initialize() -> void:
    call_deferred(&"_run")


func _run() -> void:
    var probe: Variant = AWAIT_DEFAULT_PROBE.new()
    var failure: String = await probe.verify(self)
    if not failure.is_empty():
        push_error(failure)
        quit(1)
        return
    print("GDPP_AWAIT_DEFAULT_AOT_RUNTIME_OK")
    quit()
