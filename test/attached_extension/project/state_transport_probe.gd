class_name StateTransportProbe
extends RefCounted

var initializer_marker: int = 73
var constructor_marker: int
var value: int = 5:
    get:
        return value + 100
    set(incoming):
        value = incoming + 1


func _init() -> void:
    constructor_marker = 91
