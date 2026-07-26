extends Node
class_name RuntimeFailureCase

var markers: Array[String] = []


class BrokenFieldInitialization extends RefCounted:
    var markers: Array[String] = []
    var first := record("field-first")
    var values := [1]
    var failed: Variant = values[9]
    var after := record("field-after")

    func record(value: String) -> int:
        markers.push_back(value)
        return markers.size()

    func _init() -> void:
        markers.push_back("init-body")


class BrokenConversionInitialization extends RefCounted:
    var markers: Array[String] = []
    var source: Variant = "invalid"
    var typed: int = source
    var after := record("conversion-after")

    func record(value: String) -> int:
        markers.push_back(value)
        return markers.size()

    func _init() -> void:
        markers.push_back("conversion-init")


class BrokenOnreadyInitialization extends Node:
    var markers: Array[String] = []
    @onready var first := record("onready-first")
    @onready var values := [1]
    @onready var failed: Variant = values[9]
    @onready var after := record("onready-after")

    func record(value: String) -> int:
        markers.push_back(value)
        return markers.size()

    func _ready() -> void:
        markers.push_back("ready-body")


class OnreadyOnly extends Node:
    var markers: Array[String] = []
    @onready var initialized := record("onready-only")

    func record(value: String) -> int:
        markers.push_back(value)
        return markers.size()


func _side_effect(marker: String) -> int:
    markers.push_back(marker)
    return 1


func _direct_bounds_failure() -> void:
    markers.push_back("direct-before")
    var values := [1]
    var result: Variant = _side_effect("left") + values[9] + _side_effect("right")
    markers.push_back("direct-after:%d" % result)


func _callee_bounds_failure() -> int:
    markers.push_back("callee-before")
    var values := [1]
    return values[9]


func _caller_survives_callee_failure() -> void:
    var result := _side_effect("caller-left") + _callee_bounds_failure() + _side_effect("caller-right")
    markers.push_back("caller-after:%d" % result)


func _target(values: Array[int]) -> Array[int]:
    markers.push_back("target")
    return values


func _index() -> int:
    markers.push_back("index")
    return 0


func _assignment_order() -> bool:
    var values: Array[int] = [0]
    _target(values)[_index()] = _side_effect("rhs")
    return values == [1]


func _division_failure() -> void:
    markers.push_back("division-before")
    var divisor := 0
    var result := 7 / divisor
    markers.push_back("division-after:%d" % result)


func _invalid_dynamic_binary() -> void:
    markers.push_back("binary-before")
    var left: Variant = Vector2.ONE
    var right: Variant = "invalid"
    var result: Variant = left + right
    markers.push_back("binary-after:%s" % result)


func _typed_array_engine_error_continues() -> bool:
    var values: Array[int] = [1]
    var invalid: Variant = "invalid"
    values.push_back(invalid)
    markers.push_back("typed-array-after")
    return values == [1]


func _callable_target(value: int) -> int:
    markers.push_back("callable-target:%d" % value)
    return value + 1


func _null_callable_failure() -> void:
    markers.push_back("null-call-before")
    var callback := Callable()
    var result: Variant = callback.call(_side_effect("null-call-argument"))
    markers.push_back("null-call-after:%s" % result)


func _callable_arity_failure() -> void:
    markers.push_back("arity-call-before")
    var callback: Callable = Callable(self, &"_callable_target")
    var result: Variant = callback.call()
    markers.push_back("arity-call-after:%s" % result)


func _freed_callable_failure() -> void:
    var target := Node.new()
    var callback := Callable(target, &"get_name")
    target.free()
    markers.push_back("freed-call-before")
    var result: Variant = callback.call(_side_effect("freed-call-argument"))
    markers.push_back("freed-call-after:%s" % result)


func _unbound_callable_failure() -> void:
    markers.push_back("unbound-call-before")
    var callback: Callable = Callable(self, &"_callable_target").unbind(2)
    var result: Variant = callback.call()
    markers.push_back("unbound-call-after:%s" % result)


func _null_signal_emit_continues() -> void:
    markers.push_back("null-emit-before")
    var empty := Signal()
    empty.emit(_side_effect("null-emit-argument"))
    markers.push_back("null-emit-after")


func _instance_initialization_failure_continues() -> bool:
    var field_instance := BrokenFieldInitialization.new()
    var conversion_instance := BrokenConversionInitialization.new()
    return (
        field_instance != null
        and field_instance.markers == ["field-first", "init-body"]
        and conversion_instance != null
        and conversion_instance.markers == ["conversion-init"]
    )


func _onready_initialization_failure_continues() -> bool:
    var broken := BrokenOnreadyInitialization.new()
    add_child(broken)
    if broken.markers != ["onready-first", "ready-body"]:
        broken.free()
        return false
    remove_child(broken)
    broken.request_ready()
    add_child(broken)
    if broken.markers != ["onready-first", "ready-body", "onready-first", "ready-body"]:
        broken.free()
        return false
    broken.free()
    var onready_only := OnreadyOnly.new()
    add_child(onready_only)
    var valid := onready_only.markers == ["onready-only"]
    onready_only.free()
    return valid


func run_contract() -> bool:
    markers.clear()
    _direct_bounds_failure()
    if markers != ["direct-before", "left"]:
        return false
    _caller_survives_callee_failure()
    if markers.slice(2) != ["caller-left", "callee-before", "caller-right", "caller-after:2"]:
        return false
    markers.clear()
    if not _assignment_order() or markers != ["target", "rhs", "index"]:
        return false
    markers.clear()
    _division_failure()
    if markers != ["division-before"]:
        return false
    _invalid_dynamic_binary()
    if markers != ["division-before", "binary-before"]:
        return false
    if not _typed_array_engine_error_continues():
        return false
    if markers != ["division-before", "binary-before", "typed-array-after"]:
        return false
    markers.clear()
    _null_callable_failure()
    markers.push_back("caller-after-null-call")
    if markers != ["null-call-before", "null-call-argument", "caller-after-null-call"]:
        return false
    markers.clear()
    _callable_arity_failure()
    markers.push_back("caller-after-arity-call")
    if markers != ["arity-call-before", "caller-after-arity-call"]:
        return false
    markers.clear()
    _freed_callable_failure()
    markers.push_back("caller-after-freed-call")
    if markers != [
        "freed-call-before",
        "freed-call-argument",
        "caller-after-freed-call",
    ]:
        return false
    markers.clear()
    _unbound_callable_failure()
    markers.push_back("caller-after-unbound-call")
    if markers != ["unbound-call-before", "caller-after-unbound-call"]:
        return false
    markers.clear()
    _null_signal_emit_continues()
    if markers != ["null-emit-before", "null-emit-argument", "null-emit-after"]:
        return false
    return (
        _instance_initialization_failure_continues()
        and _onready_initialization_failure_continues()
    )
