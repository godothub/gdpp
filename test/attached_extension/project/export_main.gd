extends "res://vendor_grandchild.gd"

const DEFERRED_PHYSICS_SCENE := preload("res://deferred_shape.tscn")
const CONTAINER_OWNER := preload("res://a_container_owner.gd")
const CONTAINER_VALUE := preload("res://z_container_value.gd")
const INNER_DATA := preload("res://inner_data.gd")
const NETWORK_IMAGE := preload("res://network_image.gd")
const RPC_RUNTIME_HARNESS := preload("res://rpc_runtime_harness.gd")
const RUNTIME_FAULT_MATRIX := preload("res://runtime_fault_matrix.gd")
const RUNTIME_VALUE_MATRIX := preload("res://runtime_value_matrix.gd")
const AWAIT_DEFAULT_PROBE := preload("res://await_default_probe.gd")
const COROUTINE_ACCESSOR_PROBE := preload("res://coroutine_accessor_probe.gd")
const STATE_TRANSPORT_PROBE := preload("res://state_transport_probe.gd")
const RUNTIME_SHADER := preload("res://runtime_shader.gdshader")
const ATTACHED_SCENE := preload("res://attached_scene.tscn")

signal first_lambda_resume
signal second_lambda_resume

var _network_server: TCPServer
var _network_peer: StreamPeerTCP
var _network_png: PackedByteArray
var _network_sprite: Sprite2D
var _network_deadline_msec := 0
var _network_response_sent := false
var _shader_rect: TextureRect
var _shader_material: ShaderMaterial
var _shader_process_ticks := 0
var _script_items: Array[ContainerItem] = []
var _script_lookup: Dictionary[String, ContainerItem] = {}
var _child_ping_value := -1
var _vendor_ping_value := -1
var _vendor_contract_seen := false


class ContainerItem extends RefCounted:
    var value: int


    func _init(initial: int) -> void:
        value = initial


class InnerCoroutineProbe extends RefCounted:
    func forward(signal_value: Signal) -> int:
        return await later(signal_value)


    func later(signal_value: Signal) -> int:
        await signal_value
        return 42


static func _await_static_result(signal_value: Signal) -> int:
    await signal_value
    return 42


static func _sum_static_arguments(values: Array) -> int:
    return values[0] + values[1]


func _make_delayed_adder(captured: int) -> Callable:
    return func delayed(signal_value: Signal, addend: int) -> int:
        await signal_value
        return captured + addend


func _ready() -> void:
    super._ready()
    var user_args := OS.get_cmdline_user_args()
    if user_args.has("--gdpp-await-default-oracle"):
        call_deferred(&"_verify_await_default_oracle")
    elif user_args.has("--gdpp-coroutine-accessor-oracle"):
        call_deferred(&"_verify_coroutine_accessor_oracle")
    elif user_args.has("--gdpp-fault-matrix"):
        call_deferred(&"_verify_fault_matrix")
    else:
        call_deferred(&"_verify_export_runtime")


func _verify_await_default_oracle() -> void:
    var probe: Variant = AWAIT_DEFAULT_PROBE.new()
    var failure: String = await probe.verify(get_tree())
    if not failure.is_empty():
        _fail(failure)
        return
    print("GDPP_AWAIT_DEFAULT_AOT_RUNTIME_OK")
    get_tree().quit(0)


func _verify_coroutine_accessor_oracle() -> void:
    var probe: Variant = COROUTINE_ACCESSOR_PROBE.new()
    var failure: String = await probe.verify(get_tree())
    if not failure.is_empty():
        _fail(failure)
        return
    print("GDPP_COROUTINE_ACCESSOR_AOT_RUNTIME_OK")
    get_tree().quit(0)


func _verify_fault_matrix() -> void:
    var matrix: Variant = RUNTIME_FAULT_MATRIX.new()
    print("GDPP_FAULT_MATRIX=" + matrix.run())
    var value_matrix: Variant = RUNTIME_VALUE_MATRIX.new()
    print("GDPP_VALUE_MATRIX=" + value_matrix.run())
    var await_default_probe: Variant = AWAIT_DEFAULT_PROBE.new()
    var await_default_failure: String = await await_default_probe.verify(get_tree())
    if not await_default_failure.is_empty():
        _fail(await_default_failure)
        return
    print("GDPP_AWAIT_DEFAULT_MATRIX=ok")
    var coroutine_accessor_probe: Variant = COROUTINE_ACCESSOR_PROBE.new()
    var coroutine_accessor_failure: String = await coroutine_accessor_probe.verify(get_tree())
    if not coroutine_accessor_failure.is_empty():
        _fail(coroutine_accessor_failure)
        return
    print("GDPP_COROUTINE_ACCESSOR_MATRIX=ok")
    get_tree().quit(0)


func _verify_export_runtime() -> void:
    var animation_storage_probe := get_node("AnimationStorageProbe")
    if (
        animation_storage_probe == null
        or not animation_storage_probe.call(&"has_serialized_library")
    ):
        _fail("native compatibility storage was lost while attaching the compiled script")
        return
    if not animation_storage_probe.call(&"has_serialized_node_reference"):
        _fail("serialized script Node reference was not resolved after AOT attachment")
        return
    if not animation_storage_probe.call(&"has_serialized_typed_container"):
        _fail("serialized typed container metadata was not preserved after AOT attachment")
        return
    if not animation_storage_probe.call(&"has_script_resource_identity"):
        _fail("compiled script identity did not preserve Resource property access")
        return
    var uppercase: Callable = "gdpp".to_upper
    if uppercase.call() != "GDPP":
        _fail("bound builtin method Callable lost its receiver or result")
        return
    var child_count: Callable = self.get_child_count
    if child_count.call() != get_child_count():
        _fail("bound Godot object method Callable lost its receiver or result")
        return
    var first_static: Callable = _sum_static_arguments
    var second_static: Callable = _sum_static_arguments
    if first_static != second_static or first_static.hash() != second_static.hash():
        _fail("repeated static function values lost their stable Callable identity")
        return
    var static_arguments := [19, 23]
    var bound_static := (
        first_static.bind(static_arguments)
        if first_static == _sum_static_arguments
        else first_static.bindv(static_arguments)
    )
    if bound_static.get_argument_count() != 0 or bound_static.call() != 42:
        _fail("static Callable identity selected incompatible bound argument semantics")
        return
    var widened_return: WidenedReturnBase = WidenedReturnChild.new()
    var widened_total := 0
    for item in widened_return.values():
        widened_total += item.value
    if widened_total != 7:
        _fail("dynamic override return storage was narrowed before direct iteration")
        return
    var exact_return: WidenedReturnBase = WidenedReturnBase.new()
    var exact_values := exact_return.values()
    if not exact_values.is_typed() or exact_values.get_typed_builtin() != TYPE_DICTIONARY:
        _fail("dynamic return storage lost its typed container contract at assignment")
        return
    var deferred_emit_seen := [false]
    first_lambda_resume.connect(
        func() -> void: deferred_emit_seen[0] = true,
        CONNECT_ONE_SHOT,
    )
    first_lambda_resume.emit.call_deferred()
    await get_tree().process_frame
    if not deferred_emit_seen[0]:
        _fail("bound Signal method Callable was not deferred or invoked")
        return

    var static_result: int = await _await_static_result(
        get_tree().create_timer(0.001).timeout,
    )
    if static_result != 42:
        _fail("static coroutine lost its completion owner or typed return value")
        return
    var delayed_adder := _make_delayed_adder(38)
    var lambda_result: int = await delayed_adder.call(
        get_tree().create_timer(0.001).timeout,
        4,
    )
    if lambda_result != 42:
        _fail("typed coroutine lambda lost its captured value or completion result")
        return
    var inner_result: int = await InnerCoroutineProbe.new().forward(
        get_tree().create_timer(0.001).timeout,
    )
    if inner_result != 42:
        _fail("internal class coroutine lost its temporary owner or typed result")
        return

    # No scene, preload, or constant references this script. It proves that binary-only exports
    # preserve the source-path Script identity for paths computed entirely at runtime, including
    # callers that use ResourceLoader directly instead of the global load() intrinsic.
    var dynamic_path := "res://dynamic_" + "load_target.gd"
    var threaded_error := ResourceLoader.load_threaded_request(dynamic_path, "Script", true)
    if threaded_error != OK:
        _fail("threaded dynamic compiled Script loading could not be scheduled")
        return
    var threaded_status := ResourceLoader.load_threaded_get_status(dynamic_path)
    while threaded_status == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
        await get_tree().process_frame
        threaded_status = ResourceLoader.load_threaded_get_status(dynamic_path)
    if threaded_status != ResourceLoader.THREAD_LOAD_LOADED:
        _fail("threaded dynamic compiled Script loading did not complete")
        return
    var dynamic_script: Variant = ResourceLoader.load_threaded_get(dynamic_path)
    var relative_path := dynamic_path.trim_prefix("res://")
    var relative_script: Variant = load(relative_path)
    var statically_resolved_script: Variant = load("dynamic_" + "load_target.gd")
    var resource_loader_script: Variant = ResourceLoader.load(dynamic_path)
    var uid_script: Variant = ResourceLoader.load("uid://" + "34eqhh6l7jk6")
    var dynamic_checks := {
        "script_type": dynamic_script is Script,
        "relative_identity": relative_script == dynamic_script,
        "static_identity": statically_resolved_script == dynamic_script,
        "resource_loader_identity": resource_loader_script == dynamic_script,
        "uid_identity": uid_script == dynamic_script,
        "exists": ResourceLoader.exists(dynamic_path, "Script"),
        "missing_absent": not ResourceLoader.exists(
            "res://missing_dynamic_script.gd",
            "Script",
        ),
    }
    if (
        not dynamic_checks.script_type
        or not dynamic_checks.relative_identity
        or not dynamic_checks.static_identity
        or not dynamic_checks.resource_loader_identity
        or not dynamic_checks.uid_identity
        or not dynamic_checks.exists
        or not dynamic_checks.missing_absent
    ):
        _fail(
            "dynamic compiled script loading lost path, UID, type, cache, or exists semantics: %s"
            % [dynamic_checks],
        )
        return
    var dynamic_instance: Variant = dynamic_script.new()
    if (
        dynamic_instance == null
        or dynamic_instance.evaluate(2) != 42
        or dynamic_instance.get_script() != dynamic_script
    ):
        _fail("dynamic compiled Script construction lost behavior or canonical identity")
        return
    print("GDPP_DYNAMIC_SCRIPT_RUNTIME_OK")

    var completion_order: Array[int] = []
    var concurrent_lambda := func(signal_value: Signal, value: int) -> int:
        await signal_value
        completion_order.push_back(value)
        return value
    concurrent_lambda.call(first_lambda_resume, 11)
    concurrent_lambda.call(second_lambda_resume, 22)
    second_lambda_resume.emit()
    await get_tree().process_frame
    if completion_order != [22]:
        _fail("concurrent coroutine lambda calls shared suspension state")
        return
    first_lambda_resume.emit()
    await get_tree().process_frame
    if completion_order != [22, 11]:
        _fail("coroutine lambda resumptions were lost or reordered")
        return

    var recursive_cell: Array[Callable] = []
    var factorial := func(value: int) -> int:
        if value <= 1:
            return 1
        return value * recursive_cell[0].call(value - 1)
    recursive_cell.push_back(factorial)
    if factorial.call(5) != 120:
        _fail("recursive lambda capture did not preserve shared container identity")
        return

    var rpc_harness: Variant = RPC_RUNTIME_HARNESS.new()
    var rpc_failure: String = await rpc_harness.verify(get_tree())
    if not rpc_failure.is_empty():
        _fail(rpc_failure)
        return
    print("GDPP_RPC_RUNTIME_OK")

    var captured_array := [3]
    var captured_dictionary := {"value": 4}
    var captured_object := ContainerItem.new(5)
    var callable_factory := func(delta: int) -> Callable:
        return func() -> int:
            return (
                captured_array[0]
                + captured_dictionary["value"]
                + captured_object.value
                + delta
            )
    var returned_lambda: Callable = callable_factory.call(30)
    if returned_lambda.call() != 42:
        _fail("returned nested lambda lost a container, object, or local capture")
        return
    if not is_class(&"VendorBase"):
        _fail("export changed the provider-owned Node type")
        return
    if compute(3) != 62 or invoke_hook(4) != 54:
        _fail("export changed attached inheritance or native dispatch")
        return
    native_bias = 9
    if native_bias != 9:
        _fail("inherited GDExtension property dispatch changed after AOT")
        return
    if get_ready_notifications() != 1:
        _fail("export did not preserve the provider lifecycle callback")
        return
    if (
        initialized != 1
        or init_ping_value != bonus
        or not ready_seen
        or enter_tree_count != 1
        or ready_notification_count != 1
        or exit_tree_count != 0
    ):
        _fail("attached lifecycle callbacks were missing, repeated, or out of phase")
        return
    if _child_ping_value != bonus or _vendor_ping_value != bonus:
        _fail("script or provider ready signals were not connected before lifecycle dispatch")
        return
    var init_signal_count := 0
    for signal_info in get_signal_list():
        if signal_info.name == &"init_ping":
            init_signal_count += 1
    if init_signal_count != 1:
        _fail("construction-time script signals were duplicated in Object introspection")
        return

    var lifecycle_probe: Variant = ATTACHED_SCENE.instantiate()
    add_child(lifecycle_probe)
    if (
        lifecycle_probe.initialized != 1
        or lifecycle_probe.init_ping_value != lifecycle_probe.bonus
        or not lifecycle_probe.ready_seen
        or lifecycle_probe.enter_tree_count != 1
        or lifecycle_probe.ready_notification_count != 1
        or lifecycle_probe.exit_tree_count != 0
    ):
        _fail("runtime-created attached node did not enter and become ready exactly once")
        return
    remove_child(lifecycle_probe)
    if lifecycle_probe.exit_tree_count != 1:
        _fail("runtime-created attached node did not receive exactly one exit-tree callback")
        return
    lifecycle_probe.queue_free()

    var contract_result: Variant = verify_vendor_contract(
        "provider-payload",
        [3, 5],
        {"left": 7, "right": 11},
    )
    if contract_result != "provider-payload" or not _vendor_contract_seen:
        _fail("provider signal or typed container roundtrip changed across attachment")
        return

    var onready_probe: Variant = get_node("OnreadyProbe")
    if onready_probe == null or not onready_probe.initialized():
        _fail("attached onready-only script did not receive implicit ready initialization")
        return

    var inherited_onready_probe: Variant = get_node("InheritedOnreadyProbe")
    if inherited_onready_probe == null or not inherited_onready_probe.initialized():
        _fail("attached onready initialization did not run base-first before explicit ready")
        return

    var inner_onready_probe: Variant = get_node("InnerOnreadyProbe")
    if inner_onready_probe == null or not inner_onready_probe.verify_inner_onready():
        _fail("attached internal class did not receive implicit ready initialization")
        return

    var message_dispatch_probe: Variant = get_node("MessageDispatchProbe")
    if message_dispatch_probe == null or not message_dispatch_probe.dispatch_burst(128):
        _fail(
            "gift message simulation did not preserve framing, parsing, dispatch, or UI updates",
        )
        return

    var data := load("res://vendor_data.tres")
    if data == null or not data.is_class(&"VendorResource"):
        _fail("export changed the provider-owned Resource type")
        return
    if data.compute(2) != 62:
        _fail("export changed attached Resource fields or native dispatch")
        return

    var physics_fixture := DEFERRED_PHYSICS_SCENE.instantiate()
    if physics_fixture == null:
        _fail("deferred attached-script preload did not materialize at runtime")
        return
    var collision_shape := physics_fixture.get_node("CollisionShape2D") as CollisionShape2D
    if collision_shape == null or not collision_shape.shape is CircleShape2D:
        _fail("deferred attached-script preload lost its physics resource")
        return
    physics_fixture.queue_free()

    var reflected_constants: Dictionary = get_script().get_script_constant_map()
    if reflected_constants.get(&"DEFERRED_PHYSICS_SCENE") != DEFERRED_PHYSICS_SCENE:
        _fail("attached Script reflection did not materialize a local deferred constant")
        return
    if reflected_constants.get(&"INHERITED_PHYSICS_SCENE") != INHERITED_PHYSICS_SCENE:
        _fail("attached Script reflection did not inherit a deferred constant")
        return

    var prebound_inline := get_node("PreboundInlineShader") as TextureRect
    var prebound_external := get_node("PreboundExternalShader") as TextureRect
    var inline_material := prebound_inline.material as ShaderMaterial
    var external_material := prebound_external.material as ShaderMaterial
    if (
        inline_material == null
        or external_material == null
        or inline_material.shader != RUNTIME_SHADER
        or external_material.shader != RUNTIME_SHADER
        or RUNTIME_SHADER.resource_path != "res://runtime_shader.gdshader"
        or not is_equal_approx(
            float(inline_material.get_shader_parameter(&"pulse")),
            0.375,
        )
        or not is_equal_approx(
            float(external_material.get_shader_parameter(&"pulse")),
            0.625,
        )
    ):
        _fail("AOT scene rewrite changed a prebound ShaderMaterial resource contract")
        return
    inline_material.set_shader_parameter(&"pulse", 0.5)
    external_material.set_shader_parameter(&"pulse", 0.75)
    if (
        not is_equal_approx(
            float(inline_material.get_shader_parameter(&"pulse")),
            0.5,
        )
        or not is_equal_approx(
            float(external_material.get_shader_parameter(&"pulse")),
            0.75,
        )
    ):
        _fail("prebound ShaderMaterial uniforms were not writable after AOT scene rewrite")
        return

    var dynamic_entry: Variant = INNER_DATA.Entry.new()
    dynamic_entry.count = 42
    dynamic_entry.label = "attached"
    var indexed_entries: Dictionary[int, Variant] = {}
    indexed_entries[dynamic_entry.count] = dynamic_entry
    if (
        dynamic_entry.count != 42
        or dynamic_entry.label != "attached"
        or indexed_entries.get(42) != dynamic_entry
    ):
        _fail("dynamic attached inner-class properties lost typed getter/setter semantics")
        return

    var transported_source: Variant = STATE_TRANSPORT_PROBE.new()
    transported_source.value = 10
    var transported_state: Dictionary = inst_to_dict(transported_source)
    if (
        transported_state.get(&"@path") != "res://state_transport_probe.gd"
        or transported_state.get(&"@subpath") != NodePath()
        or transported_state.get(&"value") != 11
    ):
        _fail("instance dictionary transport invoked an accessor or lost its script identity")
        return
    transported_state[&"value"] = 23
    transported_state.erase(&"initializer_marker")
    transported_state.erase(&"constructor_marker")
    var transported_result: Variant = dict_to_inst(transported_state)
    if (
        transported_result == null
        or transported_result.get_script() != STATE_TRANSPORT_PROBE
        or transported_result.value != 123
        or transported_result.initializer_marker != 0
        or transported_result.constructor_marker != 0
    ):
        _fail(
            "dictionary restoration replayed initialization, invoked a setter, or lost Script identity",
        )
        return

    var inner_state: Dictionary = inst_to_dict(dynamic_entry)
    var restored_entry: Variant = dict_to_inst(inner_state)
    if (
        inner_state.get(&"@path") != "res://inner_data.gd"
        or inner_state.get(&"@subpath") != NodePath("Entry")
        or restored_entry == null
        or restored_entry.get_script() != dynamic_entry.get_script()
        or restored_entry.count != 42
        or restored_entry.label != "attached"
    ):
        _fail("internal-class dictionary transport lost its subpath, fields, or Script identity")
        return

    _shader_rect = TextureRect.new()
    add_child(_shader_rect)
    _shader_material = ShaderMaterial.new()
    _shader_material.shader = RUNTIME_SHADER
    _shader_material.set_shader_parameter(&"pulse", 0.25)
    _shader_rect.material = _shader_material
    if (
        _shader_rect.material != _shader_material
        or not _shader_rect.material is ShaderMaterial
        or not is_equal_approx(
            float((_shader_rect.material as ShaderMaterial).get_shader_parameter(&"pulse")),
            0.25,
        )
    ):
        _fail("derived ShaderMaterial was not assigned through the Material property ABI")
        return

    var item := ContainerItem.new(73)
    _script_items.push_back(item)
    _script_lookup["runtime"] = item
    if (
        _script_items.size() != 1
        or _script_lookup.size() != 1
        or _script_items[0].value != 73
        or _script_lookup["runtime"].value != 73
        or _script_items.get_typed_class_name() != &"RefCounted"
        or _script_items.get_typed_script() == null
        or _script_items.get_typed_script() != item.get_script()
        or _script_lookup.get_typed_value_class_name() != &"RefCounted"
        or _script_lookup.get_typed_value_script() == null
        or _script_lookup.get_typed_value_script() != item.get_script()
    ):
        _fail("attached script objects lost exact typed-container metadata")
        return

    var cross_owner := CONTAINER_OWNER.new()
    var cross_value := CONTAINER_VALUE.new(91)
    cross_owner.store(cross_value)
    if (
        cross_owner.values.size() != 1
        or cross_owner.values[0].value != 91
        or cross_owner.values.get_typed_class_name() != &"RefCounted"
        or cross_owner.values.get_typed_script() != cross_value.get_script()
    ):
        _fail("cross-script typed-container identity depended on descriptor registration order")
        return

    var image := Image.create(2, 2, false, Image.FORMAT_RGBA8)
    image.fill(Color(0.25, 0.5, 0.75, 1.0))
    var jpeg_image: Image = NETWORK_IMAGE.decode_image(
        image.save_jpg_to_buffer(0.9),
        "image/jpeg",
    )
    var webp_image: Image = NETWORK_IMAGE.decode_image(
        image.save_webp_to_buffer(false, 0.9),
        "",
    )
    var jpeg_texture: Texture2D = (
        ImageTexture.create_from_image(jpeg_image) if jpeg_image != null else null
    )
    var webp_texture: Texture2D = (
        ImageTexture.create_from_image(webp_image) if webp_image != null else null
    )
    if (
        jpeg_texture == null
        or webp_texture == null
        or jpeg_texture.get_width() != 2
        or jpeg_texture.get_height() != 2
        or webp_texture.get_width() != 2
        or webp_texture.get_height() != 2
    ):
        _fail("JPEG/WebP network image decoding or ImageTexture creation failed")
        return
    _network_png = image.save_png_to_buffer()
    _network_server = TCPServer.new()
    if _network_server.listen(0, "127.0.0.1") != OK:
        _fail("loopback HTTP fixture could not listen")
        return
    _network_sprite = Sprite2D.new()
    add_child(_network_sprite)
    var url := "http://127.0.0.1:%d/image.png" % _network_server.get_local_port()
    if NETWORK_IMAGE.load_into_sprite(url, _network_sprite) != OK:
        _fail("HTTPRequest rejected the loopback image URL")
        return
    _network_deadline_msec = Time.get_ticks_msec() + 5000


func _process(_delta: float) -> void:
    if _shader_material != null:
        _shader_process_ticks += 1
        _shader_material.set_shader_parameter(&"pulse", float(_shader_process_ticks))
    if _network_server == null:
        return
    if _network_peer == null and _network_server.is_connection_available():
        _network_peer = _network_server.take_connection()
    if _network_peer != null and not _network_response_sent:
        _network_peer.poll()
        var available := _network_peer.get_available_bytes()
        if available > 0:
            var request_text := _network_peer.get_utf8_string(available)
            if not request_text.begins_with("GET /image.png "):
                _fail("loopback HTTP request path was corrupted")
                return
            var response_headers := (
                "HTTP/1.1 200 OK\r\n"
                + "Content-Type: image/png\r\n"
                + "Content-Length: %d\r\n" % _network_png.size()
                + "Connection: close\r\n\r\n"
            )
            if (
                _network_peer.put_data(response_headers.to_utf8_buffer()) != OK
                or _network_peer.put_data(_network_png) != OK
            ):
                _fail("loopback HTTP response could not be written")
                return
            _network_response_sent = true
            _network_peer.disconnect_from_host()
            _network_server.stop()
    if _network_sprite != null and _network_sprite.has_meta(&"gdpp_network_error"):
        _fail(str(_network_sprite.get_meta(&"gdpp_network_error")))
        return
    if _network_sprite != null and _network_sprite.has_meta(&"gdpp_network_loaded"):
        var texture := _network_sprite.texture
        if (
            texture == null
            or not texture is ImageTexture
            or texture.get_width() != 2
            or texture.get_height() != 2
            or _shader_process_ticks <= 0
            or not is_equal_approx(
                float(_shader_material.get_shader_parameter(&"pulse")),
                float(_shader_process_ticks),
            )
        ):
            _fail("network ImageTexture or per-frame shader parameter state was not preserved")
            return
        print("GDPP_ATTACHED_EXPORT_RUNTIME_OK")
        get_tree().quit(0)
        return
    if Time.get_ticks_msec() >= _network_deadline_msec:
        _fail("loopback network image request timed out")


func _on_child_ping(value: int) -> void:
    _child_ping_value = value


func _on_vendor_ping(value: int) -> void:
    _vendor_ping_value = value


func _on_vendor_contract(
    values: Array[int],
    weights: Dictionary[String, int],
) -> void:
    _vendor_contract_seen = (
        values == [3, 5]
        and weights == {"left": 7, "right": 11}
        and values.get_typed_builtin() == TYPE_INT
        and weights.get_typed_key_builtin() == TYPE_STRING
        and weights.get_typed_value_builtin() == TYPE_INT
    )


func _fail(message: String) -> void:
    if _network_server != null:
        _network_server.stop()
    push_error("GDPP attached export runtime: %s" % message)
    get_tree().quit(1)
