extends SceneTree

const RUNTIME_FAULT_MATRIX := preload("res://runtime_fault_matrix.gd")


func _initialize() -> void:
    var matrix: Variant = RUNTIME_FAULT_MATRIX.new()
    print("GDPP_FAULT_MATRIX=" + matrix.run())
    quit()
