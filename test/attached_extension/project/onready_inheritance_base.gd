extends Node

@onready var base_marker: Node = $BaseMarker

var ready_called := false


func _ready() -> void:
    ready_called = base_marker == get_node("BaseMarker")


func base_initialized() -> bool:
    return base_marker == get_node("BaseMarker")


func base_ready_observed() -> bool:
    return ready_called
