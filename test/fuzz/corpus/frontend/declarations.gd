@tool
@icon("res://icon.svg")
extends Node2D
class_name FuzzActor

signal changed(value: int, metadata: Dictionary[String, Variant])

enum Mode { IDLE, ACTIVE = 4, DONE }

@export_range(0.0, 10.0, 0.25)
var speed: float = 2.5:
    set(value):
        speed = clampf(value, 0.0, 10.0)
    get:
        return speed

func update(delta: float, ...events: Array) -> int:
    var total := 0
    for event: Variant in events:
        if event is int:
            total += event
    return total if delta > 0.0 else -total

class Payload extends RefCounted:
    var values: Array[int] = [1, 2, 3]
