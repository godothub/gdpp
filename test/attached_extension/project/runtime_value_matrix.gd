extends RefCounted

signal matrix_signal


func _identity(value: Variant) -> Variant:
    return value


func _stable_text(value: Variant, kind: Variant.Type) -> String:
    if kind in [TYPE_RID, TYPE_OBJECT, TYPE_CALLABLE, TYPE_SIGNAL]:
        return ""
    return str(value)


func _synthetic_builtin_members(
    vector: Vector2,
    vector_i: Vector2i,
    transform: Transform2D,
    plane: Plane,
    color: Color,
) -> Array:
    vector.x = vector.y
    vector_i.x = vector_i.y
    transform.x = transform.y
    plane.x = plane.y
    plane.d = plane.z
    color.r = color.g
    color.r8 = color.g8
    color.h = color.s
    color.ok_hsl_h = color.ok_hsl_s
    return [
        vector.x,
        vector_i.x,
        transform.x,
        plane.x,
        plane.d,
        color.r,
        color.r8,
        color.h,
        color.ok_hsl_h,
    ]


func run() -> String:
    var values: Array[Variant] = [
        null,
        false,
        42,
        3.5,
        "matrix",
        Vector2(1.25, -2.5),
        Vector2i(3, -4),
        Rect2(1.0, 2.0, 3.0, 4.0),
        Rect2i(1, 2, 3, 4),
        Vector3(1.0, 2.0, 3.0),
        Vector3i(1, 2, 3),
        Transform2D(1.0, Vector2(2.0, 3.0)),
        Vector4(1.0, 2.0, 3.0, 4.0),
        Vector4i(1, 2, 3, 4),
        Plane(Vector3.UP, 2.0),
        Quaternion(Vector3.UP, 0.5),
        AABB(Vector3.ONE, Vector3(2.0, 3.0, 4.0)),
        Basis(Vector3.UP, 0.5),
        Transform3D(Basis.IDENTITY, Vector3(1.0, 2.0, 3.0)),
        Projection.IDENTITY,
        Color(0.25, 0.5, 0.75, 1.0),
        StringName("matrix_name"),
        NodePath("Root/Child:value"),
        RID(),
        RefCounted.new(),
        Callable(self, &"_identity"),
        Signal(self, &"matrix_signal"),
        {"key": 7},
        [1, "two", true],
        PackedByteArray([0, 127, 255]),
        PackedInt32Array([-2, 0, 3]),
        PackedInt64Array([-4, 0, 5]),
        PackedFloat32Array([-1.5, 0.0, 2.5]),
        PackedFloat64Array([-3.5, 0.0, 4.5]),
        PackedStringArray(["one", "two"]),
        PackedVector2Array([Vector2(1.0, 2.0)]),
        PackedVector3Array([Vector3(1.0, 2.0, 3.0)]),
        PackedColorArray([Color(0.25, 0.5, 0.75, 1.0)]),
        PackedVector4Array([Vector4(1.0, 2.0, 3.0, 4.0)]),
    ]
    var copied: Array[Variant] = []
    copied.assign(values)
    var result: Array[Dictionary] = []
    for index in values.size():
        var value: Variant = values[index]
        var copy: Variant = copied[index]
        var truthy := false
        if value:
            truthy = true
        result.append(
            {
                "index": index,
                "type": type_string(typeof(value)),
                "copy_type": type_string(typeof(copy)),
                "equal": copy == value,
                "truthy": truthy,
                "text": _stable_text(value, typeof(value)),
            },
        )
    var payload := JSON.stringify(result)
    var digest := HashingContext.new()
    if digest.start(HashingContext.HASH_SHA256) != OK:
        return "hash_start_failed"
    if digest.update(payload.to_utf8_buffer()) != OK:
        return "hash_update_failed"
    return "%d:%s" % [result.size(), digest.finish().hex_encode()]
