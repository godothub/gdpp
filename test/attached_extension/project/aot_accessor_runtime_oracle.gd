extends SceneTree

const COROUTINE_ACCESSOR_PROBE := preload("res://coroutine_accessor_probe.gd")


func _initialize() -> void:
    call_deferred(&"_run")


func _run() -> void:
    var probe: Variant = COROUTINE_ACCESSOR_PROBE.new()
    var failure: String = await probe.verify(self)
    if not failure.is_empty():
        push_error(failure)
        quit(1)
        return
    print("GDPP_COROUTINE_ACCESSOR_AOT_RUNTIME_OK")
    quit()
