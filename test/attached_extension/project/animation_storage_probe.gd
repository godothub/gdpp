extends AnimationPlayer

@export_category("Storage Probe")
@export var verify_library := true
@export var linked_node: Node
@export var typed_records: Array[Dictionary] = []
var initialized_script_path: String = get_script().resource_path


func has_serialized_library() -> bool:
    return verify_library and has_animation(&"storage_probe")


func has_serialized_node_reference() -> bool:
    return linked_node == get_parent()


func has_serialized_typed_container() -> bool:
    return (
        typed_records.is_typed()
        and typed_records.get_typed_builtin() == TYPE_DICTIONARY
        and typed_records.size() == 1
        and typed_records[0].get("score", 0) == 42
    )


func has_script_resource_identity() -> bool:
    return (
        initialized_script_path == "res://animation_storage_probe.gd"
        and get_script().resource_path == initialized_script_path
    )
