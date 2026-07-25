class_name VendorChild
extends VendorBase

const INHERITED_PHYSICS_SCENE := preload("res://deferred_shape.tscn")

signal child_ping(value: int)

@export var bonus: int = 7
var initialized: int = 0
var ready_seen: bool = false
var enter_tree_count := 0
var ready_notification_count := 0
var exit_tree_count := 0


func _init() -> void:
    initialized += 1


func _enter_tree() -> void:
    enter_tree_count += 1


func _ready() -> void:
    ready_seen = true
    child_ping.emit(bonus)
    super.emit_vendor_ping(bonus)


func _exit_tree() -> void:
    exit_tree_count += 1


func _notification(what: int) -> void:
    if what == NOTIFICATION_READY:
        ready_notification_count += 1


func compute(value: int) -> int:
    return super.native_compute(value) + bonus


func combine(base: int, optional: int = 2, ...values: Array) -> int:
    return base + optional + values.size()


func hook(value: int) -> int:
    return value * 3 + bonus


func verify_vendor_contract(
    payload: Variant,
    values: Array[int],
    weights: Dictionary[String, int]
) -> Variant:
    native_payload = payload
    var echoed: Variant = super.variant_roundtrip(native_payload)
    var returned_values: Array[int] = super.typed_array_roundtrip(values)
    var returned_weights: Dictionary[String, int] = super.typed_dictionary_roundtrip(weights)
    super.emit_vendor_contract(returned_values, returned_weights)
    return echoed


@rpc("any_peer", "call_local", "reliable", 2)
func inherited_rpc(value: int) -> int:
    return value + bonus


@rpc("authority", "call_remote", "unreliable", 1)
func overridden_rpc(value: int) -> int:
    return value
