extends AnimationPlayer

@export_category("Storage Probe")
@export var verify_library := true
@export var linked_node: Node


func has_serialized_library() -> bool:
    return verify_library and has_animation(&"storage_probe")


func has_serialized_node_reference() -> bool:
    return linked_node == get_parent()
