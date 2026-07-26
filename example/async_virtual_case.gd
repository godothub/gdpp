extends Control
class_name AsyncVirtualCase

var _process_started := false
var virtual_trace: Array[String] = []


func _process(delta: float) -> void:
    if _process_started:
        return
    _process_started = true
    set_process(false)
    await get_tree().process_frame
    if delta >= 0.0:
        print("GDPP_ASYNC_VIRTUAL_OK")


func _get_drag_data(at_position: Vector2) -> Variant:
    virtual_trace.push_back("drag:start")
    await get_tree().process_frame
    virtual_trace.push_back("drag:resume")
    return at_position


func _get_tooltip(at_position: Vector2) -> String:
    virtual_trace.push_back("tooltip:start")
    await get_tree().process_frame
    virtual_trace.push_back("tooltip:resume")
    return str(at_position)
