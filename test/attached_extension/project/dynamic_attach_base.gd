class_name DynamicAttachBase
extends Node

var base_value := 40
var ready_seen := false


func evaluate_base(delta: int) -> int:
    return base_value + delta
