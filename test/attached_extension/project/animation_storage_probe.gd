extends AnimationPlayer


func has_serialized_library() -> bool:
    return has_animation(&"storage_probe")
