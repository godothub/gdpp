extends DynamicAttachBase

signal installed(value: int)

var derived_value := 2


func evaluate_attached() -> int:
    return evaluate_base(derived_value)


func _ready() -> void:
    ready_seen = true
    installed.emit(evaluate_attached())
