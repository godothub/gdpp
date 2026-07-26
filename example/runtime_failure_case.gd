extends Node
class_name RuntimeFailureCase

var markers: Array[String] = []


func _side_effect(marker: String) -> int:
    markers.push_back(marker)
    return 1


func _direct_bounds_failure() -> void:
    markers.push_back("direct-before")
    var values := [1]
    var result := _side_effect("left") + values[9] + _side_effect("right")
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
    return markers == ["division-before", "binary-before", "typed-array-after"]
