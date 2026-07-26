extends RefCounted

var trace: Array[String] = []
var retained_target: Node


func run() -> String:
    var cases: Array[Callable] = [
        array_oob_read,
        array_oob_write,
        packed_oob_read,
        packed_oob_write,
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
        invalid_typed_dictionary_set,
        integer_iteration,
    ]
    for index in cases.size():
        cases[index].call()
        trace.append("caller:%d" % index)
    return JSON.stringify(trace)


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


func packed_oob_read() -> void:
    before("packed_oob_read")
    var value: Variant = PackedByteArray([1])[4]
    after("packed_oob_read:" + str(value))


func packed_oob_write() -> void:
    before("packed_oob_write")
    var values := PackedByteArray([1])
    values[4] = 2
    after("packed_oob_write")


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


func invalid_typed_dictionary_set() -> void:
    before("invalid_typed_dictionary_set")
    var values: Dictionary[String, int] = {}
    var source: Variant = "bad"
    values["value"] = source
    after("invalid_typed_dictionary_set")


func integer_iteration() -> void:
    before("integer_iteration")
    var source: Variant = 42
    for value in source:
        trace.append(str(value))
    after("integer_iteration")
