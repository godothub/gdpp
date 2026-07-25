@icon("res://icon.svg")
extends Node2D
class_name HelloAot

enum MovementMode { IDLE, WALK = 4, RUN = WALK * 2 }
enum { DEFAULT_LIVES = 3, MAX_LIVES = DEFAULT_LIVES + 2 }

const LITERAL_HEXADECIMAL = 0x7fff_ffff
const LITERAL_BINARY = 0b1010_0101
const LITERAL_DECIMAL = 12_345_678
const LITERAL_MINIMUM = -9_223_372_036_854_775_808
const LITERAL_FLOAT = 1_2.5_0e+1_0
const LITERAL_MAXIMUM = 1.7976931348623157e308
const LITERAL_ROUNDED_OVERFLOW = 1.7976931348623158e308
const LITERAL_OVERFLOW = 1e400
const LITERAL_UNDERFLOW = 1e-400
const LITERAL_SUBNORMAL_ZERO = 1e-309
const LITERAL_NOT_A_NUMBER = 0e400
const LITERAL_ESCAPED = "\a\b\f\v\u0041\U01F600\uD83D\uDE00"
const LITERAL_NUL = "A�B"
const LITERAL_CONTINUED = "left\
right"
const LITERAL_RAW = r"\n\"quoted\"\\path"
const LITERAL_TRIPLE = """first
"quoted"
last"""

signal greeted(name: String)
signal typed_containers_changed(
    values: Array[int],
    weights: Dictionary[String, int]
)
@export var greeting: String = "Hello"
@export_range(0.0, 100.0, 0.5, "or_greater") var movement_speed: float = 10.0
var aliases := []
var metadata := {}
var origin := Vector2(0.0, 0.0)
var dynamic_accent: Variant = Color(0.4, 0.5, 0.6, 1.0)
@export_color_no_alpha var accent: Color = Color.html("ff8800")
@export var icon: Texture2D
@export var movement_mode: MovementMode = MovementMode.WALK
@export var accessor_score: int = 5:
    set(value):
        if value < 0:
            accessor_score = 0
        elif value > 100:
            accessor_score = 100
        else:
            accessor_score = value
    get:
        return accessor_score


var internal_counter: int = 3:
    set(value):
        internal_counter = value
    get:
        return internal_counter

var assertion_evaluations: int = 0
var matrix_frame_output: String = ""
var matrix_frame_samples: int = 0
var matrix_frame_warmup: int = 0
var matrix_frame_iterations: int = 0
var matrix_frame_index: int = 0
var matrix_frame_checksum: int = 0
var matrix_frame_intervals_us: Array[float] = []
var matrix_frame_workload_us: Array[int] = []
var matrix_frame_workload: Node
@export var typed_integers: Array[int] = [1, 2]
var typed_weights: Dictionary[String, int] = {"left": 1}
var typed_nodes: Array[Node] = []

func greet(name: String) -> void:
    var names := [name, "Godot"]
    var labels := {"prefix": greeting}
    for item in names:
        print(labels["prefix"] + ", " + item)
    if name == "":
        print("anonymous")
    elif name == "Godot":
        print("engine")
    else:
        print("player")
    emit_signal("greeted", name)


func _on_greeted(name: String) -> void:
    print("greeted " + name)


func _on_ready() -> void:
    var matrix_args := OS.get_cmdline_user_args()
    if matrix_args.has("--gdpp-coroutine-loop"):
        _run_coroutine_loop_contract()
        return
    if matrix_args.has("--gdpp-matrix-startup"):
        print("GDPP_MATRIX_STARTUP_OK")
        get_tree().quit(0)
        return
    var frame_output_index := matrix_args.find("--gdpp-matrix-frame-output")
    if frame_output_index >= 0 and frame_output_index + 1 < matrix_args.size():
        matrix_frame_output = matrix_args[frame_output_index + 1]
        var frame_count_index := matrix_args.find("--gdpp-matrix-frames")
        var frame_warmup_index := matrix_args.find("--gdpp-matrix-frame-warmup")
        var frame_iterations_index := matrix_args.find("--gdpp-matrix-frame-iterations")
        matrix_frame_samples = int(matrix_args[frame_count_index + 1])
        matrix_frame_warmup = int(matrix_args[frame_warmup_index + 1])
        matrix_frame_iterations = int(matrix_args[frame_iterations_index + 1])
        matrix_frame_workload = get_node(NodePath("MatrixWorkload"))
        return
    var matrix_output_index := matrix_args.find("--gdpp-matrix-output")
    if matrix_output_index >= 0 and matrix_output_index + 1 < matrix_args.size():
        var samples := 9
        var iterations := 20000
        var samples_index := matrix_args.find("--gdpp-matrix-samples")
        if samples_index >= 0 and samples_index + 1 < matrix_args.size():
            samples = int(matrix_args[samples_index + 1])
        var iterations_index := matrix_args.find("--gdpp-matrix-iterations")
        if iterations_index >= 0 and iterations_index + 1 < matrix_args.size():
            iterations = int(matrix_args[iterations_index + 1])
        var workload: Node = get_node(NodePath("MatrixWorkload"))
        var report: Dictionary = workload.call("run_matrix", samples, iterations)
        var output := FileAccess.open(matrix_args[matrix_output_index + 1], FileAccess.WRITE)
        if not output:
            push_error("GDPP_MATRIX cannot open output")
            get_tree().quit(2)
            return
        output.store_string(JSON.stringify(report))
        output.close()
        print("GDPP_MATRIX_OK")
        get_tree().quit(0)
        return
    if (
        greeting == "Exported hello"
        and movement_speed == 24.5
        and movement_mode == MovementMode.RUN
        and classify_mode(movement_mode) == "run"
        and classify_dynamic("ready") == "ok"
        and accessor_score == 100
        and validate_accessor() == true
        and validate_inheritance() == true
        and validate_latest_literals() == true
        and validate_typed_containers() == true
        and assertion_evaluation_count() == (1 if OS.is_debug_build() else 0)
    ):
        print("GDPP_EXPORTED_PROPERTIES_OK")
        _publish_web_smoke_status("ok")
    else:
        print("GDPP_EXPORTED_PROPERTIES_INVALID")
        _publish_web_smoke_status("invalid")


func _run_coroutine_loop_contract() -> void:
    # Mirrors production object-pool warmup: a loop-carried counter is mutated by a nested batch
    # loop and must be observed by the outer condition after every frame suspension.
    var target: int = 4996
    var added: int = 0
    while added < target:
        var batch: int = min(200, target - added)
        for _index in range(batch):
            added += 1
        await get_tree().process_frame
    if added != target:
        push_error("GDPP_COROUTINE_LOOP_INVALID")
        get_tree().quit(2)
        return
    print("GDPP_COROUTINE_LOOP_OK")
    get_tree().quit(0)


func _publish_web_smoke_status(status: String) -> void:
    if not OS.has_feature("web"):
        return
    # CI reads this deterministic DOM oracle through a persistent browser session. Browser
    # console forwarding differs between Chrome versions and must not decide whether a
    # commercial export is accepted.
    JavaScriptBridge.eval(
        "document.documentElement.dataset.gdppStatus = '%s';" % status
    )


func validate_latest_literals() -> bool:
    return (
        LITERAL_HEXADECIMAL == 2147483647
        and LITERAL_BINARY == 165
        and LITERAL_DECIMAL == 12345678
        and LITERAL_MINIMUM == -9223372036854775808
        and LITERAL_FLOAT == 125000000000.0
        and LITERAL_MAXIMUM > 1e308
        and not is_inf(LITERAL_MAXIMUM)
        and is_inf(LITERAL_ROUNDED_OVERFLOW)
        and is_inf(LITERAL_OVERFLOW)
        and LITERAL_UNDERFLOW == 0.0
        and LITERAL_SUBNORMAL_ZERO == 0.0
        and is_nan(LITERAL_NOT_A_NUMBER)
        and LITERAL_ESCAPED.length() == 7
        and LITERAL_ESCAPED.unicode_at(0) == 7
        and LITERAL_ESCAPED.unicode_at(4) == 65
        and LITERAL_ESCAPED.unicode_at(5) == 0x1f600
        and LITERAL_ESCAPED.unicode_at(6) == 0x1f600
        and LITERAL_NUL.length() == 3
        and LITERAL_NUL.unicode_at(0) == 65
        and LITERAL_NUL.unicode_at(1) == 0xfffd
        and LITERAL_NUL.unicode_at(2) == 66
        and LITERAL_CONTINUED == "leftright"
        and LITERAL_RAW == "\\n\\\"quoted\\\"\\\\path"
        and LITERAL_TRIPLE == "first\n\"quoted\"\nlast"
        and 计算标识符(6) == 21
    )


func validate_typed_containers() -> bool:
    var local_values: Array[int] = [3, 4]
    var local_weights: Dictionary[String, int] = {"right": 2}
    return (
        typed_integers.is_typed()
        and typed_integers.get_typed_builtin() == TYPE_INT
        and typed_integers == [1, 2]
        and typed_weights.is_typed_key()
        and typed_weights.is_typed_value()
        and typed_weights.get_typed_key_builtin() == TYPE_STRING
        and typed_weights.get_typed_value_builtin() == TYPE_INT
        and typed_weights["left"] == 1
        and typed_nodes.is_typed()
        and typed_nodes.get_typed_builtin() == TYPE_OBJECT
        and typed_nodes.get_typed_class_name() == &"Node"
        and local_values.is_typed()
        and local_values.get_typed_builtin() == TYPE_INT
        and local_weights.is_typed_key()
        and local_weights.is_typed_value()
    )


func typed_container_roundtrip(values: Array[int]) -> Dictionary[String, int]:
    return {"size": values.size()}


func 计算标识符(值: int) -> int:
    var template := 1
    var _gdpp_id_74656d706c617465 := 2
    var π := 3
    var é := 4
    var é := 5
    return template + _gdpp_id_74656d706c617465 + π + é + é + 值


func _process(delta: float) -> void:
    if matrix_frame_output == "":
        return
    var started := Time.get_ticks_usec()
    matrix_frame_checksum = int(
        matrix_frame_workload.call("run_frame_batch", matrix_frame_iterations)
    )
    var elapsed := Time.get_ticks_usec() - started
    if matrix_frame_index >= matrix_frame_warmup:
        matrix_frame_intervals_us.append(delta * 1000000.0)
        matrix_frame_workload_us.append(elapsed)
    matrix_frame_index += 1
    if matrix_frame_index < matrix_frame_samples + matrix_frame_warmup:
        return
    var report := {
        "schema": 1,
        "frame_intervals_us": matrix_frame_intervals_us,
        "workload_us": matrix_frame_workload_us,
        "iterations_per_frame": matrix_frame_iterations,
        "checksum": matrix_frame_checksum,
    }
    var output := FileAccess.open(matrix_frame_output, FileAccess.WRITE)
    if not output:
        push_error("GDPP_MATRIX frame cannot open output")
        matrix_frame_output = ""
        get_tree().quit(2)
        return
    output.store_string(JSON.stringify(report))
    output.close()
    matrix_frame_output = ""
    print("GDPP_MATRIX_FRAME_OK")
    get_tree().quit(0)


func move_by(delta: Vector2) -> void:
    position += delta
    queue_redraw()
    if Input.is_action_pressed("ui_accept"):
        print("moving")


func normalize_mode(value: MovementMode) -> MovementMode:
    return value


func classify_mode(value: int) -> String:
    match value:
        MovementMode.IDLE, MovementMode.WALK:
            return "slow"
        var captured when captured == MovementMode.RUN:
            return "run"
        _:
            return "unknown"


func classify_dynamic(value: Variant) -> String:
    match value:
        "ready":
            return "ok"
        _:
            return "other"


func is_node(value: Variant) -> bool:
    return value is Node


func is_string(value: Variant) -> bool:
    return value is String


func dynamic_call(target: Variant, value: int) -> Variant:
    return target.update_score(value)


func dynamic_size(target: Variant) -> Variant:
    return target.length()


func dynamic_read(target: Variant) -> Variant:
    return target.accessor_score


func dynamic_write(target: Variant, value: Variant) -> void:
    target.accessor_score = value


func dynamic_increment(target: Variant, value: Variant) -> void:
    target.accessor_score += value


func dynamic_plain_read(target: Variant) -> Variant:
    return target.assertion_evaluations


func dynamic_plain_write(target: Variant, value: Variant) -> void:
    target.assertion_evaluations = value


func dynamic_key_read(target: Variant, key: Variant) -> Variant:
    return target[key]


func dynamic_key_write(target: Variant, key: Variant, value: Variant) -> Variant:
    target[key] = value
    return target[key]


func dynamic_key_increment(
    target: Variant, key: Variant, value: Variant
) -> Variant:
    target[key] += value
    return target[key]


func dynamic_named_read(target: Variant) -> Variant:
    return target.uuid


func dynamic_named_write(target: Variant, value: Variant) -> Variant:
    target.uuid = value
    return target.uuid


func dynamic_named_nested_increment(target: Variant, increment: int) -> Variant:
    target.profile.score += increment
    return target.profile.score


func dynamic_component_write(target: Variant) -> Vector2:
    target.position.y -= 3.0
    return target.position


func dynamic_variant_component_write() -> Color:
    var value: Variant = Color(0.1, 0.2, 0.3, 1.0)
    value.a = 0.25
    return value


func dynamic_member_component_write() -> Color:
    dynamic_accent.a = 0.75
    return dynamic_accent


func dynamic_deep_component_write() -> Vector3:
    var value: Variant = Transform3D.IDENTITY
    value.origin.x = 4.0
    return value.origin


func dynamic_record_component_write(records: Array) -> Color:
    records[0].tint.a = 0.5
    return records[0].tint


func sum_dynamic(values: Variant) -> int:
    var total: int = 0
    for value in values:
        if value == 2:
            continue
        total += value
    return total


func join_dynamic(values: Variant) -> String:
    var result := ""
    for value in values:
        result += value
    return result


func invoke_callable(callback: Callable, value: int) -> Variant:
    return callback.call(value)


func invoke_dynamic_callable(callback: Variant, value: int) -> Variant:
    return callback.call(value)


func update_score(value: int) -> int:
    accessor_score = value
    return accessor_score


func validate_accessor() -> bool:
    if update_score(-5) != 0:
        return false
    return update_score(35) == 35


func validate_inheritance() -> bool:
    var child: Node = get_node_or_null(NodePath("InheritanceChild"))
    if not child:
        return false
    var answer: Variant = child.call("child_answer")
    return answer == 42


func assertion_evaluation_count() -> int:
    assertion_evaluations = 0
    assert(_record_assertion_evaluation())
    return assertion_evaluations


func _record_assertion_evaluation() -> bool:
    assertion_evaluations += 1
    return true
