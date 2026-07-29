extends Node

@onready var marker: Node = get_node("Marker")

@export var value := 0:
    set(next):
        value = next
        if not is_inside_tree():
            await ready
        marker.set_meta(&"resumed_value", value)


func resumed_value() -> int:
    return int(marker.get_meta(&"resumed_value", -1))
