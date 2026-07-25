extends Node

@onready var marker: Node = $Marker


func initialized() -> bool:
    return marker == get_node("Marker")
