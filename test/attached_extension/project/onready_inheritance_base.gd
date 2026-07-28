extends Node

@onready var base_marker: Node = $BaseMarker

var ready_called := false
var construction_script_path: String = get_script().resource_path


func _ready() -> void:
    ready_called = base_marker == get_node("BaseMarker")


func base_initialized() -> bool:
    return base_marker == get_node("BaseMarker")


func base_ready_observed() -> bool:
    return ready_called


func base_observed_most_derived_script() -> bool:
    return construction_script_path == "res://onready_inheritance_probe.gd"
