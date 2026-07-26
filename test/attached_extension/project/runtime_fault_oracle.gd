extends SceneTree

const RUNTIME_FAULT_MATRIX := preload("res://runtime_fault_matrix.gd")
const RUNTIME_VALUE_MATRIX := preload("res://runtime_value_matrix.gd")


func _initialize() -> void:
    var matrix: Variant = RUNTIME_FAULT_MATRIX.new()
    print("GDPP_FAULT_MATRIX=" + matrix.run())
    var value_matrix: Variant = RUNTIME_VALUE_MATRIX.new()
    print("GDPP_VALUE_MATRIX=" + value_matrix.run())
    quit()
