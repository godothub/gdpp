extends "res://stale_await_base.gd"

@onready var marker: Node = $Marker


func behavior_identity() -> String:
    return "final"


func lifecycle_is_current() -> bool:
    var resumed_behaviors: Array = get_meta(&"resumed_behaviors", [])
    return marker == get_node("Marker") and not resumed_behaviors.has("intermediate")
