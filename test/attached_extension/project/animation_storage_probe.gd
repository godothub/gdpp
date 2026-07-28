extends AnimationPlayer

@export_category("Storage Probe")
@export var verify_library := true


func has_serialized_library() -> bool:
    return verify_library and has_animation(&"storage_probe")
