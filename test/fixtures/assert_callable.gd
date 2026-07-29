extends RefCounted


func make_validator() -> Callable:
    return func(value: Variant) -> void:
        assert(value != null, "callable argument must not be null")
