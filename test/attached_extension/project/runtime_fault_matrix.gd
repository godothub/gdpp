extends RefCounted

var trace: Array[String] = []
var retained_target: Node
var retained_array_node: Node


func run() -> String:
    var cases: Array[Callable] = [
        array_oob_read,
        array_oob_write,
        packed_byte_oob_read,
        packed_byte_oob_write,
        packed_int32_oob_read,
        packed_int32_oob_write,
        packed_int64_oob_read,
        packed_int64_oob_write,
        packed_float32_oob_read,
        packed_float32_oob_write,
        packed_float64_oob_read,
        packed_float64_oob_write,
        packed_string_oob_read,
        packed_string_oob_write,
        packed_vector2_oob_read,
        packed_vector2_oob_write,
        packed_vector3_oob_read,
        packed_vector3_oob_write,
        packed_vector4_oob_read,
        packed_vector4_oob_write,
        packed_color_oob_read,
        packed_color_oob_write,
        wrong_array_key,
        missing_dictionary_key,
        missing_method,
        missing_property_read,
        missing_property_write,
        null_method,
        freed_method,
        freed_parameter,
        freed_field,
        freed_capture,
        null_callable,
        freed_callable,
        integer_divide_zero,
        integer_modulo_zero,
        invalid_operator,
        invalid_implicit_conversion,
        required_object_argument,
        invalid_explicit_conversion,
        invalid_typed_array_append,
        invalid_typed_array_write,
        valid_typed_array_dynamic_write,
        invalid_typed_object_array_write,
        invalid_typed_array_compound_write,
        invalid_typed_array_assignment,
        invalid_typed_object_array_append,
        invalid_typed_dictionary_key,
        invalid_typed_dictionary_read_key,
        missing_typed_dictionary_key,
        valid_typed_dictionary_dynamic_write,
        invalid_typed_dictionary_compound_key,
        invalid_typed_dictionary_compound_value,
        invalid_typed_dictionary_set,
        invalid_typed_dictionary_assignment,
        invalid_packed_array_assignment,
        integer_iteration,
    ]
    for index in cases.size():
        cases[index].call()
        _release_retained_objects()
        trace.append("caller:%d" % index)
    return JSON.stringify(trace)


func _release_retained_objects() -> void:
    # Faults abort the called function but deliberately return control to this oracle. Native
    # Objects stored by that function therefore need an outer owner which survives the fault frame.
    if retained_array_node != null and is_instance_valid(retained_array_node):
        retained_array_node.free()
    retained_array_node = null
    retained_target = null


func before(name: String) -> void:
    trace.append("before:" + name)


func after(name: String) -> void:
    trace.append("after:" + name)


func array_oob_read() -> void:
    before("array_oob_read")
    var value: Variant = [1][4]
    after("array_oob_read:" + str(value))


func array_oob_write() -> void:
    before("array_oob_write")
    var values := [1]
    values[4] = 2
    after("array_oob_write")


func packed_byte_oob_read() -> void:
    before("packed_byte_oob_read")
    var value: Variant = PackedByteArray([1])[4]
    after("packed_byte_oob_read:" + str(value))


func packed_byte_oob_write() -> void:
    before("packed_byte_oob_write")
    var values := PackedByteArray([1])
    values[4] = 2
    after("packed_byte_oob_write")


func packed_int32_oob_read() -> void:
    before("packed_int32_oob_read")
    var value: Variant = PackedInt32Array([1])[4]
    after("packed_int32_oob_read:" + str(value))


func packed_int32_oob_write() -> void:
    before("packed_int32_oob_write")
    var values := PackedInt32Array([1])
    values[4] = 2
    after("packed_int32_oob_write")


func packed_int64_oob_read() -> void:
    before("packed_int64_oob_read")
    var value: Variant = PackedInt64Array([1])[4]
    after("packed_int64_oob_read:" + str(value))


func packed_int64_oob_write() -> void:
    before("packed_int64_oob_write")
    var values := PackedInt64Array([1])
    values[4] = 2
    after("packed_int64_oob_write")


func packed_float32_oob_read() -> void:
    before("packed_float32_oob_read")
    var value: Variant = PackedFloat32Array([1.0])[4]
    after("packed_float32_oob_read:" + str(value))


func packed_float32_oob_write() -> void:
    before("packed_float32_oob_write")
    var values := PackedFloat32Array([1.0])
    values[4] = 2.0
    after("packed_float32_oob_write")


func packed_float64_oob_read() -> void:
    before("packed_float64_oob_read")
    var value: Variant = PackedFloat64Array([1.0])[4]
    after("packed_float64_oob_read:" + str(value))


func packed_float64_oob_write() -> void:
    before("packed_float64_oob_write")
    var values := PackedFloat64Array([1.0])
    values[4] = 2.0
    after("packed_float64_oob_write")


func packed_string_oob_read() -> void:
    before("packed_string_oob_read")
    var value: Variant = PackedStringArray(["one"])[4]
    after("packed_string_oob_read:" + str(value))


func packed_string_oob_write() -> void:
    before("packed_string_oob_write")
    var values := PackedStringArray(["one"])
    values[4] = "two"
    after("packed_string_oob_write")


func packed_vector2_oob_read() -> void:
    before("packed_vector2_oob_read")
    var value: Variant = PackedVector2Array([Vector2.ZERO])[4]
    after("packed_vector2_oob_read:" + str(value))


func packed_vector2_oob_write() -> void:
    before("packed_vector2_oob_write")
    var values := PackedVector2Array([Vector2.ZERO])
    values[4] = Vector2.ONE
    after("packed_vector2_oob_write")


func packed_vector3_oob_read() -> void:
    before("packed_vector3_oob_read")
    var value: Variant = PackedVector3Array([Vector3.ZERO])[4]
    after("packed_vector3_oob_read:" + str(value))


func packed_vector3_oob_write() -> void:
    before("packed_vector3_oob_write")
    var values := PackedVector3Array([Vector3.ZERO])
    values[4] = Vector3.ONE
    after("packed_vector3_oob_write")


func packed_vector4_oob_read() -> void:
    before("packed_vector4_oob_read")
    var value: Variant = PackedVector4Array([Vector4.ZERO])[4]
    after("packed_vector4_oob_read:" + str(value))


func packed_vector4_oob_write() -> void:
    before("packed_vector4_oob_write")
    var values := PackedVector4Array([Vector4.ZERO])
    values[4] = Vector4.ONE
    after("packed_vector4_oob_write")


func packed_color_oob_read() -> void:
    before("packed_color_oob_read")
    var value: Variant = PackedColorArray([Color.BLACK])[4]
    after("packed_color_oob_read:" + str(value))


func packed_color_oob_write() -> void:
    before("packed_color_oob_write")
    var values := PackedColorArray([Color.BLACK])
    values[4] = Color.WHITE
    after("packed_color_oob_write")


func wrong_array_key() -> void:
    before("wrong_array_key")
    var values: Variant = [1]
    var value: Variant = values["bad"]
    after("wrong_array_key:" + str(value))


func missing_dictionary_key() -> void:
    before("missing_dictionary_key")
    var values: Variant = {"present": 1}
    var value: Variant = values["missing"]
    after("missing_dictionary_key:" + str(value))


func missing_method() -> void:
    before("missing_method")
    var target: Variant = RefCounted.new()
    target.missing_method()
    after("missing_method")


func missing_property_read() -> void:
    before("missing_property_read")
    var target: Variant = RefCounted.new()
    var value: Variant = target.missing_property
    after("missing_property_read:" + str(value))


func missing_property_write() -> void:
    before("missing_property_write")
    var target: Variant = RefCounted.new()
    target.missing_property = 1
    after("missing_property_write")


func null_method() -> void:
    before("null_method")
    var target: Node = null
    target.get_child_count()
    after("null_method")


func freed_method() -> void:
    before("freed_method")
    var target := Node.new()
    target.free()
    target.get_child_count()
    after("freed_method")


func freed_parameter() -> void:
    var target := Node.new()
    freed_parameter_fault(target)


func freed_parameter_fault(target: Node) -> void:
    before("freed_parameter")
    target.free()
    target.get_child_count()
    after("freed_parameter")


func freed_field() -> void:
    before("freed_field")
    retained_target = Node.new()
    retained_target.free()
    retained_target.get_child_count()
    after("freed_field")


func freed_capture() -> void:
    var target := Node.new()
    var callback := func() -> void:
        before("freed_capture")
        target.get_child_count()
        after("freed_capture")
    target.free()
    callback.call()


func null_callable() -> void:
    before("null_callable")
    var callback := Callable()
    callback.call()
    after("null_callable")


func freed_callable() -> void:
    before("freed_callable")
    var target := Node.new()
    var callback := Callable(target, &"get_child_count")
    target.free()
    callback.call()
    after("freed_callable")


func integer_divide_zero() -> void:
    before("integer_divide_zero")
    var zero := 0
    var value := 7 / zero
    after("integer_divide_zero:" + str(value))


func integer_modulo_zero() -> void:
    before("integer_modulo_zero")
    var zero := 0
    var value := 7 % zero
    after("integer_modulo_zero:" + str(value))


func invalid_operator() -> void:
    before("invalid_operator")
    var left: Variant = 1
    var right: Variant = "bad"
    var value: Variant = left + right
    after("invalid_operator:" + str(value))


func invalid_implicit_conversion() -> void:
    before("invalid_implicit_conversion")
    var source: Variant = "bad"
    var value: int = source
    after("invalid_implicit_conversion:" + str(value))


func required_object_argument() -> void:
    before("required_object_argument")
    Input.parse_input_event(null)
    after("required_object_argument")


func invalid_explicit_conversion() -> void:
    before("invalid_explicit_conversion")
    var source: Variant = "bad"
    var value := int(source)
    after("invalid_explicit_conversion:" + str(value))


func invalid_typed_array_append() -> void:
    before("invalid_typed_array_append")
    var values: Array[int] = []
    var source: Variant = "bad"
    values.append(source)
    after("invalid_typed_array_append")


func invalid_typed_array_write() -> void:
    before("invalid_typed_array_write")
    var values: Array[int] = [1]
    var source: Variant = "bad"
    values[0] = source
    after("invalid_typed_array_write")


func valid_typed_array_dynamic_write() -> void:
    before("valid_typed_array_dynamic_write")
    var values: Array[int] = [1]
    var source: Variant = 4
    values[0] = source
    after("valid_typed_array_dynamic_write:" + str(values[0]))


func invalid_typed_object_array_write() -> void:
    before("invalid_typed_object_array_write")
    retained_array_node = Node.new()
    var values: Array[Node] = [retained_array_node]
    var source: Variant = RefCounted.new()
    values[0] = source
    after("invalid_typed_object_array_write")


func invalid_typed_array_compound_write() -> void:
    before("invalid_typed_array_compound_write")
    var values: Array[int] = [1]
    var source: Variant = 0.5
    values[0] += source
    after("invalid_typed_array_compound_write:" + str(values[0]))


func invalid_typed_array_assignment() -> void:
    before("invalid_typed_array_assignment")
    var source: Variant = ["bad"]
    var values: Array[int] = source
    after("invalid_typed_array_assignment:" + str(values))


func invalid_typed_object_array_append() -> void:
    before("invalid_typed_object_array_append")
    var values: Array[Node] = []
    var source: Variant = RefCounted.new()
    values.append(source)
    after("invalid_typed_object_array_append")


func invalid_typed_dictionary_key() -> void:
    before("invalid_typed_dictionary_key")
    var values: Dictionary[String, int] = {}
    var source: Variant = 4
    values[source] = 1
    after("invalid_typed_dictionary_key")


func invalid_typed_dictionary_read_key() -> void:
    before("invalid_typed_dictionary_read_key")
    var values: Dictionary[String, Variant] = {"value": 1}
    var source: Variant = 4
    var value: Variant = values[source]
    after("invalid_typed_dictionary_read_key:" + str(value))


func missing_typed_dictionary_key() -> void:
    before("missing_typed_dictionary_key")
    var values: Dictionary[String, Variant] = {"value": 1}
    var source: Variant = "missing"
    var value: Variant = values[source]
    after("missing_typed_dictionary_key:" + str(value))


func valid_typed_dictionary_dynamic_write() -> void:
    before("valid_typed_dictionary_dynamic_write")
    var values: Dictionary[String, int] = {}
    var key: Variant = "value"
    var value: Variant = 4
    values[key] = value
    after("valid_typed_dictionary_dynamic_write:" + str(values["value"]))


func invalid_typed_dictionary_compound_key() -> void:
    before("invalid_typed_dictionary_compound_key")
    var values: Dictionary[String, int] = {"value": 1}
    var source: Variant = 4
    values[source] += 1
    after("invalid_typed_dictionary_compound_key")


func invalid_typed_dictionary_compound_value() -> void:
    before("invalid_typed_dictionary_compound_value")
    var values: Dictionary[String, int] = {"value": 1}
    var source: Variant = 0.5
    values["value"] += source
    after("invalid_typed_dictionary_compound_value:" + str(values["value"]))


func invalid_typed_dictionary_set() -> void:
    before("invalid_typed_dictionary_set")
    var values: Dictionary[String, int] = {}
    var source: Variant = "bad"
    values["value"] = source
    after("invalid_typed_dictionary_set")


func invalid_typed_dictionary_assignment() -> void:
    before("invalid_typed_dictionary_assignment")
    var source: Variant = {4: "bad"}
    var values: Dictionary[String, int] = source
    after("invalid_typed_dictionary_assignment:" + str(values))


func invalid_packed_array_assignment() -> void:
    before("invalid_packed_array_assignment")
    var source: Variant = PackedStringArray(["bad"])
    var values: PackedInt32Array = source
    after("invalid_packed_array_assignment:" + str(values))


func integer_iteration() -> void:
    before("integer_iteration")
    var source: Variant = 42
    for value in source:
        trace.append(str(value))
    after("integer_iteration")
