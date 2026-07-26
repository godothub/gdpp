extends RefCounted

signal selected(value)

var _order: Array[String] = []
var _results: Array[Array] = []
var _lambda_value := -1
var _awaited_result := -1


func _mark(label: String, value: int) -> int:
    _order.append(label)
    return value


func _ordered_defaults(
    first: int = _mark("first", 1),
    second: int = await selected,
    third: int = _mark("third", first + second),
) -> void:
    _order.append("body")
    _results.append([first, second, third])


func _default_only(value: int = await selected) -> int:
    return value


func _await_default_only() -> void:
    _awaited_result = await _default_only()


func verify(tree: SceneTree) -> String:
    _order.clear()
    _results.clear()
    _ordered_defaults()
    if _order != ["first"] or not _results.is_empty():
        return "awaited defaults did not suspend after evaluating preceding defaults"
    selected.emit(2)
    await tree.process_frame
    if _order != ["first", "third", "body"] or _results != [[1, 2, 3]]:
        return "awaited defaults lost left-to-right evaluation or a preceding parameter"

    _order.clear()
    _results.clear()
    _ordered_defaults(10, 20)
    if _order != ["third", "body"] or _results != [[10, 20, 30]]:
        return "supplied arguments did not skip their default expressions"

    _lambda_value = -1
    var callback := func(value: int = await selected) -> void:
        _lambda_value = value
    callback.call()
    if _lambda_value != -1:
        return "awaited lambda default completed before its signal"
    selected.emit(31)
    await tree.process_frame
    if _lambda_value != 31:
        return "awaited lambda default lost its resumed value"

    _order.clear()
    _results.clear()
    _ordered_defaults(10)
    _ordered_defaults(20)
    selected.emit(2)
    await tree.process_frame
    if _results != [[10, 2, 12], [20, 2, 22]]:
        return "concurrent awaited defaults shared or reordered invocation state"

    _awaited_result = -1
    _await_default_only()
    if _awaited_result != -1:
        return "await did not suspend a function whose only suspension is a default argument"
    selected.emit(73)
    await tree.process_frame
    if _awaited_result != 73:
        return "await did not resume a function classified by its default argument"
    return ""
