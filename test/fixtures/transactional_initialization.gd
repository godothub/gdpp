extends RefCounted

const ROOT_SCENE = preload("res://root.tscn")
static var root_value: int = [1][4]
var root_scene = preload("res://root.tscn")


static func _static_init() -> void:
    root_value += 1


class Worker:
    const WORKER_SCENE = preload("res://worker.tscn")
    static var worker_value: int = [1][4]
    var worker_scene = preload("res://worker.tscn")

    static func _static_init() -> void:
        worker_value += 1
