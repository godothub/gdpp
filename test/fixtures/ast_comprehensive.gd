@tool
@icon("res://icon.svg")
class_name AstGolden extends Node

@export var score: int = 1:
    get:
        return score
    set(value):
        score = value
var label: get = read_label, set = write_label
static var shared := 2
const LIMIT := 3
signal changed(value: int)
enum State { IDLE, RUN = 2 }
enum { DEFAULT_STATE = State.IDLE }

class Inner extends RefCounted:
    @export var enabled := true
    enum Mode { FIRST }
    signal completed()
    func ping() -> void:
        pass
    class Nested:
        var item = null

@abstract
class AbstractInner:
    @abstract
    func execute(value: int) -> void

@rpc("authority", "call_local", "reliable", 2)
func exercise(value: Variant, optional := 1, ...rest: Array) -> Variant:
    @warning_ignore("unused_variable")
    var inferred := 1
    const local: int = 2
    var nil_value = null
    var text = "text"
    var name = &"name"
    var node_path = ^"Root/Child"
    var node = $Root/Child
    var unique = %Unique
    var values = [true, 1, 2.5, text]
    var mapping = {"key": values[0]}
    var callback := func named(input: int = 1, ...parts: Array) -> int:
        return await helper(input)
    inferred += local
    mapping.key = -inferred
    assert(not false, "message")
    helper(inferred)
    if value == null:
        pass
    elif value is String:
        breakpoint
    else:
        inferred = -inferred
    while inferred < 5:
        inferred += 1
        continue
        break
    for item: int in [1, 2]:
        inferred += item
    match [1, value, 3]:
        [1, {"hp": var hp, ..}, [var first, ..]] when hp > 0:
            return first
        0, 1:
            return "small"
        _:
            return callback.call(optional) if value else mapping["key"]
    return nil_value

static func helper(input: int) -> int:
    return input

@onready var child := $Child
