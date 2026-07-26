extends Node
class_name RuntimeFailureCase

signal resume_lambda_loop
signal lifecycle(value: int)

var markers: Array[String] = []
var async_lambda_values: Array[int] = []
var lifecycle_markers: Array[String] = []


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


class LifecycleTarget extends Node:
    var output: Array[String]

    func _init(target_output: Array[String]) -> void:
        output = target_output

    func record(value: int, label: String) -> void:
        output.push_back("%s:%d" % [label, value])


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


func _lambda_capture_semantics_match() -> bool:
    var scalar := 1
    var mutate_scalar := func() -> int:
        scalar += 1
        return scalar
    scalar = 10
    if mutate_scalar.call() != 2 or mutate_scalar.call() != 2 or scalar != 10:
        return false

    var factorial: Callable
    factorial = func(number: int) -> int:
        return 1 if number <= 1 else number * factorial.call(number - 1)
    if factorial.call(5) != 0:
        return false

    var nested_scalar := 1
    var factory := func() -> Callable:
        nested_scalar += 1
        return func() -> int:
            nested_scalar += 10
            return nested_scalar
    var first: Callable = factory.call()
    var second: Callable = factory.call()
    if (
        first.call() != 12
        or first.call() != 12
        or second.call() != 12
        or nested_scalar != 1
    ):
        return false

    var shared := [1]
    var mutate_shared := func() -> int:
        shared.push_back(shared.size() + 1)
        return shared.size()
    return mutate_shared.call() == 2 and mutate_shared.call() == 3 and shared == [1, 2, 3]


func _record_async_lambda_capture() -> void:
    var scalar := 1
    var iteration := 0
    while iteration < 1:
        await resume_lambda_loop
        var mutate_scalar := func() -> int:
            scalar += 1
            return scalar
        scalar = 10
        async_lambda_values.assign(
            [mutate_scalar.call(), scalar, mutate_scalar.call(), scalar]
        )
        iteration += 1


func _async_lambda_capture_semantics_match() -> bool:
    async_lambda_values.clear()
    _record_async_lambda_capture()
    resume_lambda_loop.emit()
    return async_lambda_values == [2, 10, 2, 10]


static func _make_owner_free_callback(output: Array[String], label: String) -> Callable:
    return func(value: int) -> void:
        output.push_back("%s:%d" % [label, value])


func _lifecycle_first(value: int) -> void:
    lifecycle_markers.push_back("first:%d" % value)
    if lifecycle.is_connected(_lifecycle_second):
        lifecycle.disconnect(_lifecycle_second)
    if not lifecycle.is_connected(_lifecycle_late):
        lifecycle.connect(_lifecycle_late)


func _lifecycle_second(value: int) -> void:
    lifecycle_markers.push_back("second:%d" % value)


func _lifecycle_late(value: int) -> void:
    lifecycle_markers.push_back("late:%d" % value)


func _lifecycle_bound(first: int, second: String, suffix: String) -> String:
    return "%d:%s:%s" % [first, second, suffix]


func _signal_callable_lifecycle_matches() -> bool:
    lifecycle_markers.clear()
    var one_shot := _make_owner_free_callback(lifecycle_markers, "once")
    var retained := _make_owner_free_callback(lifecycle_markers, "retained")
    if one_shot != one_shot or one_shot == _make_owner_free_callback(lifecycle_markers, "once"):
        return false
    if lifecycle.connect(one_shot, CONNECT_ONE_SHOT) != OK:
        return false
    if lifecycle.connect(retained) != OK:
        return false
    lifecycle.emit(1)
    lifecycle.emit(2)
    if lifecycle_markers != ["once:1", "retained:1", "retained:2"]:
        return false
    lifecycle.disconnect(retained)

    lifecycle_markers.clear()
    lifecycle.connect(_lifecycle_first)
    lifecycle.connect(_lifecycle_second)
    lifecycle.emit(3)
    lifecycle.emit(4)
    if lifecycle_markers != ["first:3", "second:3", "first:4", "late:4"]:
        return false
    lifecycle.disconnect(_lifecycle_first)
    lifecycle.disconnect(_lifecycle_late)

    lifecycle_markers.clear()
    var reference_counted := _make_owner_free_callback(lifecycle_markers, "reference")
    if lifecycle.connect(reference_counted, CONNECT_REFERENCE_COUNTED) != OK:
        return false
    if lifecycle.connect(reference_counted, CONNECT_REFERENCE_COUNTED) != OK:
        return false
    lifecycle.emit(5)
    lifecycle.disconnect(reference_counted)
    if not lifecycle.is_connected(reference_counted):
        return false
    lifecycle.emit(6)
    lifecycle.disconnect(reference_counted)
    if lifecycle.is_connected(reference_counted):
        return false
    if lifecycle_markers != ["reference:5", "reference:6"]:
        return false

    lifecycle_markers.clear()
    var target := LifecycleTarget.new(lifecycle_markers)
    var target_callback := Callable(target, &"record").bind("target")
    lifecycle.connect(target_callback)
    target.free()
    if target_callback.is_valid() or lifecycle.is_connected(target_callback):
        return false
    lifecycle.emit(7)

    var deferred := _make_owner_free_callback(lifecycle_markers, "deferred")
    if lifecycle.connect(deferred, CONNECT_DEFERRED | CONNECT_ONE_SHOT) != OK:
        return false
    lifecycle.emit(8)
    if not lifecycle_markers.is_empty():
        return false

    var bound := Callable(self, &"_lifecycle_bound").bind("tail")
    var rebound := bound.unbind(1)
    if (
        bound.get_argument_count() != 2
        or bound.call(9, "head") != "9:head:tail"
        or rebound.get_argument_count() != 3
        or rebound.call(9, "ignored", "head") != "9:ignored:tail"
    ):
        return false

    var mutex := Mutex.new()
    var threaded_values: Array[int] = []
    var worker := func(base: int) -> void:
        for offset in 64:
            mutex.lock()
            threaded_values.push_back(base + offset)
            mutex.unlock()
    var first_thread := Thread.new()
    var second_thread := Thread.new()
    first_thread.start(worker.bind(0))
    second_thread.start(worker.bind(64))
    first_thread.wait_to_finish()
    second_thread.wait_to_finish()
    threaded_values.sort()
    return (
        threaded_values.size() == 128
        and threaded_values.front() == 0
        and threaded_values.back() == 127
    )


func deferred_signal_lifecycle_matches() -> bool:
    return lifecycle_markers == ["deferred:8"]


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
        and _lambda_capture_semantics_match()
        and _async_lambda_capture_semantics_match()
        and _signal_callable_lifecycle_matches()
    )
