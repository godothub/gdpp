@static_unload
extends RefCounted
class_name StaticUnloadCase

static var value := 40


static func next() -> int:
    value += 1
    return value
