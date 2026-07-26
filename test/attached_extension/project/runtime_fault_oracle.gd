extends SceneTree

const RUNTIME_FAULT_MATRIX := preload("res://runtime_fault_matrix.gd")
const RUNTIME_VALUE_MATRIX := preload("res://runtime_value_matrix.gd")
const AWAIT_DEFAULT_PROBE := preload("res://await_default_probe.gd")
const COROUTINE_ACCESSOR_PROBE := preload("res://coroutine_accessor_probe.gd")


func _initialize() -> void:
    call_deferred(&"_run")


func _run() -> void:
    var matrix: Variant = RUNTIME_FAULT_MATRIX.new()
    print("GDPP_FAULT_MATRIX=" + matrix.run())
    var value_matrix: Variant = RUNTIME_VALUE_MATRIX.new()
    print("GDPP_VALUE_MATRIX=" + value_matrix.run())
    var await_default_probe: Variant = AWAIT_DEFAULT_PROBE.new()
    var await_default_failure: String = await await_default_probe.verify(self)
    if not await_default_failure.is_empty():
        push_error(await_default_failure)
        quit(1)
        return
    print("GDPP_AWAIT_DEFAULT_MATRIX=ok")
    var coroutine_accessor_probe: Variant = COROUTINE_ACCESSOR_PROBE.new()
    var coroutine_accessor_failure: String = await coroutine_accessor_probe.verify(self)
    if not coroutine_accessor_failure.is_empty():
        push_error(coroutine_accessor_failure)
        quit(1)
        return
    print("GDPP_COROUTINE_ACCESSOR_MATRIX=ok")
    quit()
