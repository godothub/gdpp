extends "res://vendor_grandchild.gd"

const DEFERRED_PHYSICS_SCENE := preload("res://deferred_shape.tscn")
const CONTAINER_OWNER := preload("res://a_container_owner.gd")
const CONTAINER_VALUE := preload("res://z_container_value.gd")
const INNER_DATA := preload("res://inner_data.gd")
const NETWORK_IMAGE := preload("res://network_image.gd")
const RUNTIME_SHADER := preload("res://runtime_shader.gdshader")
const ATTACHED_SCENE := preload("res://attached_scene.tscn")

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


func _ready() -> void:
    super._ready()
    call_deferred(&"_verify_export_runtime")


func _verify_export_runtime() -> void:
    if not is_class(&"VendorBase"):
        _fail("export changed the provider-owned Node type")
        return
    if compute(3) != 62 or invoke_hook(4) != 54:
        _fail("export changed attached inheritance or native dispatch")
        return
    if get_ready_notifications() != 1:
        _fail("export did not preserve the provider lifecycle callback")
        return
    if (
        initialized != 1
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

    var lifecycle_probe: Variant = ATTACHED_SCENE.instantiate()
    add_child(lifecycle_probe)
    if (
        lifecycle_probe.initialized != 1
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
        _fail("binary message dispatch did not preserve onready arrays, callables, or signals")
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
