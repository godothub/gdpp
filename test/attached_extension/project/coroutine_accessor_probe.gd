class_name CoroutineAccessorProbe
extends RefCounted

signal resumed(value)

static var static_observed := -1
static var delayed_static: int:
    get:
        await (Engine.get_main_loop() as SceneTree).process_frame
        return 44
    set(next):
        await (Engine.get_main_loop() as SceneTree).process_frame
        static_observed = next

var setter_value := -1
var setter_values: Array[int] = []
var _captured: Array[int] = []

var inline_value: int:
    get:
        return await resumed
    set(next):
        await resumed
        setter_value = next
        setter_values.push_back(next)

var bound_value: int:
    get = _read_bound


func _read_bound() -> int:
    return await resumed


class InnerProbe:
    signal resumed(value)

    var value: int:
        get:
            return await resumed


func _capture_inline() -> void:
    _captured.push_back(await get("inline_value"))


func _capture_bound() -> void:
    _captured.push_back(await get("bound_value"))


func verify(tree: SceneTree) -> String:
    static_observed = -1
    CoroutineAccessorProbe.delayed_static = 43
    var static_value: int = await CoroutineAccessorProbe.delayed_static
    await tree.process_frame
    if static_value != 44 or static_observed != 43:
        return "static coroutine accessor lost owner-free suspension state"

    var inner_probe := InnerProbe.new()
    var inner_values: Array[int] = []
    var capture_inner := func() -> void:
        inner_values.push_back(await inner_probe.get("value"))
    capture_inner.call()
    inner_probe.resumed.emit(45)
    await tree.process_frame
    if inner_values != [45]:
        return "internal class coroutine getter lost its resumed value"

    _captured.clear()
    _capture_inline()
    _capture_inline()
    resumed.emit(41)
    await tree.process_frame
    if _captured != [41, 41]:
        return "concurrent inline getter calls shared or lost coroutine state"

    _captured.clear()
    _capture_bound()
    resumed.emit(42)
    await tree.process_frame
    if _captured != [42]:
        return "method-bound coroutine getter lost its resumed value"

    setter_value = -1
    setter_values.clear()
    set("inline_value", 73)
    set("inline_value", 74)
    if setter_value != -1:
        return "coroutine setter completed before its suspension signal"
    resumed.emit(0)
    await tree.process_frame
    if setter_value != 74 or setter_values != [73, 74]:
        return "concurrent coroutine setters shared or lost their assigned values"
    return ""
