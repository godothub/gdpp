extends "res://onready_inheritance_base.gd"

@onready var derived_marker: Node = $DerivedMarker
@onready var base_initialized_first := base_initialized()

func initialized() -> bool:
    return (
        base_initialized_first
        and base_initialized()
        and base_ready_observed()
        and derived_marker == get_node("DerivedMarker")
    )
