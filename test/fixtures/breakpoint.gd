extends Node

signal resumed

var state := 3

func inspect(value: int) -> void:
    var label := "outer"
    if value > 0:
        var label := value
        breakpoint

static func inspect_static(value: int) -> void:
    breakpoint

func inspect_after_await(value: int) -> void:
    await resumed
    breakpoint

func make_callback() -> Callable:
    return func named_callback(value: int) -> void:
        breakpoint
