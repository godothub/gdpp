#include "support/test.hpp"

#include "gdpp/compiler/compiler.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <thread>

TEST_CASE("compiler generates bindable GDExtension C++") {
    const std::string source = "extends Node\n"
                               "class_name Counter\n"
                               "signal changed(value: int)\n"
                               "var value: int = 0\n"
                               "func increment(amount: int) -> int:\n"
                               "    value += amount\n"
                               "    emit_signal(\"changed\", value)\n"
                               "    return value\n";
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("counter.gd", source);

    REQUIRE(result.success);
    REQUIRE_EQ(result.unit.script_class_name, std::string{"Counter"});
    REQUIRE_EQ(result.unit.class_name, std::string{"GDPPNative_Counter"});
    REQUIRE(result.unit.header.find("GDCLASS(GDPPNative_Counter, godot::Node)") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("ClassDB::bind_method") != std::string::npos);
    REQUIRE(result.unit.source.find("auto _gdpp_assignment_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("auto _gdpp_assignment_result_") != std::string::npos);
    REQUIRE(result.unit.source.find(" = std::move(") != std::string::npos);
    REQUIRE(result.unit.source.find("value = std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_property_right_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::integer::add(") != std::string::npos);
    REQUIRE(result.unit.source.find("ADD_SIGNAL") != std::string::npos);
    REQUIRE_EQ(result.unit.symbol_file_name, std::string{"counter.gd.symbols"});
    REQUIRE(result.unit.symbol_map.rfind("GDPP_SYMBOL_MAP 1\n", 0) == 0);
    REQUIRE(result.unit.symbol_map.find("source \"res://counter.gd\"") != std::string::npos);
    REQUIRE(result.unit.symbol_map.find("method \"increment\" \"GDPPNative_Counter::increment\"") !=
            std::string::npos);
    REQUIRE(result.unit.symbol_map.find("variant_callback \"increment\" "
                                        "\"GDPPNative_Counter::_gdpp_variant_call_increment\"") !=
            std::string::npos);
    REQUIRE(result.unit.symbol_map.find(
                "property_getter \"value:get\" \"GDPPNative_Counter::_gdpp_get_value\"") !=
            std::string::npos);
}

TEST_CASE("compiler accepts nested terminal branches after MIR CFG pruning") {
    const auto result = gdpp::Compiler{}.compile(
        "nested_terminal_branches.gd", "extends Node\n"
                                       "func choose(empty: bool, active: int) -> Variant:\n"
                                       "    if empty:\n"
                                       "        if active < 10:\n"
                                       "            var value: int = 1\n"
                                       "            return value\n"
                                       "        else:\n"
                                       "            return null\n"
                                       "    else:\n"
                                       "        var value: int = 2\n"
                                       "        if value > 0:\n"
                                       "            value += 1\n"
                                       "        return value\n");

    REQUIRE(result.success);
    REQUIRE(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [](const auto& diagnostic) { return diagnostic.code == "GDS5118"; }));
    REQUIRE(result.unit.source.find("choose") != std::string::npos);
}

TEST_CASE("semantic analysis accepts Godot rest parameters and unbounded calls") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_6;
    const auto result = gdpp::Compiler{}.compile(
        "rest_semantics.gd",
        "func collect(required: int, optional: int = 2, ...values: Array) -> int:\n"
        "    return required + optional + values.size()\n"
        "static func static_collect(...values) -> int:\n"
        "    return values.size()\n"
        "func invoke() -> int:\n"
        "    var local := func(prefix: int, ...parts: Array) -> int: return prefix + parts.size()\n"
        "    return collect(1, 2, 3, 4) + static_collect(1, 2, 3) + local.call(1, 2, 3)\n",
        options);

    REQUIRE(result.success);
}

TEST_CASE("compiler lowers instance and static rest methods through the vararg ABI") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_4;
    const auto result = gdpp::Compiler{}.compile(
        "rest_methods.gd",
        "func collect(required: int, optional: int = 2, ...values: Array) -> int:\n"
        "    return required + optional + values.size()\n"
        "static func join(first, ...values) -> int:\n"
        "    return values.size()\n"
        "func invoke() -> int:\n"
        "    return collect(1, 2, 3, 4) + join(5, 6, 7)\n",
        options);

    REQUIRE(result.success);
    REQUIRE(
        result.unit.header.find("godot::Variant _gdpp_argument_optional, godot::Array values") !=
        std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_variant_call_collect") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_variant_call_join") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::bind_vararg_method(") != std::string::npos);
    REQUIRE(result.unit.source.find("method.flags |= GDEXTENSION_METHOD_FLAG_STATIC") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_rest_arguments.resize(") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_call_rest_") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::GetTypeInfo<godot::Variant>::get_class_info()") !=
            std::string::npos);
}

TEST_CASE("compiler centralizes packed values at every generated Variant boundary") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_6;
    const auto result = gdpp::Compiler{}.compile(
        "packed_variant_boundaries.gd",
        "extends Node\n"
        "signal payload(value: PackedByteArray)\n"
        "func collect(prefix: String, ...values: Array) -> int:\n"
        "    return values.size()\n"
        "func exercise(target: Variant, callback: Callable, peer: StreamPeer,\n"
        "        node: Node, bytes: PackedByteArray) -> Variant:\n"
        "    payload.emit(bytes)\n"
        "    callback.call(bytes)\n"
        "    target.accept(bytes)\n"
        "    print(\"bytes\", bytes)\n"
        "    peer.put_data(bytes)\n"
        "    node.call(&\"receive_bytes\", bytes)\n"
        "    var values: Array = [bytes]\n"
        "    var lookup: Dictionary = {&\"bytes\": bytes}\n"
        "    match bytes:\n"
        "        var captured when not captured.is_empty(): return captured\n"
        "    return collect(\"payload\", values[0], lookup[&\"bytes\"])\n",
        options);

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("] = gdpp::runtime::to_variant(_gdpp_call_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("static const godot::Variant _gdpp_signal_name_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::emit_local_signal_at(") != std::string::npos);
    REQUIRE(result.unit.source.find(", this, _gdpp_signal_name_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::call_callable_at(") != std::string::npos);
    REQUIRE(result.unit.source.find(", _gdpp_callable_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::variant_constructor_argument("
                                    "_gdpp_callable_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find("const godot::Variant _gdpp_dynamic_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(" = gdpp::runtime::to_variant(bytes);") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_dynamic_result_") != std::string::npos);
    REQUIRE(result.unit.source.find(" = gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(result.unit.source.find(" = godot::Variant(_gdpp_dynamic_argument_") ==
            std::string::npos);
    REQUIRE(result.unit.source.find(" = gdpp::runtime::to_variant(_gdpp_call_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_match_bind_") != std::string::npos);
    REQUIRE(result.unit.source.find(" = gdpp::runtime::to_variant(_gdpp_match_value_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::UtilityFunctions::print("
                                    "gdpp::runtime::to_variant(_gdpp_utility_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                ", gdpp::runtime::variant_constructor_argument(_gdpp_utility_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::packed_native_argument(_gdpp_call_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("call(_gdpp_call_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find(
                ", gdpp::runtime::variant_constructor_argument(_gdpp_call_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_array_value_") != std::string::npos);
    REQUIRE(result.unit.source.find(" = gdpp::runtime::to_variant(bytes);") != std::string::npos);
    REQUIRE(result.unit.source.find("script_function_failed()") != std::string::npos);
    REQUIRE(result.unit.source.find(".set(_gdpp_dictionary_key_") != std::string::npos);
}

TEST_CASE("variadic initializers preserve default construction and pack new arguments") {
    const auto result = gdpp::Compiler{}.compile(
        "rest_constructor.gd", "class Payload:\n"
                               "    var total: int\n"
                               "    func _init(base: int = 1, ...values: Array) -> void:\n"
                               "        total = base + values.size()\n"
                               "func build() -> Payload:\n"
                               "    return Payload.new(4, 5, 6)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("GDPPNative_RestConstructor__Payload();") != std::string::npos);
    REQUIRE(result.unit.header.find(
                "virtual void _init(godot::Variant _gdpp_argument_base, godot::Array values)") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("public gdpp::runtime::AttachedScriptBehavior") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_call_rest_") != std::string::npos);
    REQUIRE(result.unit.source.find("InternalClassResource<GDPPNative_RestConstructor__Payload>") !=
            std::string::npos);
}

TEST_CASE("semantic analysis enforces rest parameter type contracts across targets") {
    gdpp::CompileOptions modern;
    modern.target_version = gdpp::GodotVersion::v4_6;
    const auto wrong_type = gdpp::Compiler{}.compile(
        "wrong_rest_type.gd", "func collect(...values: Dictionary):\n    pass\n", modern);
    const auto typed_array = gdpp::Compiler{}.compile(
        "typed_rest.gd", "func collect(...values: Array[int]):\n    pass\n", modern);
    const auto variant_array = gdpp::Compiler{}.compile(
        "variant_rest.gd", "func collect(...values: Array[Variant]):\n    pass\n", modern);

    gdpp::CompileOptions legacy;
    legacy.target_version = gdpp::GodotVersion::v4_4;
    const auto legacy_target = gdpp::Compiler{}.compile("legacy_rest.gd",
                                                        "func collect(required: int, ...values):\n"
                                                        "    return required + values.size()\n"
                                                        "func invoke():\n"
                                                        "    return collect(1, 2, 3)\n",
                                                        legacy);
    const auto static_initializer = gdpp::Compiler{}.compile(
        "static_init_rest.gd", "static func _static_init(...values):\n    pass\n", modern);

    const auto has_code = [](const auto& result, const std::string& code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    REQUIRE(!wrong_type.success);
    REQUIRE(!typed_array.success);
    REQUIRE(variant_array.success);
    REQUIRE(legacy_target.success);
    REQUIRE(!static_initializer.success);
    REQUIRE(has_code(wrong_type, "GDS4162"));
    REQUIRE(has_code(typed_array, "GDS4163"));
    REQUIRE(has_code(static_initializer, "GDS4124"));
}

TEST_CASE("semantic overrides preserve unbounded callable ranges") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_6;
    const auto invalid = gdpp::Compiler{}.compile("invalid_vararg_override.gd",
                                                  "class Base:\n"
                                                  "    func collect(value, ...extras):\n"
                                                  "        pass\n"
                                                  "class Derived extends Base:\n"
                                                  "    func collect(value):\n"
                                                  "        pass\n",
                                                  options);
    const auto valid = gdpp::Compiler{}.compile("valid_vararg_override.gd",
                                                "class Base:\n"
                                                "    func collect(value):\n"
                                                "        pass\n"
                                                "class Derived extends Base:\n"
                                                "    func collect(value, ...extras):\n"
                                                "        pass\n",
                                                options);

    REQUIRE(!invalid.success);
    REQUIRE(valid.success);
    REQUIRE(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4102"; }));
}

TEST_CASE("compiler emits recursive structural match tests with exact and rest cardinality") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "structured_match.gd", "extends RefCounted\n"
                               "func classify(value: Variant) -> int:\n"
                               "    match value:\n"
                               "        [1, {\"hp\": var hp, ..}, var tail]:\n"
                               "            return hp + tail\n"
                               "        {\"exact\": 1}:\n"
                               "            return 1\n"
                               "        _:\n"
                               "            return -1\n"
                               "func empty_match() -> void:\n"
                               "    match 0:\n"
                               "        pass\n"
                               "func named_lambda(value: int) -> int:\n"
                               "    var operation := func double_value(input): return input * 2\n"
                               "    return operation.call(value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("get_type() != godot::Variant::ARRAY") != std::string::npos);
    REQUIRE(result.unit.source.find("get_type() != godot::Variant::DICTIONARY") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".size() < 1") != std::string::npos);
    REQUIRE(result.unit.source.find(".size() != 1") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant _gdpp_match_bind_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_local_callable") != std::string::npos);
}

TEST_CASE("compiler rejects unsafe structural match binding and dictionary key forms") {
    const gdpp::Compiler compiler;
    const auto conflicting =
        compiler.compile("conflicting_match.gd", "func test(value):\n"
                                                 "    var captured = 1\n"
                                                 "    match value:\n"
                                                 "        [var captured, ..]: pass\n");
    const auto alternatives =
        compiler.compile("alternative_match.gd", "func test(value):\n"
                                                 "    match value:\n"
                                                 "        [var captured], 1: pass\n");
    const auto dynamic_key = compiler.compile("dynamic_key_match.gd", "func test(value):\n"
                                                                      "    var key = \"hp\"\n"
                                                                      "    match value:\n"
                                                                      "        {key}: pass\n");

    REQUIRE(!conflicting.success);
    REQUIRE(!alternatives.success);
    REQUIRE(!dynamic_key.success);
}

TEST_CASE("compiler generates debug-only typed assert control flow") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("res://assertions.gd",
                                         "extends Node\n"
                                         "func validate(value: int) -> int:\n"
                                         "    assert(value > 0, \"positive value required\")\n"
                                         "    return value\n"
                                         "func validate_void(value: bool) -> void:\n"
                                         "    assert(value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("#ifdef GDPP_SCRIPT_DEBUG_ENABLED") != std::string::npos);
    REQUIRE(result.unit.source.find("ERR_FAIL_V_EDMSG") != std::string::npos);
    REQUIRE(result.unit.source.find("ERR_FAIL_EDMSG") != std::string::npos);
    REQUIRE(result.unit.source.find("Assertion failed at res://assertions.gd:3") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("positive value required") != std::string::npos);
}

TEST_CASE("compiler emits debugger frames and exact breakpoint snapshots") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("debug_scope.gd", "extends Node\n"
                                           "var state := 3\n"
                                           "func inspect(value: int) -> void:\n"
                                           "    var label := \"ready\"\n"
                                           "    if value > 0:\n"
                                           "        var label := value\n"
                                           "        breakpoint\n"
                                           "static func inspect_static(value: int) -> void:\n"
                                           "    breakpoint\n"
                                           "func make_callback() -> Callable:\n"
                                           "    return func named_callback(value: int) -> void:\n"
                                           "        breakpoint\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::ScriptDebugFrame _gdpp_debug_frame(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::debug_breakpoint(godot::String(\"res://debug_scope.gd\"), "
                "godot::StringName(\"inspect\"), 7, this") != std::string::npos);
    REQUIRE(result.unit.source.find(".push_back(godot::String(\"value\"));") != std::string::npos);
    REQUIRE(result.unit.source.find(".push_back(godot::String(\"label\"));") != std::string::npos);
    REQUIRE(result.unit.source.find(".push_back(gdpp::runtime::to_variant(label));") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".push_back(godot::String(\"state\"));") != std::string::npos);
    REQUIRE(result.unit.source.find(".push_back(gdpp::runtime::to_variant(this->state));") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"inspect_static\"), 9, nullptr") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"named_callback\")") != std::string::npos);
    REQUIRE(result.unit.source.find("debugger bridge is emitted") == std::string::npos);
}

TEST_CASE("compiler preserves legal lexical shadowing under native warning policies") {
    const auto result =
        gdpp::Compiler{}.compile("shadowing.gd", "func inspect(label: String) -> int:\n"
                                                 "    if not label.is_empty():\n"
                                                 "        var label := 42\n"
                                                 "        return label\n"
                                                 "    return 0\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("#pragma clang diagnostic ignored \"-Wshadow\"") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("#pragma GCC diagnostic ignored \"-Wshadow\"") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("#pragma warning(disable : 4456 4457 4458 4459)") !=
            std::string::npos);
}

TEST_CASE("compiler emits nil fallthrough only for dynamic functions that need it") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("dynamic_returns.gd", "func side_effect(value):\n"
                                                               "    print(value)\n"
                                                               "func identity(value):\n"
                                                               "    return value\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("GDPPNative_DynamicReturns::side_effect") != std::string::npos);
    REQUIRE(result.unit.source.find("print(_gdpp_utility_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find("    return {};\n}") != std::string::npos);
    REQUIRE(result.unit.source.find("return value;\n    return {};") == std::string::npos);
}

TEST_CASE("compiler rejects invalid assert forms and types") {
    const gdpp::Compiler compiler;
    const auto truthy_condition =
        compiler.compile("condition.gd", "func test() -> void:\n    assert(1)\n");
    const auto invalid_message =
        compiler.compile("message.gd", "func test() -> void:\n    assert(true, 42)\n");
    const auto expression =
        compiler.compile("expression.gd", "func test() -> void:\n    var result := assert(true)\n");

    REQUIRE(truthy_condition.success);
    REQUIRE(!invalid_message.success);
    REQUIRE(!expression.success);
    REQUIRE(truthy_condition.unit.source.find(
                "(gdpp::runtime::to_variant(static_cast<int64_t>(1))).booleanize()") !=
            std::string::npos);
}

TEST_CASE("compiler applies warning directives and structured await to property setters") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("accessor_await.gd", "extends Node\n"
                                              "var score: int = 0:\n"
                                              "    set(value):\n"
                                              "        score = value\n"
                                              "        await get_tree().process_frame\n"
                                              "        score = value\n"
                                              "func ignored_unreachable() -> void:\n"
                                              "    return\n"
                                              "    @warning_ignore(\"unreachable_code\")\n"
                                              "    print(\"intentionally disabled\")\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("await_signal") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::CoroutineStatePtr{}") != std::string::npos);
}

TEST_CASE("compiler defers packed array typed storage failures to the runtime boundary") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("packed_array_argument.gd",
                                         "func consume(values: Array[String]) -> void:\n"
                                         "    pass\n"
                                         "func forward(values: PackedStringArray) -> void:\n"
                                         "    consume(values)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_typed_storage<godot::TypedArray<godot::String>>") !=
            std::string::npos);
}

TEST_CASE("compiler stores packed arrays with GDScript shared identity") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "packed_reference.gd", "extends RefCounted\n"
                               "var bytes: PackedByteArray = PackedByteArray([1])\n"
                               "func forward(values: PackedByteArray) -> PackedByteArray:\n"
                               "    var alias: PackedByteArray = values\n"
                               "    return alias\n");

    REQUIRE(result.success);
    const std::string storage = "gdpp::runtime::SharedPackedArray<godot::PackedByteArray>";
    REQUIRE(result.unit.header.find(storage + " bytes") != std::string::npos);
    REQUIRE(result.unit.header.find(storage + " forward(" + storage + " values)") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(storage + " alias = values") != std::string::npos);
}

TEST_CASE("compiler converts Variant storage at typed PackedArray call boundaries") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("packed_variant_calls.gd",
                         "extends RefCounted\n"
                         "class Sink:\n"
                         "    func bytes(value: PackedByteArray): value.append(1)\n"
                         "    func i32(value: PackedInt32Array): value.append(1)\n"
                         "    func i64(value: PackedInt64Array): value.append(1)\n"
                         "    func f32(value: PackedFloat32Array): value.append(1.0)\n"
                         "    func f64(value: PackedFloat64Array): value.append(1.0)\n"
                         "    func strings(value: PackedStringArray): value.append(\"x\")\n"
                         "    func vec2(value: PackedVector2Array): value.append(Vector2.ONE)\n"
                         "    func vec3(value: PackedVector3Array): value.append(Vector3.ONE)\n"
                         "    func colors(value: PackedColorArray): value.append(Color.WHITE)\n"
                         "    func vec4(value: PackedVector4Array): value.append(Vector4.ONE)\n"
                         "func forward(bytes: PackedByteArray, i32: PackedInt32Array,\n"
                         "        i64: PackedInt64Array, f32: PackedFloat32Array,\n"
                         "        f64: PackedFloat64Array, strings: PackedStringArray,\n"
                         "        vec2: PackedVector2Array, vec3: PackedVector3Array,\n"
                         "        colors: PackedColorArray, vec4: PackedVector4Array) -> void:\n"
                         "    var dynamic_bytes = bytes\n"
                         "    var dynamic_i32 = i32\n"
                         "    var dynamic_i64 = i64\n"
                         "    var dynamic_f32 = f32\n"
                         "    var dynamic_f64 = f64\n"
                         "    var dynamic_strings = strings\n"
                         "    var dynamic_vec2 = vec2\n"
                         "    var dynamic_vec3 = vec3\n"
                         "    var dynamic_colors = colors\n"
                         "    var dynamic_vec4 = vec4\n"
                         "    var sink := Sink.new()\n"
                         "    sink.bytes(dynamic_bytes)\n"
                         "    sink.i32(dynamic_i32)\n"
                         "    sink.i64(dynamic_i64)\n"
                         "    sink.f32(dynamic_f32)\n"
                         "    sink.f64(dynamic_f64)\n"
                         "    sink.strings(dynamic_strings)\n"
                         "    sink.vec2(dynamic_vec2)\n"
                         "    sink.vec3(dynamic_vec3)\n"
                         "    sink.colors(dynamic_colors)\n"
                         "    sink.vec4(dynamic_vec4)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "godot::Variant dynamic_bytes = gdpp::runtime::to_variant(bytes)") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant result_value = gdpp::runtime::to_variant(") !=
            std::string::npos);
    for (const std::string_view type :
         {"PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array",
          "PackedFloat64Array", "PackedStringArray", "PackedVector2Array", "PackedVector3Array",
          "PackedColorArray", "PackedVector4Array"}) {
        REQUIRE(result.unit.source.find(
                    "gdpp::runtime::strict_packed_array_storage<godot::" + std::string{type} +
                    ">(gdpp::runtime::to_variant(_gdpp_call_argument_") != std::string::npos);
    }
}

TEST_CASE("compiler applies internal call contracts to every native storage family") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "variant_internal_calls.gd",
        "extends RefCounted\n"
        "class Sink:\n"
        "    func array(value: Array[String]) -> int: return value.size()\n"
        "    func dictionary(value: Dictionary[String, int]) -> int: return value.size()\n"
        "    func object(value: Node) -> String: return value.name\n"
        "    func string(value: String) -> int: return value.length()\n"
        "    func vector(value: Vector3) -> float: return value.x\n"
        "func forward(array_value: Variant, dictionary_value: Variant,\n"
        "        object_value: Variant, string_value: Variant, vector_value: Variant) -> void:\n"
        "    var sink := Sink.new()\n"
        "    sink.array(array_value)\n"
        "    sink.dictionary(dictionary_value)\n"
        "    sink.object(object_value)\n"
        "    sink.string(string_value)\n"
        "    sink.vector(vector_value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <godot_cpp/classes/node.hpp>") != std::string::npos);
    REQUIRE(result.unit.header.find("#include <godot_cpp/variant/vector3.hpp>") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_typed_storage<godot::TypedArray<godot::String>>("
                "gdpp::runtime::to_variant(_gdpp_call_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_typed_storage<godot::TypedDictionary<godot::String, "
                "int64_t>>(gdpp::runtime::to_variant(_gdpp_call_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_native_object_value_storage<godot::Node>("
                "gdpp::runtime::to_variant(_gdpp_call_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<godot::String>("
                                    "gdpp::runtime::to_variant(_gdpp_call_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<godot::Vector3>("
                                    "gdpp::runtime::to_variant(_gdpp_call_argument_") !=
            std::string::npos);
}

TEST_CASE("semantic analysis validates typed container arguments eagerly") {
    const gdpp::Compiler compiler;
    const auto legal = compiler.compile("legal_containers.gd",
                                        "var arrays: Array[Array] = []\n"
                                        "var dictionaries: Dictionary[String, Dictionary] = {}\n");
    const auto unknown =
        compiler.compile("unknown_container.gd", "var values: Array[MissingType] = []\n");
    const auto nested = compiler.compile("nested_container.gd",
                                         "var values: Dictionary[String, Array[int]] = {}\n");
    const auto void_element =
        compiler.compile("void_container.gd", "var values: Array[void] = []\n");
    const auto incompatible = compiler.compile(
        "incompatible_containers.gd", "func convert(values: Array[int]) -> void:\n"
                                      "    var floats: Array[float] = values\n"
                                      "    var labels: Dictionary[String, int] = {}\n"
                                      "    var other: Dictionary[String, float] = labels\n");

    REQUIRE(legal.success);
    REQUIRE(!unknown.success);
    REQUIRE(!nested.success);
    REQUIRE(!void_element.success);
    REQUIRE(!incompatible.success);
    REQUIRE(std::any_of(unknown.diagnostics.begin(), unknown.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4059"; }));
    REQUIRE(std::any_of(nested.diagnostics.begin(), nested.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4138"; }));
    REQUIRE(std::any_of(void_element.diagnostics.begin(), void_element.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4139"; }));
}

TEST_CASE("nullability matches Godot objects Variant and non-null value types") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile(
        "nullable_objects.gd", "extends Node\n"
                               "const EMPTY = null\n"
                               "var dynamic = null\n"
                               "var base_object: Object = null\n"
                               "var child: Node = null\n"
                               "func values() -> Array:\n"
                               "    var local_variant: Variant = null\n"
                               "    var local_node: Node = null\n"
                               "    return [EMPTY, dynamic, base_object, child, local_variant, "
                               "local_node]\n");
    const auto invalid = compiler.compile("nonnullable_values.gd",
                                          "var array: Array = null\n"
                                          "var typed_array: Array[int] = null\n"
                                          "var dictionary: Dictionary = null\n"
                                          "var typed_dictionary: Dictionary[String, int] = null\n"
                                          "var text: String = null\n"
                                          "var name: StringName = null\n"
                                          "var path: NodePath = null\n"
                                          "var callable: Callable = null\n"
                                          "var signal_value: Signal = null\n"
                                          "var vector: Vector2 = null\n"
                                          "var handle: RID = null\n");
    const auto inferred =
        compiler.compile("null_inference.gd", "var field := null\n"
                                              "func inspect(parameter := null) -> void:\n"
                                              "    var local := null\n");

    REQUIRE(valid.success);
    REQUIRE(!invalid.success);
    REQUIRE_EQ(std::count_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                             [](const auto& diagnostic) {
                                 return diagnostic.code == "GDS4002" &&
                                        diagnostic.message.find("cannot assign null") !=
                                            std::string::npos;
                             }),
               std::ptrdiff_t{11});
    REQUIRE(!inferred.success);
    REQUIRE_EQ(std::count_if(inferred.diagnostics.begin(), inferred.diagnostics.end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS4154"; }),
               std::ptrdiff_t{3});
}

TEST_CASE("compiler preserves typed container metadata in the native ABI") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "typed_containers.gd", "extends Node\n"
                               "enum Mode { IDLE, RUN }\n"
                               "signal changed(values: Array[int])\n"
                               "var integers: Array[int] = [1, 2]\n"
                               "var weights: Dictionary[String, float] = {\"left\": 1.0}\n"
                               "var nodes: Array[Node] = []\n"
                               "var modes: Array[Mode] = [Mode.IDLE]\n"
                               "func summarize(values: Array[int]) -> Dictionary[String, int]:\n"
                               "    var copy: Array[int] = values\n"
                               "    return {\"first\": copy[0]}\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <godot_cpp/variant/typed_array.hpp>") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("#include <godot_cpp/variant/typed_dictionary.hpp>") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("godot::TypedArray<int64_t> integers") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::TypedDictionary<godot::String, double> weights") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("struct ContainerObjectTag_Node") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::StringName(\"Node\")") != std::string::npos);
    REQUIRE(result.unit.header.find(
                "godot::TypedArray<typed_containers_gdpp_detail::ContainerObjectTag_Node> nodes") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("godot::TypedArray<int64_t> modes") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::TypedDictionary<godot::String, int64_t> summarize(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "-> godot::TypedArray<int64_t> { godot::TypedArray<int64_t> _gdpp_array_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("-> godot::TypedDictionary<godot::String, double> { "
                                    "godot::TypedDictionary<godot::String, double> "
                                    "_gdpp_dictionary_") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::TypedArray<int64_t>(([]() -> godot::Array") ==
            std::string::npos);
    REQUIRE(
        result.unit.source.find("godot::TypedArray<int64_t>(([]() -> godot::TypedArray<int64_t>") ==
        std::string::npos);
    REQUIRE(result.unit.source.find(
                "godot::TypedArray<int64_t>(([&]() -> godot::TypedArray<int64_t>") ==
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::GetTypeInfo<godot::TypedArray<int64_t>>") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_USAGE_SCRIPT_VARIABLE") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::MethodInfo(\"changed\", ([] { auto info = "
                                    "godot::GetTypeInfo<godot::TypedArray<int64_t>>") !=
            std::string::npos);
}

TEST_CASE("Variant-only container annotations use Godot's untyped runtime ABI") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("variant_containers.gd",
                                         "var values: Array[Variant] = []\n"
                                         "var mappings: Dictionary[Variant, Variant] = {}\n"
                                         "var constrained: Dictionary[Variant, int] = {}\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("godot::Array values") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::Dictionary mappings") != std::string::npos);
    REQUIRE(
        result.unit.header.find("godot::TypedDictionary<godot::Variant, int64_t> constrained") !=
        std::string::npos);
    REQUIRE(result.unit.header.find("godot::TypedArray<godot::Variant>") == std::string::npos);
}

TEST_CASE("typed container literals are validated in every contextual position") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile("contextual_containers.gd",
                                        "func consume(values: Array[int]) -> Array[int]:\n"
                                        "    return values\n"
                                        "func run() -> Array[int]:\n"
                                        "    var local: Array[int] = [1, 2]\n"
                                        "    local = [3, 4]\n"
                                        "    return consume([5, 6])\n");
    const auto invalid = compiler.compile("invalid_contextual_containers.gd",
                                          "var values: Array[int] = [\"bad\"]\n"
                                          "var mappings: Dictionary[String, int] = {1: \"bad\"}\n"
                                          "func consume(items: Array[int]) -> void:\n"
                                          "    pass\n"
                                          "func run() -> Array[int]:\n"
                                          "    consume([\"bad\"])\n"
                                          "    return [\"bad\"]\n");

    REQUIRE(valid.success);
    REQUIRE(!invalid.success);
    REQUIRE(std::count_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                          [](const auto& diagnostic) { return diagnostic.code == "GDS4002"; }) >=
            4);
}

TEST_CASE("typed container mutation APIs enforce element key and value contracts") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile(
        "valid_container_mutation.gd",
        "func mutate(values: Array[int], labels: Dictionary[String, int]) -> void:\n"
        "    values.append(1)\n"
        "    values.insert(0, 2)\n"
        "    labels[\"left\"] = 3\n"
        "    labels.set(\"right\", 4)\n");
    const auto invalid = compiler.compile("invalid_container_mutation.gd",
                                          "func mutate(values: Array[int], floats: Array[float], "
                                          "labels: Dictionary[String, int]) -> void:\n"
                                          "    values.append(\"bad\")\n"
                                          "    values.insert(0, \"bad\")\n"
                                          "    values.append_array(floats)\n"
                                          "    labels[1] = 3\n"
                                          "    labels.set(\"right\", \"bad\")\n");

    REQUIRE(valid.success);
    REQUIRE(!invalid.success);
    REQUIRE(std::count_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                          [](const auto& diagnostic) { return diagnostic.code == "GDS4002"; }) >=
            5);
}

TEST_CASE("compiler generates serializable Godot export properties and inspector hints") {
    const std::string source =
        "extends Node\n"
        "class_name ExportedSettings\n"
        "@export var title: String = \"Player\"\n"
        "@export_range(-10.0, 100.0, 0.5, \"or_greater\") var speed: float = 4.0\n"
        "@export_enum(\"Idle\", \"Run:4\") var state: int = 0\n"
        "@export_flags(\"Fire\", \"Ice\") var abilities: int = 0\n"
        "@export_file(\"*.json\") var config_path: String = \"\"\n"
        "@export_multiline var biography: String = \"\"\n"
        "@export_color_no_alpha var tint: Color = Color.html(\"ff8800\")\n"
        "@export_node_path(\"Node2D\") var target: NodePath\n"
        "@export var icon: Texture2D\n"
        "@export_category(\"Advanced\")\n"
        "@export var retries: int = 3\n"
        "@export var inferred_count = 5\n"
        "@export var inferred_tags = [\"commercial\"]\n";
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("exported_settings.gd", source);

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("_gdpp_get_title()") != std::string::npos);
    REQUIRE(result.unit.source.find("ADD_PROPERTY") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_HINT_RANGE") != std::string::npos);
    REQUIRE(result.unit.source.find("-10.0,100.0,0.5,or_greater") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_HINT_ENUM") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_HINT_FLAGS") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_HINT_NODE_PATH_VALID_TYPES") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_HINT_RESOURCE_TYPE") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::Ref<godot::Texture2D> icon{}") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_USAGE_CATEGORY") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::Variant inferred_count{}") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "inferred_count = gdpp::runtime::to_variant(static_cast<int64_t>(5))") !=
            std::string::npos);
    REQUIRE(
        result.unit.source.find("godot::PropertyInfo(godot::Variant::INT, \"inferred_count\"") !=
        std::string::npos);
    REQUIRE(
        result.unit.source.find("godot::PropertyInfo(godot::Variant::ARRAY, \"inferred_tags\"") !=
        std::string::npos);
}

TEST_CASE("compiler folds export custom constants before native property generation") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("custom_export.gd", "extends Resource\n"
                                             "@export_custom(PROPERTY_HINT_RANGE, \"0,10\", "
                                             "PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY)\n"
                                             "var amount: int = 1\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("static_cast<godot::PropertyHint>(1)") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<uint32_t>(268435462)") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_USAGE_SCRIPT_VARIABLE") != std::string::npos);
    REQUIRE(result.unit.source.find("PROPERTY_HINT_RANGE") == std::string::npos);
    REQUIRE(result.unit.source.find("PROPERTY_USAGE_READ_ONLY") == std::string::npos);
}

TEST_CASE("compiler rejects unresolved export custom constants before codegen") {
    const gdpp::Compiler compiler;
    const auto unknown =
        compiler.compile("unknown_custom_export.gd",
                         "@export_custom(MISSING_PROPERTY_HINT, \"\", MISSING_PROPERTY_USAGE)\n"
                         "var amount: int = 1\n");
    const auto runtime_expression =
        compiler.compile("runtime_custom_export.gd", "func make_hint() -> int:\n"
                                                     "    return 0\n"
                                                     "@export_custom(make_hint(), \"\")\n"
                                                     "var amount: int = 1\n");

    REQUIRE(!unknown.success);
    REQUIRE(!runtime_expression.success);
    REQUIRE(unknown.unit.source.empty());
    REQUIRE(runtime_expression.unit.source.empty());
    REQUIRE(std::count_if(unknown.diagnostics.begin(), unknown.diagnostics.end(),
                          [](const auto& diagnostic) { return diagnostic.code == "GDS4122"; }) ==
            2);
    REQUIRE(std::any_of(runtime_expression.diagnostics.begin(),
                        runtime_expression.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4145"; }));
}

TEST_CASE("compiler rejects unknown types in every callable and storage position") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "unknown_type_positions.gd",
        "signal changed(value: MissingSignalType)\n"
        "var field: MissingFieldType\n"
        "func inspect(parameter: MissingParameterType) -> MissingReturnType:\n"
        "    var local: MissingLocalType\n"
        "    for item: MissingLoopType in []:\n"
        "        pass\n"
        "    var callback := func(value: MissingLambdaType) -> MissingLambdaReturnType:\n"
        "        return value\n"
        "    return null\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.header.empty());
    REQUIRE(result.unit.source.empty());
    for (const auto* name :
         {"MissingSignalType", "MissingFieldType", "MissingParameterType", "MissingReturnType",
          "MissingLocalType", "MissingLoopType", "MissingLambdaType", "MissingLambdaReturnType"}) {
        REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                            [name](const auto& diagnostic) {
                                return diagnostic.code == "GDS4059" &&
                                       diagnostic.message.find(name) != std::string::npos &&
                                       diagnostic.span.end.offset > diagnostic.span.begin.offset;
                            }));
    }
}

TEST_CASE("compiler preserves annotation-implied exports and Godot object conversions") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "godot_conversions.gd",
        "extends Node2D\n"
        "@export_color_no_alpha var untyped_color\n"
        "var packed_color: Color = Color(0, 0)\n"
        "var texture: Texture2D\n"
        "func submit(canvas: RID) -> void:\n"
        "    RenderingServer.canvas_item_add_texture_rect_region(canvas, Rect2(), texture, "
        "Rect2())\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("godot::Variant untyped_color{}") != std::string::npos);
    REQUIRE(
        result.unit.source.find("godot::PropertyInfo(godot::Variant::COLOR, \"untyped_color\"") !=
        std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<godot::Color>(gdpp::runtime::to_variant(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<godot::RID>(gdpp::runtime::to_variant(") !=
            std::string::npos);
}

TEST_CASE("compiler generates registered internal classes and native lambda Callables") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("modern.gd", "extends Node\n"
                                                      "class Payload:\n"
                                                      "    var value: int\n"
                                                      "    func _init(initial: int) -> void:\n"
                                                      "        value = initial\n"
                                                      "signal changed(value: int)\n"
                                                      "func attach() -> int:\n"
                                                      "    var captured := 2\n"
                                                      "    changed.connect(\n"
                                                      "        func(value: int) -> void:\n"
                                                      "            print(value + captured))\n"
                                                      "    var payload := Payload.new(3)\n"
                                                      "    return payload.value\n");

    REQUIRE(result.success);
    REQUIRE_EQ(result.unit.inner_class_names.size(), std::size_t{1});
    REQUIRE_EQ(result.unit.inner_class_names.front(), std::string{"GDPPNative_Modern__Payload"});
    REQUIRE(result.unit.header.find(
                "GDCLASS(GDPPNative_Modern__Payload, gdpp::runtime::AttachedScriptBehavior)") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_local_callable<1, 1, false>(this") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("InternalClassResource<GDPPNative_Modern__Payload>") !=
            std::string::npos);
}

TEST_CASE("lambda call frames copy creation snapshots per invocation") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("lambda_snapshots.gd", "func make() -> Callable:\n"
                                                                "    var captured := 1\n"
                                                                "    return func() -> int:\n"
                                                                "        captured += 1\n"
                                                                "        return captured\n");

    REQUIRE(result.success);
    const auto callable = result.unit.source.find("gdpp::runtime::make_local_callable<");
    const auto creation_snapshot =
        result.unit.source.find(") mutable -> godot::Variant {", callable);
    const auto invocation_snapshot =
        result.unit.source.find("return [=]() mutable -> godot::Variant {", creation_snapshot);
    const auto invocation = result.unit.source.find("captured", invocation_snapshot);
    const auto close = result.unit.source.find("}();\n})", invocation);
    REQUIRE(callable != std::string::npos);
    REQUIRE(creation_snapshot > callable);
    REQUIRE(invocation_snapshot > creation_snapshot);
    REQUIRE(invocation > invocation_snapshot);
    REQUIRE(close > invocation);
}

TEST_CASE("async loop lambdas snapshot symbol-identified cells at creation") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("async_lambda_snapshots.gd",
                         "extends Node\n"
                         "signal resume\n"
                         "func run() -> void:\n"
                         "    var captured := 1\n"
                         "    var iteration := 0\n"
                         "    while iteration < 1:\n"
                         "        await resume\n"
                         "        var callback := func() -> int:\n"
                         "            captured += 1\n"
                         "            return captured\n"
                         "        captured = 10\n"
                         "        print(callback.call(), captured, callback.call(), captured)\n"
                         "        iteration += 1\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;
    const auto cell = source.find("std::make_shared<int64_t>(captured)");
    const auto snapshot = source.find("[[maybe_unused]] auto _gdpp_lambda_capture_", cell);
    const auto snapshot_source = source.find(" = (*_gdpp_async_cell_", snapshot);
    const auto callable = source.find("gdpp::runtime::make_local_callable<", snapshot_source);
    const auto invocation = source.find("return [=]() mutable -> godot::Variant {", callable);
    const auto captured_write = source.find("_gdpp_lambda_capture_", invocation);
    REQUIRE(cell != std::string::npos);
    REQUIRE(snapshot > cell);
    REQUIRE(snapshot_source > snapshot);
    REQUIRE(callable > snapshot_source);
    REQUIRE(invocation > callable);
    REQUIRE(captured_write > invocation);
}

TEST_CASE("typed containers preserve internal class runtime identity without include cycles") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("inner_typed_containers.gd",
                                         "class Payload:\n"
                                         "    var value: int = 1\n"
                                         "var payloads: Array[Payload] = []\n"
                                         "var by_name: Dictionary[String, Payload] = {}\n"
                                         "func replace(values: Array[Payload]) -> Array[Payload]:\n"
                                         "    payloads = values\n"
                                         "    return payloads\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find(
                "struct ContainerObjectTag_GDPPNative_InnerTypedContainers__Payload") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("godot::StringName(\"RefCounted\")") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_attached_script_path = "
                                    "\"res://inner_typed_containers.gd::Payload\"") !=
            std::string::npos);
    REQUIRE(result.unit.header.find(
                "gdpp::runtime::ScriptTypedArray<inner_typed_containers_gdpp_detail::"
                "ContainerObjectTag_GDPPNative_InnerTypedContainers__Payload> payloads") !=
            std::string::npos);
    REQUIRE(result.unit.header.find(
                "gdpp::runtime::ScriptTypedDictionary<godot::String, "
                "inner_typed_containers_gdpp_detail::"
                "ContainerObjectTag_GDPPNative_InnerTypedContainers__Payload> by_name") !=
            std::string::npos);
    REQUIRE(result.unit.header.find(
                "godot::StringName(\"GDPPNative_InnerTypedContainers__Payload\")") ==
            std::string::npos);
}

TEST_CASE("compiler topologically lowers internal class inheritance and super calls") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("inner_inheritance.gd", "class Derived extends Base:\n"
                                                 "    func value() -> int:\n"
                                                 "        return super() + 1\n"
                                                 "class Base:\n"
                                                 "    var number: int = 4\n"
                                                 "    func value() -> int:\n"
                                                 "        return number\n"
                                                 "func read() -> int:\n"
                                                 "    return Derived.new().value()\n");

    REQUIRE(result.success);
    REQUIRE_EQ(result.unit.inner_class_names.size(), std::size_t{2});
    REQUIRE_EQ(result.unit.inner_class_names.front(),
               std::string{"GDPPNative_InnerInheritance__Base"});
    REQUIRE_EQ(result.unit.inner_class_names.back(),
               std::string{"GDPPNative_InnerInheritance__Derived"});
    REQUIRE(result.unit.header.find("class GDPPNative_InnerInheritance__Derived : public "
                                    "GDPPNative_InnerInheritance__Base") != std::string::npos);
    REQUIRE(result.unit.header.find("virtual int64_t value() override") != std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_InnerInheritance__Base::value()") !=
            std::string::npos);
}

TEST_CASE("static lambdas support defaults without binding an instance owner") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("factory.gd", "static func make() -> Callable:\n"
                                                       "    return func(value: int = 1) -> int:\n"
                                                       "        return value + 1\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_local_callable<0, 1, false>(nullptr") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".size() > 0 ?") != std::string::npos);
    const auto callable = result.unit.source.find("mutable -> godot::Variant {");
    const auto scope = result.unit.source.find("gdpp::runtime::ScriptFunctionScope", callable);
    const auto parameter = result.unit.source.find("int64_t value =", callable);
    const auto parameter_check = result.unit.source.find("script_function_failed()", parameter);
    REQUIRE(callable < scope);
    REQUIRE(scope < parameter);
    REQUIRE(parameter < parameter_check);
}

TEST_CASE("void lambdas return a Variant when argument conversion fails") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("void_lambda.gd", "func make() -> Callable:\n"
                                           "    return func(value: Variant) -> void:\n"
                                           "        var values: Array[int] = value\n"
                                           "        print(values.size())\n");

    REQUIRE(result.success);
    const auto callable = result.unit.source.find("mutable -> godot::Variant {");
    const auto failure = result.unit.source.find("if (script_function_failed())", callable);
    REQUIRE(callable != std::string::npos);
    REQUIRE(failure != std::string::npos);
    REQUIRE(result.unit.source.find("return godot::Variant{};", failure) != std::string::npos);
}

TEST_CASE("typed lambdas return their declared default value after runtime failure") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("typed_lambda_failure.gd", "func make() -> Callable:\n"
                                                    "    return func(values: Array) -> int:\n"
                                                    "        return values[9]\n");

    REQUIRE(result.success);
    const auto callable = result.unit.source.find("mutable -> godot::Variant {");
    const auto failure = result.unit.source.find("if (script_function_failed())", callable);
    REQUIRE(callable != std::string::npos);
    REQUIRE(failure != std::string::npos);
    REQUIRE(result.unit.source.find("return gdpp::runtime::to_variant(int64_t{});", failure) !=
            std::string::npos);
}

TEST_CASE("variadic lambdas receive excess arguments as a Godot Array") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "rest_lambda.gd",
        "static func make() -> Callable:\n"
        "    return func(required: int, optional: int = 2, ...values: Array) -> int:\n"
        "        return required + optional + values.size()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_local_callable<1, 2, true>(nullptr") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Array values;") != std::string::npos);
    REQUIRE(result.unit.source.find("values.resize(") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_rest_index = 2") != std::string::npos);
}

TEST_CASE("compiler preserves instance defaults and explicit null through native bindings") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("defaults.gd", "extends Node\n"
                                        "var fallback = [1]\n"
                                        "func choose(pool = fallback, focus: Control = null):\n"
                                        "    return pool if focus == null else focus\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find(
                "godot::Variant _gdpp_argument_pool = gdpp::runtime::default_argument()") !=
            std::string::npos);
    REQUIRE(result.unit.header.find(
                "godot::Variant _gdpp_argument_focus = gdpp::runtime::default_argument()") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::is_default_argument(_gdpp_argument_pool) ? "
                                    "gdpp::runtime::to_variant(fallback)") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "method.default_arguments.push_back(gdpp::runtime::default_argument())") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("DEFVAL(godot::Variant())") == std::string::npos);
    REQUIRE(result.unit.source.find(
                "? gdpp::runtime::ObjectStorage<godot::Control>{} : "
                "gdpp::runtime::strict_native_object_value_storage<godot::Control>") !=
            std::string::npos);
}

TEST_CASE("compiler lowers awaited function and lambda defaults through one continuation chain") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "await_defaults.gd", "extends Node\n"
                             "signal selected(value)\n"
                             "func mark(value: int) -> int:\n"
                             "    return value\n"
                             "func ordered(first: int = mark(1), second: int = await selected, "
                             "third: int = mark(first + second)) -> void:\n"
                             "    print(first, second, third)\n"
                             "func default_only(value: int = await selected) -> int:\n"
                             "    return value\n"
                             "func await_default_only() -> int:\n"
                             "    return await default_only()\n"
                             "func make_callback() -> Callable:\n"
                             "    return func(value: int = await selected) -> void:\n"
                             "        print(value)\n");

    REQUIRE(result.success);
    const auto first = result.unit.source.find("int64_t first =");
    const auto after = result.unit.source.find("auto _gdpp_after_parameter_", first);
    const auto third = result.unit.source.find("int64_t third =", after);
    const auto commit = result.unit.source.find("auto _gdpp_commit_parameter_", third);
    REQUIRE(first < after);
    REQUIRE(after < third);
    REQUIRE(third < commit);
    REQUIRE(result.unit.source.find("!gdpp::runtime::is_default_argument(_gdpp_argument_second)") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".size() > 0", commit) != std::string::npos);
    REQUIRE(result.unit.source.find("std::make_shared<int64_t>()") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::Variant default_only(") != std::string::npos);
    REQUIRE(result.unit.header.find("godot::Variant await_default_only()") != std::string::npos);
    REQUIRE(result.unit.source.find("unlowered await expression") == std::string::npos);
    REQUIRE(result.unit.source.find("unsupported structured await") == std::string::npos);
}

TEST_CASE("compiler gives dynamic conditional branches an unambiguous native common type") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("conditional.gd", "func ratio(value, enabled: bool):\n"
                                           "    return value if enabled else 0.0\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("const bool _gdpp_conditional_condition_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::to_variant(value)") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::to_variant(0.0)") != std::string::npos);
}

TEST_CASE("compiler resolves forward constants before field initializers") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("forward_constant.gd", "var speed = MIN_SPEED\n"
                                                                "const MIN_SPEED = TAU * 0.25\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("static const double& MIN_SPEED();") != std::string::npos);
    REQUIRE(result.unit.source.find("speed = gdpp::runtime::to_variant(MIN_SPEED())") !=
            std::string::npos);
}

TEST_CASE("compiler defers scalar Godot constants until after extension initialization") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "constant_utility.gd", "extends Node2D\nconst MAX_ANGLE: float = deg_to_rad(10.0)\n"
                               "func angle() -> float:\n    return MAX_ANGLE\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("static const double& MAX_ANGLE();") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_constant_MAX_ANGLE_storage()") != std::string::npos);
    REQUIRE(result.unit.source.find("::MAX_ANGLE =") == std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_return_value_") != std::string::npos);
    REQUIRE(result.unit.source.find(" = MAX_ANGLE();") != std::string::npos);
}

TEST_CASE("compiler completes coroutine state inside structured await continuations") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "await_return.gd", "extends Node\n"
                           "signal resumed\n"
                           "var cancelled = false\n"
                           "func run():\n"
                           "    await resumed\n"
                           "    if cancelled:\n"
                           "        return\n"
                           "    var active = [1].filter(func(value): return value > 0)\n"
                           "    print(\"done\")\n");

    REQUIRE(result.success);
    const auto cancelled_branch = result.unit.source.find("const bool _gdpp_if_condition_");
    REQUIRE(cancelled_branch != std::string::npos);
    REQUIRE(result.unit.source.find("(gdpp::runtime::to_variant(cancelled)).booleanize()",
                                    cancelled_branch) != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::complete_coroutine(", cancelled_branch) !=
            std::string::npos);
    REQUIRE(result.unit.source.find("return;", cancelled_branch) != std::string::npos);
    const auto lambda = result.unit.source.find("mutable -> godot::Variant {");
    const auto lambda_scope = result.unit.source.find("gdpp::runtime::ScriptFunctionScope", lambda);
    const auto lambda_parameter =
        result.unit.source.find("[[maybe_unused]] godot::Variant value =", lambda_scope);
    REQUIRE(lambda < lambda_scope);
    REQUIRE(lambda_scope < lambda_parameter);
    REQUIRE(result.unit.source.find("const auto _gdpp_return_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("return gdpp::runtime::to_variant(_gdpp_return_value_") !=
            std::string::npos);
}

TEST_CASE("compiler routes native script identity through the compatibility runtime") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("identity.gd", "extends Node\n"
                                                        "func identity():\n"
                                                        "    return get_script()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::script_identity(this)") != std::string::npos);
}

TEST_CASE("compiler selects the Godot Node template for implicit get_node calls") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("node_lookup.gd", "extends Node\n"
                                           "func find_child():\n"
                                           "    return get_node(\"Child\")\n"
                                           "func find_optional():\n"
                                           "    return get_node_or_null(\"Child\")\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("get_node<godot::Node>(") != std::string::npos);
    REQUIRE(result.unit.source.find("get_node_or_null(") != std::string::npos);
    REQUIRE(result.unit.source.find("get_node_or_null<") == std::string::npos);
}

TEST_CASE("compiler lowers inherited Godot signals on self and typed objects") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("button_signals.gd", "extends Button\n"
                                              "func wire(other: Button) -> void:\n"
                                              "    pressed.connect(on_pressed)\n"
                                              "    resized.connect(on_resized)\n"
                                              "    other.pressed.connect(on_pressed)\n"
                                              "func on_pressed() -> void:\n"
                                              "    pass\n"
                                              "func on_resized() -> void:\n"
                                              "    pass\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Signal(this, godot::StringName(\"pressed\"))") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Signal(this, godot::StringName(\"resized\"))") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Signal(other, godot::StringName(\"pressed\"))") !=
            std::string::npos);
}

TEST_CASE("compiler emits local signals through their owner without temporary Signal objects") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("signal_emit.gd", "extends Node\n"
                                                           "signal pulse(value: int)\n"
                                                           "func fire(value: int) -> void:\n"
                                                           "    pulse.emit(value + 1)\n"
                                                           "    self.pulse.emit(value + 2)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("static const godot::Variant _gdpp_signal_name_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::emit_local_signal_at(") != std::string::npos);
    REQUIRE(result.unit.source.find(", this, _gdpp_signal_name_") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Signal(this, godot::StringName(\"pulse\")).emit") ==
            std::string::npos);
}

TEST_CASE("compiler flattens nested internal classes with inherited members") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("nested.gd", "class Parent:\n"
                                      "    const parent_value := 1\n"
                                      "    signal changed\n"
                                      "    class Nested:\n"
                                      "        const nested_value := 2\n"
                                      "class Child extends Parent:\n"
                                      "    func read() -> int:\n"
                                      "        print(self.changed.get_name())\n"
                                      "        return parent_value + Parent.Nested.nested_value\n"
                                      "func create() -> int:\n"
                                      "    return Child.new().read()\n");

    REQUIRE(result.success);
    REQUIRE(std::find(result.unit.inner_class_names.begin(), result.unit.inner_class_names.end(),
                      "GDPPNative_Nested__Parent__Nested") != result.unit.inner_class_names.end());
    REQUIRE(result.unit.header.find(
                "class GDPPNative_Nested__Child : public GDPPNative_Nested__Parent") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_Nested__Parent::parent_value") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_Nested__Parent__Nested::nested_value") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Signal(owner(), godot::StringName(\"changed\"))") !=
            std::string::npos);
}

TEST_CASE("compiler preserves explicit Variant exports and rejects untyped exports") {
    const gdpp::Compiler compiler;
    const auto wrong_type =
        compiler.compile("wrong.gd", "@export_range(0, 10) var label: String = \"bad\"\n");
    const auto dynamic = compiler.compile("dynamic.gd", "@export var value: Variant\n");
    const auto inferred = compiler.compile("inferred.gd", "@export var value: Variant = 12\n");
    const auto untyped = compiler.compile("untyped.gd", "@export var value\n");
    const auto unsupported = compiler.compile("rpc.gd", "@rpc var value: int = 1\n");

    REQUIRE(!wrong_type.success);
    REQUIRE(dynamic.success);
    REQUIRE(dynamic.unit.source.find("godot::Variant::NIL, \"value\"") != std::string::npos);
    REQUIRE(dynamic.unit.source.find("godot::PROPERTY_USAGE_NIL_IS_VARIANT") != std::string::npos);
    REQUIRE(inferred.success);
    REQUIRE(inferred.unit.source.find("godot::Variant::INT, \"value\"") != std::string::npos);
    REQUIRE(!untyped.success);
    REQUIRE(!unsupported.success);
    REQUIRE(wrong_type.unit.source.empty());
}

TEST_CASE("compiler emits complete native RPC configuration for Node classes") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "rpc_node.gd", "extends Node\n"
                       "const RPC_MODE = \"any_peer\"\n"
                       "const RPC_CHANNEL = 3\n"
                       "@rpc\n"
                       "func defaults() -> void:\n"
                       "    pass\n"
                       "@rpc(RPC_MODE, \"call_local\", \"reliable\", RPC_CHANNEL)\n"
                       "static func synchronize() -> void:\n"
                       "    pass\n"
                       "class Child extends Node:\n"
                       "    @rpc(\"unreliable_ordered\")\n"
                       "    func update_remote() -> void:\n"
                       "        pass\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("GDPPNative_RpcNode();") != std::string::npos);
    REQUIRE(result.unit.source.find("#include <godot_cpp/classes/multiplayer_api.hpp>") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("RPC_MODE_AUTHORITY") != std::string::npos);
    REQUIRE(result.unit.source.find("RPC_MODE_ANY_PEER") != std::string::npos);
    REQUIRE(result.unit.source.find("TRANSFER_MODE_UNRELIABLE") != std::string::npos);
    REQUIRE(result.unit.source.find("TRANSFER_MODE_UNRELIABLE_ORDERED") != std::string::npos);
    REQUIRE(result.unit.source.find("TRANSFER_MODE_RELIABLE") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp_rpc_config[\"call_local\"] = true") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp_rpc_config[\"channel\"] = int64_t{3}") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("rpc_config(godot::StringName(\"synchronize\")") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("rpc[godot::StringName(\"update_remote\")] = config") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("descriptor.rpc_config = rpc") != std::string::npos);
}

TEST_CASE("compiler preserves official RPC configuration field presence") {
    const gdpp::Compiler compiler;
    const auto defaults = compiler.compile("rpc_defaults.gd", "extends Node\n"
                                                              "@rpc\n"
                                                              "func ping() -> void:\n"
                                                              "    pass\n");
    const auto explicit_defaults = compiler.compile(
        "rpc_explicit_defaults.gd", "extends Node\n"
                                    "@rpc(\"authority\", \"call_remote\", \"unreliable\", 0)\n"
                                    "func ping() -> void:\n"
                                    "    pass\n");

    REQUIRE(defaults.success);
    REQUIRE(defaults.unit.source.find("RPC_MODE_AUTHORITY") != std::string::npos);
    REQUIRE(defaults.unit.source.find("[\"transfer_mode\"]") == std::string::npos);
    REQUIRE(defaults.unit.source.find("[\"call_local\"]") == std::string::npos);
    REQUIRE(defaults.unit.source.find("[\"channel\"]") == std::string::npos);

    REQUIRE(explicit_defaults.success);
    REQUIRE(explicit_defaults.unit.source.find("TRANSFER_MODE_UNRELIABLE") != std::string::npos);
    REQUIRE(explicit_defaults.unit.source.find("[\"call_local\"] = false") != std::string::npos);
    REQUIRE(explicit_defaults.unit.source.find("[\"channel\"] = int64_t{0}") != std::string::npos);
}

TEST_CASE("compiler accepts inert RPC metadata on non-Node classes") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("rpc_ref.gd", "extends RefCounted\n"
                                                       "@rpc(\"call_local\")\n"
                                                       "func local_only() -> void:\n"
                                                       "    pass\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("rpc_config(") == std::string::npos);
}

TEST_CASE("compiler rejects malformed and ambiguous RPC configurations") {
    const gdpp::Compiler compiler;
    const auto duplicate =
        compiler.compile("duplicate_rpc.gd", "@rpc(\"authority\", \"any_peer\")\n"
                                             "func synchronize() -> void:\n"
                                             "    pass\n");
    const auto invalid = compiler.compile("invalid_rpc.gd", "@rpc(\"ordered\")\n"
                                                            "func synchronize() -> void:\n"
                                                            "    pass\n");
    const auto channel = compiler.compile(
        "channel_rpc.gd", "@rpc(\"authority\", \"call_remote\", \"reliable\", \"one\")\n"
                          "func synchronize() -> void:\n"
                          "    pass\n");
    const auto repeated = compiler.compile("repeated_rpc.gd", "@rpc\n"
                                                              "@rpc\n"
                                                              "func synchronize() -> void:\n"
                                                              "    pass\n");
    const auto awaited = compiler.compile("awaited_rpc.gd", "@rpc(await \"authority\")\n"
                                                            "func synchronize() -> void:\n"
                                                            "    pass\n");

    REQUIRE(!duplicate.success);
    REQUIRE(!invalid.success);
    REQUIRE(!channel.success);
    REQUIRE(!repeated.success);
    REQUIRE(!awaited.success);
    REQUIRE(std::any_of(duplicate.diagnostics.begin(), duplicate.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4136"; }));
    REQUIRE(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4135"; }));
    REQUIRE(std::any_of(channel.diagnostics.begin(), channel.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4137"; }));
    REQUIRE(std::any_of(repeated.diagnostics.begin(), repeated.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4133"; }));
    REQUIRE(std::any_of(
        awaited.diagnostics.begin(), awaited.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "GDS4090" &&
                   diagnostic.message.find("annotation arguments") != std::string::npos;
        }));
}

TEST_CASE("compiler initializes onready fields immediately before ready") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("hud.gd", "extends Node\n"
                                   "@export_group(\"Nodes\", \"node_\")\n"
                                   "@onready var node_label: Label = $Label as Label\n"
                                   "func _ready() -> void:\n"
                                   "    node_label.text = \"ready\"\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("gdpp::runtime::ObjectStorage<godot::Label> node_label{}") !=
            std::string::npos);
    const auto initialization = result.unit.source.find("node_label =");
    const auto user_body = result.unit.source.find("->set_text");
    REQUIRE(initialization != std::string::npos);
    REQUIRE(user_body != std::string::npos);
    REQUIRE(initialization < user_body);
    REQUIRE(result.unit.source.find(
                "Cannot assign member 'text' on a null or freed object at hud.gd:5") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("ADD_GROUP(\"Nodes\", \"node_\")") != std::string::npos);
    REQUIRE(result.unit.source.find("D_METHOD(\"_ready\"") == std::string::npos);
}

TEST_CASE("compiler preloads member resources before instances are constructed") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "preloaded_scene.gd", "extends Node\nvar scene = preload(\"res://effects/spark.tscn\")\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("static void _gdpp_preload_resources()") != std::string::npos);
    REQUIRE(result.unit.header.find("static godot::Variant& _gdpp_preloaded_scene()") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("scene = _gdpp_preloaded_scene();") != std::string::npos);
    REQUIRE(
        result.unit.source.find("_gdpp_preloaded_scene() = gdpp::runtime::to_variant("
                                "gdpp::runtime::strict_native_ref_storage<godot::PackedScene>(") !=
        std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::load_resource(") != std::string::npos);
    REQUIRE(result.unit.header.find("static void _gdpp_release_preloaded_resources()") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_preload_initialization()") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_preload_initialization().run(") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_preload_initialization().reset(") != std::string::npos);
    REQUIRE(result.unit.source.find("std::once_flag") == std::string::npos);
    REQUIRE(result.unit.source.find("std::call_once") == std::string::npos);
    REQUIRE(
        result.unit.source.find("_gdpp_preloaded_scene() = "
                                "std::remove_reference_t<decltype(_gdpp_preloaded_scene())>{};") !=
        std::string::npos);
    REQUIRE(result.unit.source.find("if (!gdpp_editor_hint) _gdpp_preload_resources();") !=
            std::string::npos);
}

TEST_CASE("script initialization is transactional across roots and internal classes") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("transactional_initialization.gd",
                                         "extends Node\n"
                                         "static var root_value: int = [1][4]\n"
                                         "var root_scene = preload(\"res://root.tscn\")\n"
                                         "static func _static_init() -> void:\n"
                                         "    root_value += 1\n"
                                         "class Worker:\n"
                                         "    static var worker_value: int = [1][4]\n"
                                         "    var worker_scene = preload(\"res://worker.tscn\")\n"
                                         "    static func _static_init() -> void:\n"
                                         "        worker_value += 1\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;
    REQUIRE(std::count(source.begin(), source.end(), '\n') > 20);
    REQUIRE(source.find("Script initialization previously failed") == std::string::npos);
    REQUIRE(source.find("Static initialization previously failed for GDPPNative_") !=
            std::string::npos);
    REQUIRE(source.find("Preload initialization previously failed for GDPPNative_") !=
            std::string::npos);
    REQUIRE(source.find("_gdpp_static_root_value_release();") != std::string::npos);
    REQUIRE(source.find("_gdpp_static_worker_value_release();") != std::string::npos);
    REQUIRE(source.find("_gdpp_preloaded_root_scene()") != std::string::npos);
    REQUIRE(source.find("_gdpp_preloaded_worker_scene()") != std::string::npos);
    REQUIRE(source.find("std::once_flag") == std::string::npos);
    REQUIRE(source.find("std::call_once") == std::string::npos);

    std::size_t transactions = 0;
    constexpr std::string_view run{"ScriptInitializationState state"};
    for (auto position = source.find(run); position != std::string::npos;
         position = source.find(run, position + run.size()))
        ++transactions;
    REQUIRE(transactions >= std::size_t{4});
}

TEST_CASE("compiler releases script static storage before Godot servers stop") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("static_cache.gd", "extends Node\n"
                                                            "static var cached_values: Array = []\n"
                                                            "static var cached_node: Node\n");

    REQUIRE(result.success);
    const auto release = result.unit.source.find("::_gdpp_release_preloaded_resources() {");
    REQUIRE(release != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_static_cached_values_release();", release) !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_static_cached_node_release();", release) !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant GDPPNative_StaticCache::cached_values") ==
            std::string::npos);
    REQUIRE(result.unit.source.find("static std::atomic<godot::Array*> value{nullptr}") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "static std::atomic<gdpp::runtime::ObjectStorage<godot::Node>*> value{nullptr}") !=
            std::string::npos);
}

TEST_CASE("compiler lazily initializes and explicitly releases resource constants") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "constant_scene.gd", "extends Node\nconst SCENE = preload(\"res://effects/spark.tscn\")\n"
                             "func scene_resource():\n    return SCENE\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("static const godot::Ref<godot::PackedScene>& SCENE()") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_constant_SCENE_storage()") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_constant_SCENE_ready()") != std::string::npos);
    const auto getter = result.unit.source.find("::SCENE()");
    REQUIRE(getter != std::string::npos);
    REQUIRE(result.unit.source.find("SCENE()", getter + std::string{"::SCENE()"}.size()) !=
            std::string::npos);
    const auto release = result.unit.source.find("::_gdpp_release_preloaded_resources() {");
    REQUIRE(release != std::string::npos);
    REQUIRE(result.unit.source.find(
                "_gdpp_constant_SCENE_storage() = "
                "std::remove_reference_t<decltype(_gdpp_constant_SCENE_storage())>{};",
                release) != std::string::npos);
}

TEST_CASE("compiler safely assigns every reference-backed Godot storage family") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dictionary_storage.gd",
                         "extends Node\n"
                         "const STREAMS: Dictionary = {0: preload(\"res://audio/theme.wav\")}\n"
                         "var state: Dictionary = {\"ready\": true}\n"
                         "var typed_state: Dictionary[String, int] = {\"ready\": 1}\n"
                         "var labels: Array[String] = [\"ready\"]\n"
                         "var bytes: PackedByteArray = PackedByteArray([1, 2])\n"
                         "var title: String = \"ready\"\n"
                         "var key: StringName = &\"ready\"\n"
                         "var path: NodePath = ^\"root\"\n"
                         "var callback: Callable = reset\n"
                         "@onready var ready_state: Dictionary = {\"node\": get_node(\".\")}\n"
                         "func reset() -> void:\n"
                         "    state = {}\n"
                         "    state = state\n"
                         "    typed_state = typed_state\n"
                         "    labels = labels\n"
                         "    bytes = bytes\n"
                         "    title = title\n"
                         "    key = key\n"
                         "    path = path\n"
                         "    callback = callback\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::assign_native_storage(_gdpp_constant_STREAMS_storage(),") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::assign_native_storage(state,") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::assign_native_storage(ready_state,") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::assign_native_storage(_gdpp_constant_STREAMS_storage(), "
                "std::remove_reference_t<decltype(_gdpp_constant_STREAMS_storage())>{})") !=
            std::string::npos);
    for (const auto* storage :
         {"state", "typed_state", "labels", "bytes", "title", "key", "path", "callback"}) {
        REQUIRE(
            result.unit.source.find("gdpp::runtime::assign_native_storage(" + std::string{storage} +
                                    ", std::move(_gdpp_assignment_result_") != std::string::npos);
    }
}

TEST_CASE("compiler defers instance initialization while the editor exports scenes") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "service_state.gd",
        "extends Node\nvar service_value = Engine.get_singleton(\"CustomerService\").value\n");

    REQUIRE(result.success);
    const auto editor_hint =
        result.unit.source.find("const bool gdpp_editor_hint = gdpp::runtime::is_editor_hint();");
    const auto guard = result.unit.source.find("if (!gdpp_editor_hint) {");
    const auto initialization = result.unit.source.find("service_value =");
    const auto guard_end = result.unit.source.find("    }", initialization);
    REQUIRE(editor_hint != std::string::npos);
    REQUIRE(guard != std::string::npos);
    REQUIRE(initialization != std::string::npos);
    REQUIRE(guard_end != std::string::npos);
    REQUIRE(editor_hint < guard);
    REQUIRE(guard < initialization);
    REQUIRE(initialization < guard_end);
}

TEST_CASE("compiler exposes pure field defaults while keeping service initializers deferred") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("scene_defaults.gd",
                         "extends Node\n"
                         "@export var speed: int = 300\n"
                         "@export var offset: Vector2 = Vector2(2.0, 4.0)\n"
                         "var service_value = Engine.get_singleton(\"CustomerService\").value\n");

    REQUIRE(result.success);
    const auto editor_hint =
        result.unit.source.find("const bool gdpp_editor_hint = gdpp::runtime::is_editor_hint();");
    const auto speed = result.unit.source.find("speed = static_cast<int64_t>(300);");
    const auto offset = result.unit.source.find("offset =");
    const auto guard = result.unit.source.find("if (!gdpp_editor_hint) {");
    const auto service = result.unit.source.find("service_value =", guard);
    REQUIRE(editor_hint != std::string::npos);
    REQUIRE(speed != std::string::npos);
    REQUIRE(offset != std::string::npos);
    REQUIRE(guard != std::string::npos);
    REQUIRE(service != std::string::npos);
    REQUIRE(editor_hint < speed);
    REQUIRE(speed < offset);
    REQUIRE(offset < guard);
    REQUIRE(guard < service);
    REQUIRE(result.unit.source.find("speed = static_cast<int64_t>(300);", speed + 1) ==
            std::string::npos);
    const auto later_offset = result.unit.source.find("offset =", offset + 1);
    REQUIRE(later_offset == std::string::npos || service < later_offset);
}

TEST_CASE("tool scripts execute initialization paths inside the editor") {
    const auto result = gdpp::Compiler{}.compile(
        "editor_state.gd",
        "@tool\n"
        "extends Node\n"
        "var scene = preload(\"res://effects/spark.tscn\")\n"
        "var service_value = Engine.get_singleton(\"CustomerService\").value\n"
        "func _init() -> void:\n"
        "    service_value = 1\n"
        "class Worker:\n"
        "    var nested_value = Engine.get_singleton(\"CustomerService\").value\n"
        "    func _init() -> void:\n"
        "        nested_value = 2\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.is_tool);
    REQUIRE(result.unit.source.find("const bool gdpp_editor_hint = ") == std::string::npos);
    REQUIRE(result.unit.source.find("if (!gdpp_editor_hint) {") == std::string::npos);
    REQUIRE(result.unit.source.find("if (gdpp_editor_hint) return;") == std::string::npos);
    REQUIRE(result.unit.source.find("if (gdpp::runtime::is_editor_hint()) return;") ==
            std::string::npos);
    REQUIRE(result.unit.source.find("    _gdpp_preload_resources();") != std::string::npos);
    REQUIRE(result.unit.source.find("    _init();") != std::string::npos);
}

TEST_CASE("compiler preserves static unload as an explicit generated lifecycle contract") {
    const auto result = gdpp::Compiler{}.compile("static_lifecycle.gd", "@static_unload\n"
                                                                        "extends Node\n"
                                                                        "static var value := 42\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.static_unload);
    REQUIRE(result.unit.header.find("inline static constexpr bool _gdpp_static_unload = true;") !=
            std::string::npos);
}

TEST_CASE("editor extension base classes require tool execution mode") {
    const gdpp::Compiler compiler;
    const auto invalid_plugin = compiler.compile(
        "editor_plugin.gd", "extends EditorPlugin\nfunc _enter_tree():\n    pass\n");
    const auto invalid_script =
        compiler.compile("editor_script.gd", "extends EditorScript\nfunc _run():\n    pass\n");
    const auto valid_plugin = compiler.compile(
        "tool_editor_plugin.gd", "@tool\nextends EditorPlugin\nfunc _enter_tree():\n    pass\n");

    REQUIRE(!invalid_plugin.success);
    REQUIRE(!invalid_script.success);
    REQUIRE(valid_plugin.success);
    REQUIRE(std::any_of(invalid_plugin.diagnostics.begin(), invalid_plugin.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4152"; }));
    REQUIRE(std::any_of(invalid_script.diagnostics.begin(), invalid_script.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4152"; }));
}

TEST_CASE("compiler publishes valid script icon metadata") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile("icon_class.gd", "@icon(\"icons/type.svg\")\n"
                                                         "class_name IconClass\n"
                                                         "extends Node\n");
    const auto empty = compiler.compile("empty_icon.gd", "@icon(\"\")\n"
                                                         "class_name EmptyIcon\n"
                                                         "extends Node\n");

    REQUIRE(valid.success);
    REQUIRE(valid.unit.icon_path.has_value());
    REQUIRE_EQ(*valid.unit.icon_path, std::string{"icons/type.svg"});
    REQUIRE(!empty.success);
    REQUIRE(std::any_of(empty.diagnostics.begin(), empty.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4115"; }));
}

TEST_CASE("compiler preserves UTF-8 and unique-node paths in generated Godot strings") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("unicode_nodes.gd", "extends Node\n"
                                             "func locate() -> void:\n"
                                             "    var volume = %\"全局音量条\"\n"
                                             "    volume.set(\"标题\", \"中文\")\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::NodePath(godot::String::utf8(\"%全局音量条\"))") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::String::utf8(\"中文\")") != std::string::npos);
}

TEST_CASE("compiler lowers inherited methods as first-class callables") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("deferred_child.gd", "extends Node\n"
                                              "func defer_child(child: Node) -> void:\n"
                                              "    add_child.call_deferred(child)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Callable(this, godot::StringName(\"add_child\"))") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".call_deferred(") != std::string::npos);
}

TEST_CASE("compiler emits explicit engine super method dispatch") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("super.gd", "extends Node\n"
                                                     "func release_later() -> void:\n"
                                                     "    super.queue_free()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Node::queue_free()") != std::string::npos);
}

TEST_CASE("compiler lowers top-level signal awaits to lifetime-aware continuations") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("await.gd", "extends Node\n"
                                                     "signal resumed(value: int)\n"
                                                     "func run(input: int) -> void:\n"
                                                     "    var captured := input + 1\n"
                                                     "    await resumed\n"
                                                     "    print(captured)\n"
                                                     "    await resumed\n"
                                                     "    print(captured + 1)\n");

    REQUIRE(result.success);
    const auto first_await = result.unit.source.find("gdpp::runtime::await_signal");
    REQUIRE(first_await != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::await_signal", first_await + 1) !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Signal(this, godot::StringName(\"resumed\"))") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("[=](const godot::Array &_gdpp_await_values_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("= captured;") != std::string::npos);
}

TEST_CASE("compiler retains every lexical local and awaited temporary receiver while suspended") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("await_lifetime.gd", "extends Node\n"
                                              "signal resumed\n"
                                              "func callback() -> Callable:\n"
                                              "    return func() -> int:\n"
                                              "        await resumed\n"
                                              "        return 42\n"
                                              "func run(parameter: RefCounted) -> int:\n"
                                              "    var unrelated := RefCounted.new()\n"
                                              "    await resumed\n"
                                              "    return await callback().call()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("static_cast<void>(parameter);") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(unrelated);") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "static_cast<void>(_gdpp_id_40676470702d61776169742d76616c75652d") !=
            std::string::npos);
}

TEST_CASE("compiler flattens long await chains into bounded MIR dispatch") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("long_await_chain.gd", "extends Node\n"
                                                                "signal resumed\n"
                                                                "func run(enabled: bool) -> void:\n"
                                                                "    await resumed\n"
                                                                "    await resumed\n"
                                                                "    await resumed\n"
                                                                "    if enabled:\n"
                                                                "        await resumed\n"
                                                                "        await resumed\n"
                                                                "    else:\n"
                                                                "        await resumed\n"
                                                                "    await resumed\n"
                                                                "    await resumed\n"
                                                                "    await resumed\n"
                                                                "    await resumed\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("using _gdpp_async_step_type_") != std::string::npos);
    REQUIRE(result.unit.source.find("switch (_gdpp_async_pc_") != std::string::npos);
    REQUIRE(result.unit.source.find("std::weak_ptr<_gdpp_async_step_type_") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(enabled);") != std::string::npos);
    REQUIRE(result.unit.source.find("[=](const godot::Array &) mutable") == std::string::npos);
    std::size_t await_count = 0;
    for (std::size_t offset = 0; (offset = result.unit.source.find("gdpp::runtime::await_signal",
                                                                   offset)) != std::string::npos;
         offset += 1) {
        ++await_count;
    }
    REQUIRE_EQ(await_count, std::size_t{10});
}

TEST_CASE("compiler keeps local-bearing long coroutines on a lifetime-preserving state machine") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("long_local_await_chain.gd", "extends Node\n"
                                                      "signal resumed\n"
                                                      "func run() -> void:\n"
                                                      "    var retained := RefCounted.new()\n"
                                                      "    await resumed\n"
                                                      "    await resumed\n"
                                                      "    await resumed\n"
                                                      "    await resumed\n"
                                                      "    await resumed\n"
                                                      "    await resumed\n"
                                                      "    await resumed\n"
                                                      "    await resumed\n"
                                                      "    print(retained)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("using _gdpp_async_step_type_") == std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(retained);") != std::string::npos);
}

TEST_CASE("compiler restores signal arguments for await-initialized locals") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("await_value.gd", "extends Node\n"
                                                           "signal selected(value: String)\n"
                                                           "func choose():\n"
                                                           "    var side = await selected\n"
                                                           "    print(side)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::await_result(") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant side =") != std::string::npos);
    const auto argument = result.unit.source.find("= side;");
    const auto failure = result.unit.source.find("script_function_failed()", argument);
    const auto print = result.unit.source.find("godot::UtilityFunctions::print(", failure);
    REQUIRE(argument < failure);
    REQUIRE(failure < print);
}

TEST_CASE("compiler lowers await expressions through ordered continuation temporaries") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("await_expression.gd",
                                         "extends Node\n"
                                         "signal selected(value: int)\n"
                                         "func side(value: int) -> int:\n"
                                         "    return value\n"
                                         "func run() -> void:\n"
                                         "    var total: int = side(10) + await selected\n"
                                         "    print(side(20), await selected, side(30), total)\n"
                                         "    if await selected:\n"
                                         "        print(total)\n");

    REQUIRE(result.success);
    REQUIRE_EQ(std::count(result.unit.source.begin(), result.unit.source.end(), '\0'),
               std::ptrdiff_t{0});
    const auto first_side = result.unit.source.find("static_cast<int64_t>(10)");
    const auto first_await = result.unit.source.find("const godot::Variant _gdpp_awaitable_");
    const auto second_side = result.unit.source.find("static_cast<int64_t>(20)");
    const auto second_await =
        result.unit.source.find("const godot::Variant _gdpp_awaitable_", first_await + 1);
    const auto delayed_side = result.unit.source.find("static_cast<int64_t>(30)");
    REQUIRE(first_side < first_await);
    REQUIRE(second_side < second_await);
    REQUIRE(second_await < delayed_side);
    REQUIRE(result.unit.source.find("gdpp::runtime::await_result(") != std::string::npos);
    REQUIRE(result.unit.source.find("unlowered await expression") == std::string::npos);
}

TEST_CASE("compiler preserves awaited assignment target ordering and writeback") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("await_assignment_target.gd",
                                         "extends Node\n"
                                         "signal selected(value)\n"
                                         "signal replacement(value)\n"
                                         "func run() -> void:\n"
                                         "    (await selected).value = await replacement\n"
                                         "    (await selected)[await replacement] = 42\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::await_signal") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_named") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_key") != std::string::npos);
    REQUIRE(result.unit.source.find("unlowered await expression") == std::string::npos);
    REQUIRE(result.unit.source.find("unsupported structured await") == std::string::npos);
}

TEST_CASE("compiler lowers short-circuit conditional and loop-condition awaits through CFG") {
    const gdpp::Compiler compiler;
    const auto short_circuit =
        compiler.compile("await_short_circuit.gd", "signal selected\n"
                                                   "func run() -> void:\n"
                                                   "    print(true and await selected)\n");
    const auto conditional = compiler.compile("await_conditional.gd",
                                              "signal selected\n"
                                              "func run() -> void:\n"
                                              "    print((await selected) if true else false)\n");
    const auto loop_condition = compiler.compile("await_loop.gd", "signal selected\n"
                                                                  "func run() -> void:\n"
                                                                  "    var count: int = 0\n"
                                                                  "    while await selected:\n"
                                                                  "        count += 1\n"
                                                                  "        if count == 1:\n"
                                                                  "            continue\n"
                                                                  "        break\n"
                                                                  "    print(count)\n");

    REQUIRE(short_circuit.success);
    REQUIRE(conditional.success);
    REQUIRE(loop_condition.success);
    REQUIRE(short_circuit.unit.source.find("gdpp::runtime::await_signal") != std::string::npos);
    REQUIRE(short_circuit.unit.source.find(
                "const bool _gdpp_async_if_condition_0 = static_cast<bool>(true);") !=
            std::string::npos);
    REQUIRE(conditional.unit.source.find("gdpp::runtime::await_signal") != std::string::npos);
    REQUIRE(conditional.unit.source.find("unlowered await expression") == std::string::npos);
    REQUIRE(loop_condition.unit.source.find("std::weak_ptr<std::function<void()>>") !=
            std::string::npos);
    REQUIRE(loop_condition.unit.source.find("_gdpp_async_keep_loop_") != std::string::npos);
    REQUIRE(loop_condition.unit.source.find("_gdpp_async_cell_") != std::string::npos);
}

TEST_CASE("compiler shares suspended while condition state with loop body mutations") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("await_batch_loop.gd", "extends Node\n"
                                                "func fill(limit: int) -> void:\n"
                                                "    var added: int = 0\n"
                                                "    while added < limit:\n"
                                                "        var batch: int = min(200, limit - added)\n"
                                                "        for index in range(batch):\n"
                                                "            added += 1\n"
                                                "        await get_tree().process_frame\n"
                                                "    print(added)\n");

    REQUIRE(result.success);
    const auto declaration = result.unit.source.find("std::make_shared<int64_t>(added)");
    REQUIRE(declaration != std::string::npos);
    const auto cell_begin = result.unit.source.rfind("_gdpp_async_cell_", declaration);
    REQUIRE(cell_begin != std::string::npos);
    const auto cell_end = result.unit.source.find(' ', cell_begin);
    REQUIRE(cell_end != std::string::npos);
    const auto cell = result.unit.source.substr(cell_begin, cell_end - cell_begin);
    const auto loop_condition = result.unit.source.find("if (!(", declaration);
    REQUIRE(loop_condition != std::string::npos);
    const auto loop_condition_end = result.unit.source.find(")) {", loop_condition);
    REQUIRE(loop_condition_end != std::string::npos);
    const auto condition =
        result.unit.source.substr(loop_condition, loop_condition_end - loop_condition);
    REQUIRE(condition.find("(*" + cell + ")") != std::string::npos);
    REQUIRE(condition.find("added") == std::string::npos);
    const auto increment = result.unit.source.find("(*" + cell + ") =", loop_condition_end);
    REQUIRE(increment != std::string::npos);
}

TEST_CASE("compiler keeps valid unused source bindings warning clean in native builds") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("await_unused_bindings.gd",
                                         "extends Node\n"
                                         "signal resumed\n"
                                         "func run(_unused: int) -> void:\n"
                                         "    var _local: int = 1\n"
                                         "    var _callback := func(_value: int): pass\n"
                                         "    for _index in range(2):\n"
                                         "        await resumed\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("[[maybe_unused]] int64_t _unused") != std::string::npos);
    REQUIRE(result.unit.source.find("[[maybe_unused]] int64_t _local") != std::string::npos);
    REQUIRE(result.unit.source.find("[[maybe_unused]] int64_t _value") != std::string::npos);
    REQUIRE(result.unit.source.find("[[maybe_unused]] int64_t _index") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(_gdpp_await_values_") != std::string::npos);
}

TEST_CASE("compiler lowers async iterator break and continue without a reference cycle") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("await_for_control.gd", "signal selected\n"
                                                                 "func run() -> void:\n"
                                                                 "    for value in [1, 2, 3]:\n"
                                                                 "        await selected\n"
                                                                 "        if value == 1:\n"
                                                                 "            continue\n"
                                                                 "        break\n"
                                                                 "    print(42)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("std::weak_ptr<std::function<void()>>") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_async_keep_loop_") != std::string::npos);
    REQUIRE(result.unit.source.find("continue;\n") == std::string::npos);
    REQUIRE(result.unit.source.find("break;\n") == std::string::npos);
}

TEST_CASE("compiler lifts entry parameters through suspended loop recovery") {
    const gdpp::Compiler compiler;
    const auto function = compiler.compile("await_parameter_loop.gd",
                                           "signal selected\n"
                                           "func run(limit: int, count: int = 0) -> void:\n"
                                           "    while await selected:\n"
                                           "        count += 1\n"
                                           "        if count < limit:\n"
                                           "            continue\n"
                                           "        break\n"
                                           "    print(count)\n");
    const auto setter = compiler.compile("await_setter_loop.gd", "signal selected\n"
                                                                 "var score: int = 0:\n"
                                                                 "    set(value):\n"
                                                                 "        while await selected:\n"
                                                                 "            value += 1\n"
                                                                 "            if value < 2:\n"
                                                                 "                continue\n"
                                                                 "            break\n"
                                                                 "        score = value\n");
    const auto nested =
        compiler.compile("await_nested_parameter_loop.gd", "signal selected\n"
                                                           "class Worker:\n"
                                                           "    signal selected\n"
                                                           "    func run(count: int) -> void:\n"
                                                           "        while await selected:\n"
                                                           "            count += 1\n"
                                                           "            break\n"
                                                           "func make_callback():\n"
                                                           "    return func(count: int):\n"
                                                           "        while await selected:\n"
                                                           "            count += 1\n"
                                                           "            break\n");

    REQUIRE(function.success);
    REQUIRE(setter.success);
    REQUIRE(nested.success);
    REQUIRE(function.unit.source.find("std::make_shared<int64_t>(count)") != std::string::npos);
    REQUIRE(function.unit.source.find("_gdpp_utility_argument_") != std::string::npos);
    REQUIRE(function.unit.source.find(" = (*_gdpp_async_cell_") != std::string::npos);
    REQUIRE(setter.unit.source.find("std::make_shared<int64_t>(value)") != std::string::npos);
    REQUIRE(setter.unit.source.find("auto _gdpp_assignment_value_") != std::string::npos);
    REQUIRE(setter.unit.source.find("score = std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    const auto nested_parameter = nested.unit.source.find("std::make_shared<int64_t>(count)");
    REQUIRE(nested_parameter != std::string::npos);
    REQUIRE(nested.unit.source.find("std::make_shared<int64_t>(count)", nested_parameter + 1) !=
            std::string::npos);
}

TEST_CASE("compiler resumes match guards bodies and fallthrough branches") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "await_match_guard.gd", "signal selected\n"
                                "func run(value: int) -> void:\n"
                                "    match value:\n"
                                "        var captured when captured > 0 and await selected:\n"
                                "            print(captured)\n"
                                "            await selected\n"
                                "        2 when await selected:\n"
                                "            print(2)\n"
                                "        _:\n"
                                "            print(-1)\n"
                                "    print(99)\n");
    const auto immediate =
        compiler.compile("immediate_match_guard.gd", "func classify(value: int) -> int:\n"
                                                     "    match value:\n"
                                                     "        1 when await true:\n"
                                                     "            return 10\n"
                                                     "        _:\n"
                                                     "            return 20\n");

    REQUIRE(result.success);
    REQUIRE(immediate.success);
    REQUIRE(result.unit.source.find("_gdpp_async_match_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::await_signal") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_match_bind_") != std::string::npos);
    REQUIRE(result.unit.source.find("unsupported structured await") == std::string::npos);
    REQUIRE(immediate.unit.source.find("gdpp::runtime::await_signal") == std::string::npos);
    REQUIRE(immediate.unit.source.find("static_cast<int64_t>(10)") != std::string::npos);
    REQUIRE(immediate.unit.source.find("static_cast<int64_t>(20)") != std::string::npos);
}

TEST_CASE("compiler emits awaited match branches through one linear dispatcher") {
    std::string source = "signal selected\nfunc run(value: int) -> void:\n    match value:\n";
    constexpr std::size_t guarded_branches = 10;
    for (std::size_t branch = 0; branch < guarded_branches; ++branch) {
        source += "        " + std::to_string(branch) +
                  " when await selected:\n            print(" + std::to_string(branch) + ")\n";
    }
    source += "        _:\n            print(-1)\n";

    const auto result = gdpp::Compiler{}.compile("linear_await_match.gd", source);
    const auto occurrences = [](const std::string& text, const std::string_view needle) {
        std::size_t count = 0;
        for (std::size_t position = 0;
             (position = text.find(needle, position)) != std::string::npos;
             position += needle.size()) {
            ++count;
        }
        return count;
    };

    REQUIRE(result.success);
    REQUIRE_EQ(occurrences(result.unit.source, "gdpp::runtime::await_signal"), guarded_branches);
    REQUIRE_EQ(occurrences(result.unit.source, "using _gdpp_async_match_dispatch_type_"),
               std::size_t{1});
    REQUIRE_EQ(occurrences(result.unit.source, "std::weak_ptr<_gdpp_async_match_dispatch_type_"),
               std::size_t{1});
    REQUIRE(result.unit.source.size() < 100'000U);
}

TEST_CASE("compiler isolates awaited assert operands and emits one shared continuation") {
    const gdpp::Compiler compiler;
    const auto assertion = compiler.compile(
        "await_assert.gd", "signal condition_ready\n"
                           "signal message_ready\n"
                           "func run() -> void:\n"
                           "    assert(await condition_ready, str(await message_ready))\n"
                           "    print(\"continued\")\n");

    const auto occurrences = [](const std::string& text, const std::string& needle) {
        std::size_t count = 0;
        for (std::size_t position = 0;
             (position = text.find(needle, position)) != std::string::npos;
             position += needle.size()) {
            ++count;
        }
        return count;
    };
    REQUIRE(assertion.success);
    REQUIRE_EQ(occurrences(assertion.unit.source, "gdpp::runtime::await_signal"), std::size_t{2});
    REQUIRE_EQ(occurrences(assertion.unit.source, "auto _gdpp_after_assert_"), std::size_t{1});
    const auto debug_begin = assertion.unit.source.find("#ifdef GDPP_SCRIPT_DEBUG_ENABLED");
    const auto first_await = assertion.unit.source.find("const godot::Variant _gdpp_awaitable_");
    const auto release_branch = assertion.unit.source.find("#else", debug_begin);
    const auto debug_end = assertion.unit.source.find("#endif", release_branch);
    REQUIRE(debug_begin < first_await);
    REQUIRE(first_await < release_branch);
    REQUIRE(release_branch < debug_end);
    REQUIRE(assertion.unit.source.find("ERR_FAIL_EDMSG", first_await) < release_branch);
}

TEST_CASE("compiler preserves typed returns for non-suspending awaited asserts") {
    const gdpp::Compiler compiler;
    const auto assertion =
        compiler.compile("immediate_assert.gd", "func run() -> int:\n"
                                                "    assert(await true, await \"message\")\n"
                                                "    return 7\n");

    REQUIRE(assertion.success);
    REQUIRE(assertion.unit.source.find("gdpp::runtime::await_signal") == std::string::npos);
    REQUIRE(assertion.unit.source.find("ERR_FAIL_V_EDMSG") != std::string::npos);
    REQUIRE(assertion.unit.source.find("const auto _gdpp_return_value_") != std::string::npos);
    REQUIRE(assertion.unit.source.find(" = static_cast<int64_t>(7);") != std::string::npos);
}

TEST_CASE("compiler applies truthiness to typed containers with short circuiting") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("container_truth.gd", "extends Node\n"
                                               "var items: Array[int] = []\n"
                                               "func has_items() -> bool:\n"
                                               "    return items && items.size()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("const bool _gdpp_logic_left_") != std::string::npos);
    REQUIRE(result.unit.source.find("(gdpp::runtime::to_variant(items)).booleanize()") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("if (!_gdpp_logic_left_") != std::string::npos);
}

TEST_CASE("compiler applies zero-value truthiness to every Godot value family") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile(
        "truthiness_matrix.gd",
        "func evaluate(node: Node, callback: Callable, event: Signal, dynamic: Variant) -> "
        "Array[bool]:\n"
        "    var results: Array[bool] = []\n"
        "    results.append(not null)\n"
        "    results.append(not 0)\n"
        "    results.append(not 0.0)\n"
        "    results.append(not \"\")\n"
        "    results.append(not StringName())\n"
        "    results.append(not NodePath())\n"
        "    results.append(not Vector2())\n"
        "    results.append(not Rect2())\n"
        "    results.append(not Transform2D())\n"
        "    results.append(not Color())\n"
        "    results.append(not RID())\n"
        "    results.append(not callback)\n"
        "    results.append(not event)\n"
        "    results.append(not {})\n"
        "    results.append(not [])\n"
        "    results.append(not PackedByteArray())\n"
        "    results.append(not node)\n"
        "    results.append(not self)\n"
        "    match 1:\n"
        "        1 when \"enabled\": results.append(true)\n"
        "    if dynamic and [1]:\n"
        "        results.append(true)\n"
        "    return results\n");
    const auto invalid =
        compiler.compile("void_truthiness.gd", "func nothing() -> void:\n"
                                               "    pass\n"
                                               "func reject() -> void:\n"
                                               "    if nothing(): pass\n"
                                               "    while nothing(): break\n"
                                               "    assert(nothing())\n"
                                               "    var selected = 1 if nothing() else 2\n"
                                               "    var logical = nothing() and true\n"
                                               "    var negated = not nothing()\n"
                                               "    match 1:\n"
                                               "        1 when nothing(): pass\n"
                                               "    print(selected, logical, negated)\n");
    const auto booleanize_count = [&]() {
        std::size_t count = 0;
        for (std::size_t position = 0;
             (position = valid.unit.source.find(".booleanize()", position)) != std::string::npos;
             position += std::string_view{".booleanize()"}.size()) {
            ++count;
        }
        return count;
    };

    REQUIRE(valid.success);
    REQUIRE(booleanize_count() >= std::size_t{14});
    REQUIRE(valid.unit.source.find("(node != nullptr)") != std::string::npos);
    REQUIRE(valid.unit.source.find("(!(true))") != std::string::npos);
    REQUIRE(!invalid.success);
    REQUIRE_EQ(std::count_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS4153"; }),
               std::ptrdiff_t{7});
}

TEST_CASE("compiler infers exported PackedScene resources from preload paths") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "scene_export.gd", "extends Node\n"
                           "@export var projectile_scene = preload(\"res://projectile.tscn\")\n");

    REQUIRE(result.success);
    REQUIRE(
        result.unit.source.find("PROPERTY_HINT_RESOURCE_TYPE, godot::String(\"PackedScene\")") !=
        std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::load_resource(") != std::string::npos);
}

TEST_CASE("compiler preserves typed results and independent state in coroutine lambdas") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("lambda_await.gd", "extends Node\n"
                                            "signal resumed\n"
                                            "func callback() -> Callable:\n"
                                            "    var captured := 38\n"
                                            "    return func delayed(addend: int) -> int:\n"
                                            "        await resumed\n"
                                            "        return captured + addend\n"
                                            "func invoke() -> int:\n"
                                            "    return await callback().call(4)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::await_signal") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::begin_coroutine(this)") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_lambda_coroutine_state_") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::complete_coroutine(_gdpp_lambda_coroutine_state_") !=
            std::string::npos);
    const auto lambda_state = result.unit.source.find("const auto _gdpp_lambda_coroutine_state_");
    const auto typed_sum =
        result.unit.source.find("gdpp::integer::add(_gdpp_integer_left_", lambda_state);
    const auto completion = result.unit.source.find(
        "gdpp::runtime::complete_coroutine(_gdpp_lambda_coroutine_state_", lambda_state);
    const auto safe_fault_accessor =
        result.unit.source.find("const auto script_function_failed = []() noexcept { return "
                                "gdpp::runtime::script_function_failed(); };",
                                lambda_state);
    const auto lambda_end = result.unit.source.find("\n});", lambda_state);
    REQUIRE(lambda_state != std::string::npos);
    REQUIRE(typed_sum != std::string::npos);
    REQUIRE(completion != std::string::npos);
    REQUIRE(safe_fault_accessor != std::string::npos);
    REQUIRE(lambda_end != std::string::npos);
    REQUIRE(safe_fault_accessor < lambda_end);
    const auto coroutine_lambda_source =
        result.unit.source.substr(lambda_state, lambda_end - lambda_state);
    REQUIRE(coroutine_lambda_source.find("[&_gdpp_script_function_scope]") == std::string::npos);
}

TEST_CASE("compiler preserves dynamic coroutine return values through the native ABI") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("dynamic_coroutine.gd", "extends Node\n"
                                                                 "signal resumed\n"
                                                                 "func spawn():\n"
                                                                 "    await resumed\n"
                                                                 "    return 42\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("godot::Variant spawn()") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::begin_coroutine(this)") != std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_return_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::complete_coroutine(_gdpp_coroutine_state, "
                                    "gdpp::runtime::to_variant(_gdpp_return_value_") !=
            std::string::npos);
}

TEST_CASE("compiler supports structured awaits across instance and static coroutine contracts") {
    const gdpp::Compiler compiler;
    const auto nested = compiler.compile("nested_await.gd", "signal resumed\n"
                                                            "func run():\n"
                                                            "    if true:\n"
                                                            "        await resumed\n"
                                                            "    for index in range(3):\n"
                                                            "        await resumed\n"
                                                            "        print(index)\n");
    const auto non_void = compiler.compile("value_await.gd", "signal resumed\n"
                                                             "func run() -> int:\n"
                                                             "    await resumed\n"
                                                             "    return 1\n");
    const auto static_await =
        compiler.compile("static_await.gd", "static func run(resumed: Signal) -> void:\n"
                                            "    await resumed\n");
    const auto nonsignal = compiler.compile("bad_await.gd", "func run() -> void:\n"
                                                            "    await 42\n");
    const auto ignored =
        compiler.compile("ignored_await.gd", "func run() -> void:\n"
                                             "    @warning_ignore(\"redundant_await\")\n"
                                             "    await 42\n");
    const auto initializer = compiler.compile("init_await.gd", "signal resumed\n"
                                                               "func _init() -> void:\n"
                                                               "    await resumed\n");

    REQUIRE(nested.success);
    REQUIRE(nested.unit.source.find("std::make_shared<std::function<void()>>") !=
            std::string::npos);
    REQUIRE(nested.unit.source.find("gdpp::runtime::iter_next") != std::string::npos);
    REQUIRE(non_void.success);
    REQUIRE(non_void.unit.header.find("godot::Variant run()") != std::string::npos);
    REQUIRE(non_void.unit.source.find("gdpp::runtime::coroutine_result(") != std::string::npos);
    REQUIRE(static_await.success);
    REQUIRE(static_await.unit.header.find("static godot::Variant run(godot::Signal resumed)") !=
            std::string::npos);
    REQUIRE(static_await.unit.source.find("gdpp::runtime::begin_coroutine(nullptr)") !=
            std::string::npos);
    REQUIRE(static_await.unit.source.find(
                "gdpp::runtime::coroutine_owner(_gdpp_coroutine_state)") != std::string::npos);
    REQUIRE(nonsignal.success);
    REQUIRE(nonsignal.unit.source.find("static_cast<void>(static_cast<int64_t>(42))") !=
            std::string::npos);
    REQUIRE(std::any_of(nonsignal.diagnostics.begin(), nonsignal.diagnostics.end(),
                        [](const gdpp::Diagnostic& diagnostic) {
                            return diagnostic.severity == gdpp::DiagnosticSeverity::warning &&
                                   diagnostic.code == "GDS4093";
                        }));
    REQUIRE(ignored.success);
    REQUIRE(std::none_of(
        ignored.diagnostics.begin(), ignored.diagnostics.end(),
        [](const gdpp::Diagnostic& diagnostic) { return diagnostic.code == "GDS4093"; }));
    REQUIRE(!initializer.success);
    bool found_initializer_await = false;
    for (const auto& diagnostic : initializer.diagnostics)
        found_initializer_await = found_initializer_await || diagnostic.code == "GDS4097";
    REQUIRE(found_initializer_await);
}

TEST_CASE("compiler matches redundant await and internal class coroutine call contracts") {
    const gdpp::Compiler compiler;
    const auto direct =
        compiler.compile("redundant_direct.gd", "func immediate() -> int:\n"
                                                "    @warning_ignore(\"redundant_await\")\n"
                                                "    return await 42\n"
                                                "func consume() -> int:\n"
                                                "    return immediate()\n");
    const auto awaited =
        compiler.compile("redundant_awaited.gd", "func immediate() -> int:\n"
                                                 "    @warning_ignore(\"redundant_await\")\n"
                                                 "    return await 42\n"
                                                 "func consume() -> int:\n"
                                                 "    return await immediate()\n");
    const auto inner =
        compiler.compile("inner_coroutine.gd", "extends Node\n"
                                               "signal resumed\n"
                                               "class Probe extends RefCounted:\n"
                                               "    func forward(next_step: Signal) -> int:\n"
                                               "        return await later(next_step)\n"
                                               "    func later(next_step: Signal) -> int:\n"
                                               "        await next_step\n"
                                               "        return 42\n"
                                               "func consume() -> int:\n"
                                               "    return await Probe.new().forward(resumed)\n");
    const auto constructor =
        compiler.compile("immediate_init.gd", "extends RefCounted\n"
                                              "func _init() -> void:\n"
                                              "    @warning_ignore(\"redundant_await\")\n"
                                              "    await 42\n");

    REQUIRE(!direct.success);
    REQUIRE(std::any_of(direct.diagnostics.begin(), direct.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4132"; }));
    REQUIRE(awaited.success);
    REQUIRE(awaited.unit.header.find("godot::Variant immediate()") != std::string::npos);
    REQUIRE(inner.success);
    REQUIRE(inner.unit.source.find("GDPPNative_InnerCoroutine__Probe::forward") !=
            std::string::npos);
    REQUIRE(inner.unit.source.find("gdpp::runtime::await_signal") != std::string::npos);
    REQUIRE(std::none_of(inner.diagnostics.begin(), inner.diagnostics.end(),
                         [](const auto& diagnostic) { return diagnostic.code == "GDS4093"; }));
    REQUIRE(constructor.success);
}

TEST_CASE("compiler requires consumed coroutine results to be awaited and permits detachment") {
    const gdpp::Compiler compiler;
    const auto direct =
        compiler.compile("direct_coroutine.gd", "signal resumed\n"
                                                "func produce(immediate: bool) -> int:\n"
                                                "    if immediate:\n"
                                                "        return 7\n"
                                                "    await resumed\n"
                                                "    return 8\n"
                                                "func consume() -> int:\n"
                                                "    return produce(true)\n");
    const auto awaited =
        compiler.compile("awaited_coroutine.gd", "signal resumed\n"
                                                 "func produce(immediate: bool) -> int:\n"
                                                 "    if immediate:\n"
                                                 "        return 7\n"
                                                 "    await resumed\n"
                                                 "    return 8\n"
                                                 "func consume() -> int:\n"
                                                 "    return await produce(true)\n");
    const auto detached = compiler.compile("detached_coroutine.gd", "signal resumed\n"
                                                                    "func produce() -> void:\n"
                                                                    "    await resumed\n"
                                                                    "func launch() -> void:\n"
                                                                    "    produce()\n");

    REQUIRE(!direct.success);
    REQUIRE(std::any_of(
        direct.diagnostics.begin(), direct.diagnostics.end(),
        [](const gdpp::Diagnostic& diagnostic) { return diagnostic.code == "GDS4132"; }));
    REQUIRE(awaited.success);
    REQUIRE(awaited.unit.source.find("produce(_gdpp_call_argument_") != std::string::npos);
    REQUIRE(awaited.unit.source.find("gdpp::runtime::is_awaitable(") != std::string::npos);
    REQUIRE(awaited.unit.source.find("gdpp::runtime::await_result(") != std::string::npos);
    REQUIRE(detached.success);
    REQUIRE(detached.unit.source.find("produce()") != std::string::npos);
}

TEST_CASE("compiler applies GDScript truthiness to RefCounted objects") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("resource_truth.gd", "extends Node\n"
                                                              "var shape: BoxShape3D\n"
                                                              "func missing_shape() -> bool:\n"
                                                              "    return not shape\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("(!((shape).is_valid()))") != std::string::npos);
}

TEST_CASE("object conditional expressions emit an explicitly typed null branch") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "nullable_image.gd", "extends Node\n"
                             "func load_image(ok: bool) -> Image:\n"
                             "    var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)\n"
                             "    return image if ok else null\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Ref<godot::Image>{}") != std::string::npos);
}

TEST_CASE("compiler synthesizes ready for onready fields") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("camera.gd", "extends Node\n@onready var camera := $Camera\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("virtual void _ready() override;") != std::string::npos);
    REQUIRE(result.unit.source.find("::_ready()") != std::string::npos);
    REQUIRE(result.unit.source.find("camera = gdpp::runtime::to_variant(get_node") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("D_METHOD(\"_ready\"") == std::string::npos);
}

TEST_CASE("compiler preserves explicit typed iterator variables") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("typed_for.gd", "func sum(values: Array[int]) -> int:\n"
                                                         "    var total: int = 0\n"
                                                         "    for value: int in values:\n"
                                                         "        total += value\n"
                                                         "    return total\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_array_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "int64_t value = gdpp::runtime::strict_builtin_storage<int64_t>("
                "gdpp::runtime::to_variant(_gdpp_array_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_init") == std::string::npos);
}

TEST_CASE("compiler emits native Godot mathematical range loops") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("mathematical_ranges.gd",
                         "func collect(count: float, float_bounds: Vector2, int_bounds: Vector2i, "
                         "float_steps: Vector3, int_steps: Vector3i) -> Array:\n"
                         "    var values: Array = []\n"
                         "    for value: float in count:\n"
                         "        values.append(value)\n"
                         "    for value: float in float_bounds:\n"
                         "        values.append(value)\n"
                         "    for value: int in int_bounds:\n"
                         "        values.append(value)\n"
                         "    for value: float in float_steps:\n"
                         "        values.append(value)\n"
                         "    for value: int in int_steps:\n"
                         "        values.append(value)\n"
                         "    return values\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("const double _gdpp_float_limit_") != std::string::npos);
    REQUIRE(result.unit.source.find("for (double _gdpp_float_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_vector2_bounds_") != std::string::npos);
    REQUIRE(result.unit.source.find("for (double _gdpp_vector2_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("for (int64_t _gdpp_vector2_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_vector3_bounds_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_vector3_step_") != std::string::npos);
    REQUIRE(result.unit.source.find("? _gdpp_vector3_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::integer::range_advance(_gdpp_vector3_value_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_init") == std::string::npos);
}

TEST_CASE("typed iterator variables constrain collection literal elements") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile("typed_literal_loops.gd",
                                        "func collect() -> Array:\n"
                                        "    var result: Array = []\n"
                                        "    for value: float in [1, 2, 3]:\n"
                                        "        result.append(value)\n"
                                        "    for key: StringName in { first = 1, second = 2 }:\n"
                                        "        result.append(key)\n"
                                        "    return result\n");
    const auto invalid_array =
        compiler.compile("invalid_typed_array_loop.gd", "func visit() -> void:\n"
                                                        "    for value: String in [1, 2, 3]:\n"
                                                        "        pass\n");
    const auto invalid_dictionary = compiler.compile("invalid_typed_dictionary_loop.gd",
                                                     "func visit() -> void:\n"
                                                     "    for key: int in { \"name\": 1 }:\n"
                                                     "        pass\n");

    REQUIRE(valid.success);
    REQUIRE(valid.unit.source.find("godot::TypedArray<double> _gdpp_array_") != std::string::npos);
    REQUIRE(valid.unit.source.find("godot::TypedDictionary<godot::StringName, godot::Variant> ") !=
            std::string::npos);
    REQUIRE(!invalid_array.success);
    REQUIRE(std::count_if(invalid_array.diagnostics.begin(), invalid_array.diagnostics.end(),
                          [](const auto& diagnostic) { return diagnostic.code == "GDS4002"; }) >=
            3);
    REQUIRE(!invalid_dictionary.success);
    REQUIRE(std::any_of(invalid_dictionary.diagnostics.begin(),
                        invalid_dictionary.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4002"; }));
}

TEST_CASE("compiler lowers static object iterators through Godot's Variant protocol") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("object_iterator.gd",
                                         "class Iterator:\n"
                                         "    var count: int = 2\n"
                                         "    func _iter_init(state: Array) -> bool:\n"
                                         "        state[0] = 0\n"
                                         "        return true\n"
                                         "    func _iter_next(state: Array) -> bool:\n"
                                         "        state[0] += 1\n"
                                         "        return state[0] < count\n"
                                         "    func _iter_get(state: Variant) -> StringName:\n"
                                         "        return StringName(str(state))\n"
                                         "func collect(iterator: Iterator) -> Array[StringName]:\n"
                                         "    var result: Array[StringName] = []\n"
                                         "    for value in iterator:\n"
                                         "        result.append(value)\n"
                                         "    return result\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_init") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_next") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_get") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName value = "
                                    "gdpp::runtime::strict_builtin_storage<godot::StringName>(") !=
            std::string::npos);
}

TEST_CASE("semantic failures stop before HIR verifier diagnostics") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("invalid_iterator.gd", "func visit() -> void:\n"
                                                                "    for value in true:\n"
                                                                "        pass\n");

    REQUIRE(!result.success);
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4007"; }));
    REQUIRE(
        std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto& diagnostic) { return diagnostic.code.rfind("GDS5", 0) == 0; }));
    REQUIRE_EQ(result.metrics.hir_statement_count, std::size_t{0});
    REQUIRE_EQ(result.metrics.mir_block_count, std::size_t{0});
}

TEST_CASE("compiler iterates packed arrays with their Godot element types") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "packed_for.gd", "func join_arguments() -> String:\n"
                         "    var result := \"\"\n"
                         "    var arguments: PackedStringArray = OS.get_cmdline_user_args()\n"
                         "    for argument: String in arguments:\n"
                         "        result += argument\n"
                         "    return result\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::SharedPackedArray<godot::PackedStringArray> arguments") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::String argument =") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_packed_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find(".size();") != std::string::npos);
    REQUIRE(result.unit.source.find("auto &&_gdpp_packed_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_packed_size_") == std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_init") == std::string::npos);
}

TEST_CASE("compiler emits semantic iteration strategies without backend type guessing") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "static_iteration.gd",
        "func collect(values: Array[int], labels: Dictionary[String, int]) -> Array:\n"
        "    var result: Array = []\n"
        "    for character: String in \"A🙂B\":\n"
        "        result.append(character)\n"
        "    for value: int in values:\n"
        "        result.append(value)\n"
        "    for key: String in labels:\n"
        "        result.append(key)\n"
        "    return result\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("const godot::String _gdpp_string_iterable_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".substr(_gdpp_string_index_") != std::string::npos);
    REQUIRE(result.unit.source.find("auto &&_gdpp_array_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "godot::String key = gdpp::runtime::strict_builtin_storage<godot::String>(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_init") != std::string::npos);
}

TEST_CASE("mutable indexed iterables use live storage and live bounds") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("mutable_iteration.gd", "func collect() -> Array:\n"
                                                 "    var values := [1, 2]\n"
                                                 "    for value in values:\n"
                                                 "        if value == 1:\n"
                                                 "            values.append(3)\n"
                                                 "    var packed := PackedInt64Array([1, 2])\n"
                                                 "    for value in packed:\n"
                                                 "        if value == 1:\n"
                                                 "            packed.append(3)\n"
                                                 "    return [values, packed]\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("auto &&_gdpp_array_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find("auto &&_gdpp_packed_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_array_iterable_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_packed_size_") == std::string::npos);
}

TEST_CASE("uninitialized locals preserve Godot default initialization") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("local_defaults.gd", "extends Node\n"
                                              "func defaults() -> Array:\n"
                                              "    var typed: int\n"
                                              "    var dynamic\n"
                                              "    var object: Node\n"
                                              "    return [typed, dynamic, object]\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("int64_t typed{};") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant dynamic{};") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::ObjectStorage<godot::Node> object{};") !=
            std::string::npos);
}

TEST_CASE("compiler preserves typed subscript and builtin component scalar semantics") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "typed_subscripts.gd",
        "func update(values: Array[int], packed: PackedInt64Array, vector: Vector2) -> float:\n"
        "    values[0] += 2\n"
        "    packed[0] += 3\n"
        "    return vector.x * vector.x + float(values[0] + packed[0])\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_subscript_container_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_typed_array_get<int64_t>("
                                    "_gdpp_subscript_target_") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<int64_t>(values[") == std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_typed_array_get<int64_t>("
                                    "_gdpp_subscript_container_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_array_set("
                                    "_gdpp_subscript_container_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_packed_array_get("
                                    "_gdpp_subscript_container_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_packed_array_set("
                                    "_gdpp_subscript_container_") != std::string::npos);
    REQUIRE(result.unit.source.find("to_variant(gdpp::runtime::checked_packed_array_get(") ==
            std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<double>(vector.x)") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::binary") == std::string::npos);
}

TEST_CASE("compiler contains invalid native sequence indexes instead of dereferencing them") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "checked_subscripts.gd",
        "func read(values: Array[String], bytes: PackedByteArray, index: int) -> String:\n"
        "    values[index] = \"changed\"\n"
        "    bytes[index] = 7\n"
        "    return values[index] + str(bytes[index])\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_typed_array_get<godot::String>("
                                    "_gdpp_subscript_target_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_array_set(") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_packed_array_get("
                                    "_gdpp_subscript_target_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_packed_array_set(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("values[index]") == std::string::npos);
    REQUIRE(result.unit.source.find("bytes.native()[index]") == std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 2, 5") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 3, 5") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 4, 12") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 4, 32") != std::string::npos);
}

TEST_CASE("typed container subscripts preserve Godot runtime failure boundaries") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "typed_container_boundaries.gd",
        "func write_array(values: Array[int], source: Variant) -> void:\n"
        "    values[0] = source\n"
        "func compound_array(values: Array[int], source: Variant) -> void:\n"
        "    values[0] += source\n"
        "func write_dictionary(values: Dictionary[String, int], key: Variant, source: Variant) "
        "-> void:\n"
        "    values[key] = source\n"
        "func compound_dictionary(values: Dictionary[String, int], key: Variant, source: "
        "Variant) -> void:\n"
        "    values[key] += source\n"
        "func read_dictionary(values: Dictionary[String, Variant], key: Variant) -> Variant:\n"
        "    return values[key]\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;
    const auto section = [&](const std::string& function, const std::string& next) {
        const auto begin = source.find("::" + function + "(");
        const auto end = source.find("::" + next + "(", begin + 1);
        REQUIRE(begin != std::string::npos);
        REQUIRE(end != std::string::npos);
        return source.substr(begin, end - begin);
    };

    const auto array_write = section("write_array", "compound_array");
    REQUIRE(array_write.find("gdpp::runtime::checked_array_set(") != std::string::npos);
    REQUIRE(array_write.find("gdpp::runtime::strict_builtin_storage<int64_t>(") ==
            std::string::npos);

    const auto array_compound = section("compound_array", "write_dictionary");
    REQUIRE(array_compound.find("gdpp::runtime::checked_array_set(") != std::string::npos);
    REQUIRE(array_compound.find("gdpp::runtime::strict_builtin_storage<int64_t>(") ==
            std::string::npos);

    const auto dictionary_write = section("write_dictionary", "compound_dictionary");
    REQUIRE(dictionary_write.find("gdpp::runtime::checked_dictionary_set(") != std::string::npos);
    REQUIRE(dictionary_write.find("gdpp::runtime::strict_builtin_storage<int64_t>(") ==
            std::string::npos);

    const auto dictionary_compound = section("compound_dictionary", "read_dictionary");
    REQUIRE(dictionary_compound.find("gdpp::runtime::checked_dictionary_get(") !=
            std::string::npos);
    REQUIRE(dictionary_compound.find("gdpp::runtime::unchecked_dictionary_set(") !=
            std::string::npos);
    const auto dictionary_binary = dictionary_compound.find("gdpp::runtime::binary(");
    REQUIRE(dictionary_binary != std::string::npos);
    REQUIRE(dictionary_compound.find("gdpp::runtime::strict_builtin_storage<int64_t>(",
                                     dictionary_binary) == std::string::npos);

    const auto dictionary_read = source.substr(source.find("::read_dictionary("));
    REQUIRE(dictionary_read.find("gdpp::runtime::checked_dictionary_get(") != std::string::npos);
}

TEST_CASE("compiler compares every static Godot object representation by Variant identity") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "object_identity.gd", "func same_ref(left: RefCounted, right: RefCounted) -> bool:\n"
                              "    return left == right\n"
                              "func different_node(left: Node, right: Node) -> bool:\n"
                              "    return left != right\n");

    REQUIRE(result.success);
    const auto first = result.unit.source.find("gdpp::runtime::binary(godot::Variant::OP_EQUAL");
    REQUIRE(first != std::string::npos);
    const auto second =
        result.unit.source.find("gdpp::runtime::binary(godot::Variant::OP_EQUAL", first + 1);
    REQUIRE(second != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::binary(godot::Variant::OP_EQUAL", second + 1) ==
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_binary_left_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_binary_right_") != std::string::npos);
}

TEST_CASE("compiler sequences checked subscript receivers before indexes") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("ordered_subscripts.gd", "func values() -> Array[int]:\n"
                                                                  "    return [7]\n"
                                                                  "func index() -> int:\n"
                                                                  "    return 0\n"
                                                                  "func read() -> int:\n"
                                                                  "    return values()[index()]\n");

    REQUIRE(result.success);
    const auto target = result.unit.source.find("auto &&_gdpp_subscript_target_");
    const auto receiver = result.unit.source.find("values()", target);
    const auto index_temporary =
        result.unit.source.find("const auto _gdpp_subscript_index_", target);
    const auto index = result.unit.source.find("index()", index_temporary);
    const auto read = result.unit.source.find("gdpp::runtime::checked_array_get("
                                              "_gdpp_subscript_target_",
                                              index);
    REQUIRE(target != std::string::npos);
    REQUIRE(receiver > target);
    REQUIRE(index_temporary > receiver);
    REQUIRE(index > index_temporary);
    REQUIRE(read > index);
}

TEST_CASE("compiler preallocates Array literals and evaluates elements in source order") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("array_literal.gd",
                                         "func build(value: int) -> Array[int]:\n"
                                         "    return [value, value + 1, value + 2, value + 3]\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(".resize(4)") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_array_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Array::make") == std::string::npos);
}

TEST_CASE("compiler lowers direct range loops without allocating a temporary Array") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("range_for.gd", "func sum() -> int:\n"
                                                         "    var total := 0\n"
                                                         "    for value in range(1, 10, 2):\n"
                                                         "        total += value\n"
                                                         "    return total\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_range_start_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_range_stop_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_range_step_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::integer::range_advance(_gdpp_range_value_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_range") == std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_init") == std::string::npos);
}

TEST_CASE("compiler preserves GDScript range vararg runtime failures") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("range_vararg.gd", "func values(first) -> Array[int]:\n"
                                            "    var zero := range()\n"
                                            "    var excessive := range(first, 2, 3, 4)\n"
                                            "    for value in range(1, 2, 3, 4):\n"
                                            "        excessive.push_back(value)\n"
                                            "    return zero + excessive\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_range_checked()") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_range_checked(") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<int64_t>") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_range_start_") == std::string::npos);
}

TEST_CASE("compiler defines static script fields outside the generated header") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("cache.gd", "class_name Cache\n"
                                                     "static var count: int = 1\n"
                                                     "static func increment() -> int:\n"
                                                     "    count += 1\n"
                                                     "    return count\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("static int64_t& _gdpp_static_count_storage()") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("static gdpp::runtime::ScriptInitializationState& "
                                    "_gdpp_static_initialization()") != std::string::npos);
    REQUIRE(result.unit.source.find("value = new int64_t{}") != std::string::npos);
    REQUIRE(result.unit.source.find("*value = static_cast<int64_t>(1)") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_static_initialization().run(") != std::string::npos);
    REQUIRE(result.unit.source.find("static thread_local bool active = false") ==
            std::string::npos);
    const auto increment = result.unit.source.find("::increment() {");
    REQUIRE(increment != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_ensure_static_initialized();", increment) !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_static_count_storage() =") != std::string::npos);
    REQUIRE(result.unit.source.find("int64_t GDPPNative_Cache::count = 1") == std::string::npos);
    REQUIRE(result.unit.header.find("static int64_t _gdpp_get_count()") != std::string::npos);
}

TEST_CASE("compiler preserves owner-free static fields methods lambdas and super calls") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("static_context.gd", "extends Node\n"
                                              "class Parent:\n"
                                              "    static func score() -> int:\n"
                                              "        return 2\n"
                                              "class Child extends Parent:\n"
                                              "    static var total: int = 1:\n"
                                              "        get:\n"
                                              "            return total\n"
                                              "        set(value):\n"
                                              "            total = value\n"
                                              "    static func score() -> int:\n"
                                              "        total += super.score()\n"
                                              "        var adjust := func(value: int) -> int:\n"
                                              "            return value + 1\n"
                                              "        return adjust.call(total)\n"
                                              "func run() -> int:\n"
                                              "    Child.total = Child.score()\n"
                                              "    return Child.total\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("static int64_t score()") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_static_total_storage()") != std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_StaticContext__Parent::score()") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("make_local_callable<") != std::string::npos);
    REQUIRE(result.unit.source.find(">(nullptr") != std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_StaticContext__Child::_gdpp_set_total(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_StaticContext__Child::_gdpp_get_total()") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(" = GDPPNative_StaticContext__Child::_gdpp_get_total();") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("auto &&_gdpp_assignment_receiver_") == std::string::npos);
    REQUIRE(result.unit.source.find("= GDPPNative_StaticContext__Child;") == std::string::npos);
}

TEST_CASE("compiler rejects every implicit instance dependency from static contexts") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "invalid_static_context.gd", "extends Node\n"
                                     "var field: int = 1\n"
                                     "signal pulse\n"
                                     "func instance_method() -> void:\n"
                                     "    pass\n"
                                     "static var invalid_initializer: int = field\n"
                                     "static var invalid_accessor: String:\n"
                                     "    get:\n"
                                     "        return name\n"
                                     "static func invalid_default(value: int = field) -> void:\n"
                                     "    print(value)\n"
                                     "static func invalid_body() -> void:\n"
                                     "    print(self)\n"
                                     "    print(field)\n"
                                     "    pulse.emit()\n"
                                     "    instance_method()\n"
                                     "    print(name)\n"
                                     "    queue_free()\n"
                                     "    print($Child)\n"
                                     "    var callback := func() -> void:\n"
                                     "        print(self)\n"
                                     "    callback.call()\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.header.empty());
    REQUIRE(result.unit.source.empty());
    REQUIRE_EQ(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                             [](const auto& diagnostic) {
                                 return diagnostic.code == "GDS4146" &&
                                        diagnostic.span.end.offset > diagnostic.span.begin.offset;
                             }),
               std::ptrdiff_t{11});
    for (const auto* dependency :
         {"field", "pulse", "instance_method", "name", "queue_free", "self"}) {
        REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                            [dependency](const auto& diagnostic) {
                                return diagnostic.code == "GDS4146" &&
                                       diagnostic.message.find(dependency) != std::string::npos;
                            }));
    }
}

TEST_CASE("semantic analysis enforces abstract function declaration shape") {
    const gdpp::Compiler compiler;
    const auto duplicate =
        compiler.compile("duplicate_abstract.gd", "@abstract\n"
                                                  "class_name DuplicateAbstract\n"
                                                  "@abstract\n"
                                                  "@abstract\n"
                                                  "func execute()\n");
    const auto bodyful = compiler.compile("bodyful_abstract.gd", "@abstract\n"
                                                                 "class_name BodyfulAbstract\n"
                                                                 "@abstract\n"
                                                                 "func execute() -> void:\n"
                                                                 "    pass\n");
    const auto bodyless = compiler.compile("bodyless.gd", "func execute() -> void\n");
    const auto static_abstract =
        compiler.compile("static_abstract.gd", "@abstract\n"
                                               "class_name StaticAbstract\n"
                                               "@abstract\n"
                                               "static func execute() -> void\n");
    const auto duplicate_class = compiler.compile(
        "duplicate_abstract_class.gd", "@abstract @abstract class_name DuplicateClass\n");

    REQUIRE(!duplicate.success);
    REQUIRE(!bodyful.success);
    REQUIRE(!bodyless.success);
    REQUIRE(!static_abstract.success);
    REQUIRE(!duplicate_class.success);
    REQUIRE(std::any_of(duplicate.diagnostics.begin(), duplicate.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4147"; }));
    REQUIRE(std::any_of(bodyful.diagnostics.begin(), bodyful.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4148"; }));
    REQUIRE(std::any_of(bodyless.diagnostics.begin(), bodyless.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4148"; }));
    REQUIRE(std::any_of(static_abstract.diagnostics.begin(), static_abstract.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4147"; }));
    REQUIRE(std::any_of(duplicate_class.diagnostics.begin(), duplicate_class.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4147"; }));
}

TEST_CASE("semantic analysis closes internal abstract class obligations") {
    const gdpp::Compiler compiler;
    const auto valid =
        compiler.compile("abstract_inner.gd", "@abstract class Contract:\n"
                                              "    @abstract func execute(value: int) -> String\n"
                                              "class Implementation extends Contract:\n"
                                              "    func execute(value: int) -> String:\n"
                                              "        return str(value)\n");
    const auto direct = compiler.compile("concrete_abstract.gd", "class InvalidContract:\n"
                                                                 "    @abstract func execute()\n");
    const auto inherited =
        compiler.compile("missing_implementation.gd", "@abstract class Contract:\n"
                                                      "    @abstract func execute()\n"
                                                      "class Missing extends Contract:\n"
                                                      "    pass\n");

    REQUIRE(valid.success);
    REQUIRE(!direct.success);
    REQUIRE(!inherited.success);
    REQUIRE(std::any_of(direct.diagnostics.begin(), direct.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4149"; }));
    REQUIRE(std::any_of(inherited.diagnostics.begin(), inherited.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4149"; }));
}

TEST_CASE("semantic analysis rejects abstract internal class construction") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("construct_abstract.gd", "@abstract class Contract:\n"
                                                  "    pass\n"
                                                  "func create() -> void:\n"
                                                  "    var invalid := Contract.new()\n");

    REQUIRE(!result.success);
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4111"; }));
}

TEST_CASE("semantic analysis rejects calls to unimplemented abstract parents") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("abstract_super.gd", "@abstract class Contract:\n"
                                              "    @abstract func execute() -> void\n"
                                              "class Implementation extends Contract:\n"
                                              "    func execute() -> void:\n"
                                              "        super()\n"
                                              "    func invoke_parent() -> void:\n"
                                              "        super.execute()\n");

    REQUIRE(!result.success);
    REQUIRE_EQ(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS4150"; }),
               std::ptrdiff_t{2});
}

TEST_CASE("compiler emits guarded native stubs for abstract method contracts") {
    const gdpp::Compiler compiler;
    const auto root = compiler.compile("work_contract.gd", "@abstract\n"
                                                           "extends RefCounted\n"
                                                           "class_name WorkContract\n"
                                                           "@abstract\n"
                                                           "func execute(value: int) -> String\n");
    const auto inner =
        compiler.compile("inner_contract.gd", "@abstract class Contract:\n"
                                              "    @abstract func execute(value: int) -> String\n"
                                              "class Implementation extends Contract:\n"
                                              "    func execute(value: int) -> String:\n"
                                              "        return str(value)\n");

    REQUIRE(root.success);
    REQUIRE(root.unit.is_abstract);
    REQUIRE(root.unit.header.find("virtual godot::String execute(int64_t value);") !=
            std::string::npos);
    REQUIRE(root.unit.header.find("execute(int64_t value) = 0") == std::string::npos);
    REQUIRE(root.unit.source.find("GDPPNative_WorkContract::execute(") != std::string::npos);
    REQUIRE(root.unit.source.find("Cannot call abstract function 'execute'.") !=
            std::string::npos);
    REQUIRE(root.unit.source.find("&GDPPNative_WorkContract::_gdpp_variant_call_execute") !=
            std::string::npos);

    REQUIRE(inner.success);
    REQUIRE_EQ(inner.unit.abstract_inner_class_names.size(), std::size_t{1});
    REQUIRE(inner.unit.abstract_inner_class_names.front().find("__Contract") != std::string::npos);
    REQUIRE(inner.unit.header.find("virtual godot::String execute(int64_t value);") !=
            std::string::npos);
    REQUIRE(inner.unit.header.find("virtual godot::String execute(int64_t value) override;") !=
            std::string::npos);
    REQUIRE(inner.unit.header.find("execute(int64_t value) = 0") == std::string::npos);
    REQUIRE(inner.unit.source.find("GDPPNative_InnerContract__Contract::execute(") !=
            std::string::npos);
    REQUIRE(inner.unit.source.find("GDPPNative_InnerContract__Implementation::execute(") !=
            std::string::npos);
}

TEST_CASE("compiler preserves tool execution mode for project registration") {
    const auto result = gdpp::Compiler{}.compile("editor_worker.gd", "@tool\n"
                                                                     "extends Node\n"
                                                                     "func refresh() -> void:\n"
                                                                     "    pass\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.is_tool);
}

TEST_CASE("compiler preserves engine virtual ABI around abstract contracts") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("abstract_process.gd", "@abstract\n"
                                                "extends Node\n"
                                                "class_name AbstractProcess\n"
                                                "@abstract\n"
                                                "func _process(delta: float) -> void\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("virtual void _process(double _gdpp_engine_argument_0) "
                                    "override;") != std::string::npos);
    REQUIRE(result.unit.header.find("virtual void _gdpp_virtual_impl__process(double delta);") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_AbstractProcess::_process(") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "GDPPNative_AbstractProcess::_gdpp_virtual_impl__process(") !=
            std::string::npos);
}

TEST_CASE("compiler resolves versioned builtin value constants") {
    const gdpp::Compiler compiler;
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_7;
    const auto result = compiler.compile("constants.gd",
                                         "var color := Color.GRAY\n"
                                         "var direction: Vector3 = Vector3.UP\n",
                                         options);

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Color(0.74509805") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Vector3(0, 1, 0)") != std::string::npos);
}

TEST_CASE("compiler lowers negated membership through the Variant operator") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "membership.gd", "func missing(value: Variant, items: Array) -> bool:\n"
                         "    return value not in items\n"
                         "func contains_node(value: Node, items: Array[Node]) -> bool:\n"
                         "    return value in items\n"
                         "func sibling_material(value: CanvasItemMaterial) -> ShaderMaterial:\n"
                         "    return value as ShaderMaterial\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "!(static_cast<bool>(gdpp::runtime::binary(godot::Variant::OP_IN") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::ShaderMaterial>") !=
            std::string::npos);
}

TEST_CASE("compiler assigns shader resources through the property accessor base type") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("shader_material.gd", "extends TextureRect\n"
                                               "func install(shader: Shader) -> Material:\n"
                                               "    var effect := ShaderMaterial.new()\n"
                                               "    effect.shader = shader\n"
                                               "    material = effect\n"
                                               "    return material\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Ref<godot::Material>(godot::Object::cast_to<"
                                    "godot::Material>((_gdpp_assignment_value_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("set_material(std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("cast_to<godot::CanvasItemMaterial>") == std::string::npos);
    REQUIRE(result.unit.source.find("godot::Ref<godot::Material>") != std::string::npos);
}

TEST_CASE("compiler applies Material ABI across every shader-capable property family") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "shader_property_families.gd",
        "func install(\n"
        "    canvas: CanvasItem,\n"
        "    tile: TileData,\n"
        "    particles_2d: GPUParticles2D,\n"
        "    particles_3d: GPUParticles3D,\n"
        "    fog: FogVolume,\n"
        "    sky: Sky,\n"
        "    geometry: GeometryInstance3D,\n"
        "    csg: CSGBox3D,\n"
        "    mesh: PrimitiveMesh,\n"
        "    effect: ShaderMaterial,\n"
        ") -> void:\n"
        "    canvas.material = effect\n"
        "    tile.material = effect\n"
        "    particles_2d.process_material = effect\n"
        "    particles_3d.process_material = effect\n"
        "    fog.material = effect\n"
        "    sky.sky_material = effect\n"
        "    geometry.material_override = effect\n"
        "    geometry.material_overlay = effect\n"
        "    csg.material = effect\n"
        "    mesh.material = effect\n"
        "    (canvas.material as ShaderMaterial).set_shader_parameter(&\"pulse\", 1.0)\n"
        "    canvas.material.set(\"shader_parameter/pulse\", 2.0)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("cast_to<godot::CanvasItemMaterial>") == std::string::npos);
    REQUIRE(result.unit.source.find("cast_to<godot::FogMaterial>") == std::string::npos);
    REQUIRE(result.unit.source.find("cast_to<godot::PanoramaSkyMaterial>") == std::string::npos);
    REQUIRE(result.unit.source.find("set_process_material(_gdpp_property_assigned_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("set_material(_gdpp_property_assigned_") != std::string::npos);
    REQUIRE(result.unit.source.find("set_material_override(_gdpp_property_assigned_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("set_material_overlay(_gdpp_property_assigned_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("set_shader_parameter") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::String(\"shader_parameter/pulse\")") !=
            std::string::npos);
}

TEST_CASE("compiler routes hidden Godot property accessors through Object properties") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("hidden_property_accessors.gd",
                         "extends Node\n"
                         "func configure(option: OptionButton, control: Control) -> void:\n"
                         "    option.selected = 2\n"
                         "    control.anchors_preset = 1\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_named(") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"selected\")") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"anchors_preset\")") != std::string::npos);
    REQUIRE(result.unit.source.find("->_select_int(") == std::string::npos);
    REQUIRE(result.unit.source.find("->_set_anchors_layout_preset(") == std::string::npos);
}

TEST_CASE("compiler applies Godot-compatible numeric and builtin conversions") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("commercial_conversions.gd",
                                         "extends Sprite2D\n"
                                         "func configure(bounds: Vector4) -> int:\n"
                                         "    self_modulate = \"bcbcbc\"\n"
                                         "    return randi_range(bounds.x, bounds.y)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("static_cast<godot::Color>(gdpp::runtime::to_variant("
                                    "_gdpp_assignment_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::String(\"bcbcbc\")") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<int64_t>") != std::string::npos);
}

TEST_CASE("compiler covers strict and explicit Godot conversion families end to end") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile(
        "conversion_families.gd",
        "func convert(path: NodePath, values: Array, "
        "strings: PackedStringArray, vector_i: Vector2i, rect_i: Rect2i, "
        "basis: Basis, projection: Projection) -> Array:\n"
        "    var text: String = path\n"
        "    var restored_path: NodePath = text\n"
        "    var vector: Vector2 = vector_i\n"
        "    var rect: Rect2 = rect_i\n"
        "    var rotation: Quaternion = basis\n"
        "    var transform: Transform3D = projection\n"
        "    var packed: PackedInt64Array = values\n"
        "    var unpacked: Array = strings\n"
        "    var parse_source: String = \"42\"\n"
        "    var parsed: int = parse_source as int\n"
        "    return [text, restored_path, vector, rect, rotation, transform, packed, "
        "unpacked, parsed]\n");
    const auto invalid =
        compiler.compile("invalid_casts.gd", "extends Node\n"
                                             "func reject() -> void:\n"
                                             "    var vector = \"not a vector\" as Vector2\n"
                                             "    var text = Node.new() as String\n"
                                             "    var dictionary = [] as Dictionary\n"
                                             "    var nonnullable = null as String\n"
                                             "    print(vector, text, dictionary, nonnullable)\n");

    REQUIRE(valid.success);
    REQUIRE(valid.unit.source.find("static_cast<godot::String>(gdpp::runtime::to_variant(path))") !=
            std::string::npos);
    REQUIRE(valid.unit.source.find("gdpp::runtime::explicit_variant_cast<int64_t>("
                                   "gdpp::runtime::to_variant(parse_source), "
                                   "godot::Variant::INT, "
                                   "gdpp::runtime::ScriptSourceLocation{") != std::string::npos);
    REQUIRE(valid.unit.source.find(
                "gdpp::runtime::strict_packed_array_storage<godot::PackedInt64Array>"
                "(gdpp::runtime::to_variant(values), gdpp::runtime::ScriptSourceLocation{") !=
            std::string::npos);
    REQUIRE(!invalid.success);
    REQUIRE_EQ(std::count_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS4075"; }),
               std::ptrdiff_t{4});
}

TEST_CASE("compiler rejects analyzer-only explicit casts without runtime constructors") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("runtime_cast_failures.gd",
                         "func reject(integer: int, items: Array, vector: Vector2) -> void:\n"
                         "    var integer_text = integer as String\n"
                         "    var array_text = items as String\n"
                         "    var vector_text = vector as String\n"
                         "    print(integer_text, array_text, vector_text)\n");

    REQUIRE(!result.success);
    REQUIRE_EQ(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS4156"; }),
               std::ptrdiff_t{3});
}

TEST_CASE("compiler rejects Object values retained behind typed RID locals") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("rid_storage.gd", "func reject(value: Object) -> RID:\n"
                                                           "    var handle: RID = value\n"
                                                           "    return value\n");

    REQUIRE(!result.success);
    REQUIRE_EQ(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS4157"; }),
               std::ptrdiff_t{2});
}

TEST_CASE("compiler applies strict conversion rules to reduced constant casts") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("constant_casts.gd", "const SCRIPT_TEXT: Variant = \"42\"\n"
                                              "func reject() -> void:\n"
                                              "    const local_text: Variant = \"24\"\n"
                                              "    var literal = \"42\" as int\n"
                                              "    var script_named = SCRIPT_TEXT as int\n"
                                              "    var local_named = local_text as int\n"
                                              "    print(literal, script_named, local_named)\n");

    REQUIRE(!result.success);
    REQUIRE_EQ(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS4075"; }),
               std::ptrdiff_t{3});
}

TEST_CASE("compiler preserves Godot runtime typed storage failure boundaries") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "typed_storage_failures.gd",
        "func reject(plain: Array, packed: PackedInt64Array, "
        "dictionary: Dictionary, integers: Array[int]) -> void:\n"
        "    var from_plain: Array[int] = plain\n"
        "    var from_packed: Array[int] = packed\n"
        "    var from_dictionary: Dictionary[String, int] = dictionary\n"
        "    var from_cast: Array[float] = integers as Array[float]\n"
        "    var inferred := integers as Array[float]\n"
        "    print(from_plain, from_packed, from_dictionary, from_cast, inferred)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_typed_storage<godot::TypedArray<int64_t>>") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_typed_storage<godot::TypedDictionary<godot::String, "
                "int64_t>>") != std::string::npos);
}

TEST_CASE("compiler enforces dynamic typed storage through exact Godot metadata") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dynamic_typed_storage.gd",
                         "func restore_array(value: Variant) -> Array[int]:\n"
                         "    return value\n"
                         "func restore_dictionary(value: Variant) -> Dictionary[String, int]:\n"
                         "    return value\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_typed_storage<godot::TypedArray<int64_t>>") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::strict_typed_storage<godot::TypedDictionary<godot::String, "
                "int64_t>>") != std::string::npos);
}

TEST_CASE("typed Array assign accepts a differently typed Array conversion source") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("typed_array_assign.gd", "extends Node\n"
                                                  "var children: Array[Node2D] = []\n"
                                                  "func collect(values: Array[Node]) -> void:\n"
                                                  "    children.assign(values)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(".assign(") != std::string::npos);
}

TEST_CASE("compiler guards dynamic explicit casts with the runtime source type") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dynamic_explicit_cast.gd", "func parse(value: Variant) -> int:\n"
                                                     "    return value as int\n"
                                                     "func text(value: Variant) -> String:\n"
                                                     "    return value as String\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::explicit_variant_cast<int64_t>("
                                    "gdpp::runtime::to_variant(value), "
                                    "godot::Variant::INT, "
                                    "gdpp::runtime::ScriptSourceLocation{") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::explicit_variant_cast<godot::String>("
                                    "gdpp::runtime::to_variant(value), "
                                    "godot::Variant::STRING, "
                                    "gdpp::runtime::ScriptSourceLocation{") != std::string::npos);
}

TEST_CASE("compiler infers native Godot virtual signatures and escapes C++ keywords") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("virtuals.gd", "extends Node\n"
                                                        "func _ready():\n"
                                                        "    pass\n"
                                                        "func _process(delta):\n"
                                                        "    print(delta)\n"
                                                        "func _input(event):\n"
                                                        "    print(event)\n"
                                                        "func throw(value):\n"
                                                        "    print(value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("virtual void _ready() override") != std::string::npos);
    REQUIRE(
        result.unit.header.find("virtual void _process(double _gdpp_engine_argument_0) override") !=
        std::string::npos);
    REQUIRE(result.unit.header.find("virtual void _input(const godot::Ref<godot::InputEvent>& "
                                    "_gdpp_engine_argument_0) override") != std::string::npos);
    REQUIRE(result.unit.header.find("virtual void _gdpp_virtual_impl__process(double delta)") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("this->_gdpp_virtual_impl__process(") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_id_7468726f77(godot::Variant value)") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("D_METHOD(\"_ready\"") == std::string::npos);
    REQUIRE(result.unit.source.find("D_METHOD(\"_process\"") == std::string::npos);
    REQUIRE(result.unit.source.find("D_METHOD(\"_input\"") == std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"throw\")") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_variant_call__gdpp_id_7468726f77") != std::string::npos);
}

TEST_CASE("compiler adapts flexible GDScript virtual signatures to the exact engine ABI") {
    const gdpp::Compiler compiler;
    const auto flexible = compiler.compile(
        "flexible_virtual.gd", "extends Node\n"
                               "func _process(delta: Variant = 0.0, context = null) -> void:\n"
                               "    if context == null:\n"
                               "        print(delta)\n"
                               "func invoke() -> void:\n"
                               "    _process()\n");
    const auto scalar_abi =
        compiler.compile("scalar_virtual.gd", "extends Mesh\n"
                                              "func _surface_get_format(index: int) -> int:\n"
                                              "    return index\n");
    const auto raw_pointer = compiler.compile(
        "raw_pointer_virtual.gd", "extends AudioEffectInstance\n"
                                  "func _process(source, destination, frame_count) -> void:\n"
                                  "    pass\n");

    REQUIRE(flexible.success);
    REQUIRE(flexible.unit.header.find(
                "virtual void _process(double _gdpp_engine_argument_0) override") !=
            std::string::npos);
    REQUIRE(flexible.unit.header.find(
                "virtual void _gdpp_virtual_impl__process(godot::Variant "
                "_gdpp_argument_delta = gdpp::runtime::default_argument(), godot::Variant "
                "_gdpp_argument_context = gdpp::runtime::default_argument())") !=
            std::string::npos);
    REQUIRE(flexible.unit.source.find("this->_gdpp_virtual_impl__process("
                                      "gdpp::runtime::to_variant(_gdpp_engine_argument_0), "
                                      "gdpp::runtime::default_argument())") != std::string::npos);
    REQUIRE(flexible.unit.source.find("_gdpp_virtual_impl__process(") != std::string::npos);

    REQUIRE(scalar_abi.success);
    REQUIRE(scalar_abi.unit.header.find(
                "virtual uint32_t _surface_get_format(int32_t _gdpp_engine_argument_0) const "
                "override") != std::string::npos);
    REQUIRE(scalar_abi.unit.source.find("return static_cast<uint32_t>(") != std::string::npos);

    REQUIRE(!raw_pointer.success);
    REQUIRE(std::any_of(raw_pointer.diagnostics.begin(), raw_pointer.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4118"; }));
}

TEST_CASE("compiler preserves character scalar ABI and rejects native pointer calls") {
    const gdpp::Compiler compiler;
    gdpp::CompileOptions latest_options;
    latest_options.target_version = gdpp::GodotVersion::v4_7;
    const auto character = compiler.compile(
        "character_api.gd", "extends Node\n"
                            "func contains_character(font: Font, value: int) -> bool:\n"
                            "    return font.has_char(value)\n");
    const auto pointer =
        compiler.compile("pointer_api.gd",
                         "extends Node\n"
                         "func load_native(manager: GDExtensionManager, path: String) -> int:\n"
                         "    return manager.load_extension_from_function(path, null)\n",
                         latest_options);

    REQUIRE(character.success);
    REQUIRE(character.unit.source.find("static_cast<char32_t>(") != std::string::npos);
    REQUIRE(!pointer.success);
    REQUIRE(std::any_of(pointer.diagnostics.begin(), pointer.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4118"; }));
}

TEST_CASE("compiler preserves coroutine state behind every engine virtual return ABI") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("async_virtuals.gd",
                                         "extends Control\n"
                                         "signal resumed\n"
                                         "func _process(delta: float) -> void:\n"
                                         "    await resumed\n"
                                         "    print(delta)\n"
                                         "func _get_drag_data(at_position: Vector2) -> Variant:\n"
                                         "    await resumed\n"
                                         "    return at_position\n"
                                         "func _get_tooltip(at_position: Vector2) -> String:\n"
                                         "    await resumed\n"
                                         "    return str(at_position)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find(
                "virtual godot::Variant _gdpp_virtual_impl__process(double delta)") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("virtual godot::Variant _gdpp_virtual_impl__get_drag_data(") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("virtual godot::Variant _gdpp_virtual_impl__get_tooltip(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(this->_gdpp_virtual_impl__process(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("const godot::Variant _gdpp_virtual_result = "
                                    "this->_gdpp_virtual_impl__get_drag_data(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(
                "validate_virtual_return(_gdpp_virtual_result, godot::Variant::STRING") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::begin_coroutine(this)") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_coroutine_state, _gdpp_resume_") != std::string::npos);
}

TEST_CASE("compiler emits injective ASCII names for Unicode and C++ identifiers") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "unicode_names.gd", "extends Node\n"
                            "enum _Anim { FLOOR, AIR }\n"
                            "enum { GROUND }\n"
                            "var template: int = 1\n"
                            "var _gdpp_id_74656d706c617465: int = 2\n"
                            "var _gdpp_enum_GROUND: int = 9\n"
                            "var \xcf\x80: int = 3\n"
                            "var e\xcc\x81: int = 4\n"
                            "var \xc3\xa9: int = 5\n"
                            "func \xe8\xae\xa1\xe7\xae\x97(\xe5\x80\xbc: int) -> int:\n"
                            "    var anim := _Anim.FLOOR\n"
                            "    anim = _Anim.AIR\n"
                            "    return template + _gdpp_id_74656d706c617465 + \xcf\x80 + "
                            "e\xcc\x81 + \xc3\xa9 + \xe5\x80\xbc + anim\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("_gdpp_id_74656d706c617465") != std::string::npos);
    REQUIRE(
        result.unit.header.find("_gdpp_id_5f676470705f69645f37343635366437303663363137343635") !=
        std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_id_cf80") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_id_65cc81") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_id_c3a9") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_id_e8aea1e7ae97") != std::string::npos);
    REQUIRE(result.unit.header.find("struct _gdpp_id_5f416e696d") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_id_5f676470705f656e756d5f47524f554e44") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_id_5f416e696d::_gdpp_enum_FLOOR") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_id_5f416e696d::_gdpp_enum_AIR") != std::string::npos);
    REQUIRE(std::all_of(result.unit.header.begin(), result.unit.header.end(), [](char character) {
        return static_cast<unsigned char>(character) < 0x80U;
    }));
}

TEST_CASE("compiler encodes numeric resource stems as native identifiers") {
    const gdpp::Compiler compiler;
    const auto numeric = compiler.compile("11.gd", "func run() -> void:\n    await 42\n");

    REQUIRE(numeric.success);
    REQUIRE(numeric.unit.header.find("namespace _gdpp_id_3131_gdpp_detail") != std::string::npos);
}

TEST_CASE("compiler can namespace project native classes by a build identity") {
    gdpp::CompileOptions options;
    options.native_class_suffix = "_0123456789abcdef";
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "player.gd", "extends Node\nclass_name Player\nfunc answer() -> int:\n    return 42\n",
        options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.unit.class_name, std::string{"GDPPNative_Player_0123456789abcdef"});
    REQUIRE(result.unit.header.find("GDCLASS(GDPPNative_Player_0123456789abcdef") !=
            std::string::npos);
}

TEST_CASE("code generation uses the selected Godot API for new object types") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_7;
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "accessible.gd", "extends Node\nclass_name Accessible\nvar server: AccessibilityServer\n",
        options);

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("godot_cpp/classes/accessibility_server.hpp") !=
            std::string::npos);
    REQUIRE(result.unit.header.find(
                "gdpp::runtime::ObjectStorage<godot::AccessibilityServer> server{}") !=
            std::string::npos);
}

TEST_CASE("compiler generates typed named and anonymous enum constants") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "state_machine.gd", "extends Node\n"
                            "class_name StateMachine\n"
                            "enum State { IDLE, WALK = 4, RUN = WALK * 2 }\n"
                            "enum { DEFAULT_LIVES = 3, MAX_LIVES = DEFAULT_LIVES + 2 }\n"
                            "@export var state: State = State.RUN\n"
                            "func choose(value: State) -> State:\n"
                            "    return value\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("struct State") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_enum_RUN = 8") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_enum_MAX_LIVES = 5") != std::string::npos);
    REQUIRE(result.unit.header.find("int64_t choose(int64_t value)") != std::string::npos);
    REQUIRE(result.unit.source.find("State::_gdpp_enum_RUN") != std::string::npos);
    REQUIRE(result.unit.source.find("bind_integer_constant") != std::string::npos);
    REQUIRE(result.unit.source.find("IDLE:0,WALK:4,RUN:8") != std::string::npos);
}

TEST_CASE("compiler preserves canonical numeric raw triple and Unicode literals") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("literal_contract.gd", R"gd(extends Node
enum Values { HEX = 0xff_00, BINARY = 0b1010_0101, DECIMAL = 12_345 }
const LEADING = .5
const TRAILING = 4.
const EXPONENT = 1_2.5_0e+1_0
const OVERFLOW = 1e400
const UNDERFLOW = 1e-4000
const NOT_A_NUMBER = 0e400
const ESCAPED = "\a\b\f\v\u0041\U01F600\uD83D\uDE00"
const NUL = "A\u0000B"
const RAW = r"\n\"quoted\"\\path"
const TRIPLE = """first
"quoted"
last"""
)gd");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("_gdpp_enum_HEX = 65280") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_enum_BINARY = 165") != std::string::npos);
    REQUIRE(result.unit.header.find("_gdpp_enum_DECIMAL = 12345") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_constant_LEADING_storage() = 0.5") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_constant_TRAILING_storage() = 4.0") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("12.50e+10") != std::string::npos);
    REQUIRE(result.unit.source.find("Math_INF") != std::string::npos);
    REQUIRE(result.unit.source.find("Math_NAN") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_constant_UNDERFLOW_storage() = 0.0") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("0xff_00") == std::string::npos);
    REQUIRE(result.unit.source.find("0b1010_0101") == std::string::npos);
    REQUIRE(result.unit.source.find("\\a\\b\\f\\vA") != std::string::npos);
    REQUIRE(result.unit.source.find(u8"😀😀") != std::string::npos);
    REQUIRE(result.unit.source.find(u8"A�B") != std::string::npos);
    REQUIRE(result.unit.source.find("\\000") == std::string::npos);
    REQUIRE(result.unit.source.find("\\\\n\\\\\\\"quoted\\\\\\\"\\\\\\\\path") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("first\\n\\\"quoted\\\"\\nlast") != std::string::npos);
    REQUIRE(result.unit.source.find('\a') == std::string::npos);
    REQUIRE(result.unit.source.find('\b') == std::string::npos);
    REQUIRE(result.unit.source.find('\f') == std::string::npos);
    REQUIRE(result.unit.source.find('\v') == std::string::npos);
    REQUIRE(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                          [](const gdpp::Diagnostic& diagnostic) {
                              return diagnostic.code == "GDS1007";
                          }) == std::ptrdiff_t{1});
    REQUIRE(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                          [](const gdpp::Diagnostic& diagnostic) {
                              return diagnostic.code == "GDS1008";
                          }) == std::ptrdiff_t{1});
}

TEST_CASE("compiler rejects malformed literal source transactionally") {
    const gdpp::Compiler compiler;
    const auto numeric = compiler.compile("bad_number.gd", "var value := 0b102\n");
    const auto escape = compiler.compile("bad_escape.gd", "var value := \"\\q\"\n");
    const auto unicode = compiler.compile("bad_unicode.gd", "var value := \"\\uD800\"\n");

    REQUIRE(!numeric.success);
    REQUIRE(!escape.success);
    REQUIRE(!unicode.success);
    REQUIRE(numeric.unit.source.empty());
    REQUIRE(escape.unit.source.empty());
    REQUIRE(unicode.unit.source.empty());
}

TEST_CASE("compiler reports all recovered syntax failures without emitting partial units") {
    const auto result =
        gdpp::Compiler{}.compile("independent_failures.gd", "var first = 1, second = 2\n"
                                                            "signal changed() trailing\n"
                                                            "enum Kind { A } trailing\n"
                                                            "func calculate() -> int:\n"
                                                            "    var local = 3, other = 4\n"
                                                            "    return local\n"
                                                            "var recovered := 5\n");

    REQUIRE(!result.success);
    REQUIRE_EQ(result.diagnostics.size(), std::size_t{4});
    REQUIRE(result.unit.script_class_name.empty());
    REQUIRE(result.unit.class_name.empty());
    REQUIRE(result.unit.header_file_name.empty());
    REQUIRE(result.unit.source_file_name.empty());
    REQUIRE(result.unit.symbol_file_name.empty());
    REQUIRE(result.unit.header.empty());
    REQUIRE(result.unit.source.empty());
    REQUIRE(result.unit.symbol_map.empty());
    REQUIRE(result.unit.symbols.empty());
    REQUIRE(result.unit.inner_class_names.empty());
    REQUIRE(result.unit.abstract_inner_class_names.empty());
    REQUIRE(!result.unit.icon_path.has_value());
}

TEST_CASE("compiler omits ambiguous native reflection entries for duplicate enum members") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("duplicate_native_enum_members.gd", "extends Node\n"
                                                             "enum First { NONE, A }\n"
                                                             "enum Second { NONE, B }\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("\"NONE\"") == std::string::npos);
    REQUIRE(result.unit.source.find("\"A\"") != std::string::npos);
    REQUIRE(result.unit.source.find("\"B\"") != std::string::npos);
}

TEST_CASE("compiler resolves inherited engine constants and qualified class enums") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_5;
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("engine_constants.gd",
                                         "extends Camera3D\n"
                                         "func transformed(what: int) -> bool:\n"
                                         "    return what == NOTIFICATION_TRANSFORM_CHANGED\n"
                                         "func go_back(what: int) -> bool:\n"
                                         "    return what == Node.NOTIFICATION_WM_GO_BACK_REQUEST\n"
                                         "func inherited_mode() -> int:\n"
                                         "    return Node.ProcessMode.PROCESS_MODE_INHERIT\n",
                                         options);

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("find_engine_singleton") == std::string::npos);
    REQUIRE(result.unit.source.find(" = 2000;") != std::string::npos);
    REQUIRE(result.unit.source.find(" = 1007;") != std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_return_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_integer_left_") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<int64_t>(gdpp::runtime::to_variant(0))") !=
            std::string::npos);
}

TEST_CASE("compiler rejects invalid enum declarations and members") {
    const gdpp::Compiler compiler;
    const auto duplicate = compiler.compile("duplicate.gd", "enum State { IDLE, IDLE }\n");
    const auto nonconstant =
        compiler.compile("nonconstant.gd", "var value: int = 2\nenum State { IDLE = value }\n");
    const auto missing =
        compiler.compile("missing.gd", "enum State { IDLE }\nvar state: State = State.MISSING\n");

    REQUIRE(!duplicate.success);
    REQUIRE(!nonconstant.success);
    REQUIRE(!missing.success);
    REQUIRE(duplicate.unit.header.empty());
}

TEST_CASE("compiler accepts multiline enums and contextual keyword iterators") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("contextual-layout.gd", "enum DialogState\n"
                                                 "{\n"
                                                 "    OFF = 0,\n"
                                                 "    PLAYING = 1\n"
                                                 "}\n"
                                                 "func collect(values: Array) -> Array:\n"
                                                 "    var result: Array = []\n"
                                                 "    for match in values:\n"
                                                 "        result.append(match)\n"
                                                 "    return result\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("_gdpp_enum_PLAYING = 1") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant match =") != std::string::npos);
}

TEST_CASE("compiler preserves named enums as read-only Dictionary values") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("enum-dictionary.gd", "enum TokenType { IDENTIFIER = 2, NUMBER = 7 }\n"
                                               "func metadata() -> Dictionary:\n"
                                               "    return TokenType\n"
                                               "func names() -> Array:\n"
                                               "    return TokenType.keys()\n"
                                               "func name_for(value: int):\n"
                                               "    return TokenType.find_key(value)\n"
                                               "func has_name(value: String) -> bool:\n"
                                               "    return TokenType.has(value)\n"
                                               "func count() -> int:\n"
                                               "    return TokenType.size()\n");
    const auto mutation =
        compiler.compile("enum-mutation.gd", "enum TokenType { IDENTIFIER }\n"
                                             "func mutate() -> void:\n"
                                             "    TokenType.clear()\n"
                                             "    TokenType[\"IDENTIFIER\"] = 2\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("static const godot::Dictionary& _gdpp_dictionary()") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("result[godot::String(\"IDENTIFIER\")] = int64_t{2}") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("result.make_read_only()") != std::string::npos);
    REQUIRE(result.unit.source.find("TokenType::_gdpp_dictionary()") != std::string::npos);
    REQUIRE(!mutation.success);
    REQUIRE(std::any_of(mutation.diagnostics.begin(), mutation.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4165"; }));
    REQUIRE(std::any_of(mutation.diagnostics.begin(), mutation.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4110"; }));
}

TEST_CASE("compiler generates single-evaluation match control flow") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("matcher.gd", "extends Node\n"
                                       "enum State { IDLE, WALK = 4, RUN = 8 }\n"
                                       "func classify(value: int) -> String:\n"
                                       "    match value:\n"
                                       "        State.IDLE, State.WALK:\n"
                                       "            return \"slow\"\n"
                                       "        var captured when captured == State.RUN:\n"
                                       "            return \"run\"\n"
                                       "        _:\n"
                                       "            return \"unknown\"\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("const auto _gdpp_match_value_0 = value") != std::string::npos);
    REQUIRE(result.unit.source.find("bool _gdpp_match_done_0 = false") != std::string::npos);
    REQUIRE(result.unit.source.find("State::_gdpp_enum_IDLE") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant _gdpp_match_bind_") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "int64_t captured = gdpp::runtime::strict_builtin_storage<int64_t>(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("if (static_cast<bool>(([&]() -> bool") != std::string::npos);
    REQUIRE(result.unit.source.find(" = State::_gdpp_enum_RUN;") != std::string::npos);
    REQUIRE(result.unit.source.find("    return {};\n}") != std::string::npos);
}

TEST_CASE("compiler accepts identifier patterns and warns about unreachable match branches") {
    const gdpp::Compiler compiler;
    const auto live_identifier =
        compiler.compile("nonconstant_match.gd", "func test(value: int) -> int:\n"
                                                 "    match value:\n"
                                                 "        value:\n"
                                                 "            return 1\n"
                                                 "        _:\n"
                                                 "            return 0\n");
    const auto composite_live =
        compiler.compile("composite_match.gd", "func test(value: int) -> int:\n"
                                               "    match value:\n"
                                               "        value + 1:\n"
                                               "            return 1\n"
                                               "        _:\n"
                                               "            return 0\n");
    const auto unreachable =
        compiler.compile("unreachable_match.gd", "func test(value: int) -> int:\n"
                                                 "    match value:\n"
                                                 "        _:\n"
                                                 "            return 0\n"
                                                 "        1:\n"
                                                 "            return 1\n");

    REQUIRE(live_identifier.success);
    REQUIRE(!composite_live.success);
    REQUIRE(unreachable.success);
    REQUIRE(composite_live.unit.source.empty());
    REQUIRE(std::any_of(unreachable.diagnostics.begin(), unreachable.diagnostics.end(),
                        [](const gdpp::Diagnostic& diagnostic) {
                            return diagnostic.severity == gdpp::DiagnosticSeverity::warning &&
                                   diagnostic.code == "GDS4044";
                        }));
}

TEST_CASE("dynamic match values use the Variant compatibility runtime") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dynamic_match.gd", "func classify(value: Variant) -> String:\n"
                                             "    match value:\n"
                                             "        \"ready\":\n"
                                             "            return \"ok\"\n"
                                             "        _:\n"
                                             "            return \"other\"\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::binary(godot::Variant::OP_EQUAL") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<bool>") != std::string::npos);
}

TEST_CASE("compiler lowers dynamic calls properties and keyed access through the runtime") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "dynamic_access.gd",
        "func invoke(target: Variant, value: int) -> Variant:\n"
        "    return target.answer(value, value + 1)\n"
        "func read_property(target: Variant) -> Variant:\n"
        "    return target.score\n"
        "func write_property(target: Variant, value: Variant) -> void:\n"
        "    target.score = value\n"
        "func increment_property(target: Variant, value: Variant) -> void:\n"
        "    target.score += value\n"
        "func read_key(target: Variant, key: Variant) -> Variant:\n"
        "    return target[key]\n"
        "func write_key(target: Variant, key: Variant, value: Variant) -> void:\n"
        "    target[key] = value\n"
        "func increment_key(target: Variant, key: Variant, value: Variant) -> void:\n"
        "    target[key] += value\n"
        "class Payload:\n"
        "    func dynamic_surface() -> Variant:\n"
        "        self.missing_method()\n"
        "        return self.missing_property\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::call_dynamic") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"answer\")") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_named") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"score\")") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_key") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_key") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_dynamic_current_") != std::string::npos);
    REQUIRE(result.unit.source.find("target.answer(") == std::string::npos);
    REQUIRE(result.unit.source.find("target.score") == std::string::npos);
}

TEST_CASE("compiler lowers Dictionary named access through its keyed native ABI") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dictionary_named_access.gd",
                         "func mutate(values: Dictionary, increment: int) -> Variant:\n"
                         "    values.score += increment\n"
                         "    values.label = \"ready\"\n"
                         "    return values.score\n"
                         "func source() -> Dictionary:\n"
                         "    return {}\n"
                         "func read_source() -> Variant:\n"
                         "    return source().missing\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_dictionary_target_") != std::string::npos);
    REQUIRE(result.unit.source.find("static const godot::StringName _gdpp_dictionary_read_key_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_dictionary_get_named(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("if (script_function_failed())") != std::string::npos);
    REQUIRE(result.unit.source.find(").get(") == std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant _gdpp_dictionary_current_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::checked_dictionary_set_named(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::unchecked_dictionary_set_named(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant &_gdpp_dictionary_slot_") == std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") == std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_named") == std::string::npos);
    const auto read_source = result.unit.source.find("::read_source(");
    const auto receiver = result.unit.source.find("_gdpp_dictionary_read_receiver_", read_source);
    const auto receiver_fault = result.unit.source.find("if (script_function_failed())", receiver);
    const auto receiver_lookup =
        result.unit.source.find("gdpp::runtime::checked_dictionary_get_named(", receiver_fault);
    REQUIRE(read_source != std::string::npos);
    REQUIRE(receiver != std::string::npos);
    REQUIRE(receiver_fault != std::string::npos);
    REQUIRE(receiver_fault < receiver_lookup);
}

TEST_CASE("compiler only updates proven local Dictionary slots in place") {
    const gdpp::Compiler compiler;
    const auto local =
        compiler.compile("local_dictionary_slot.gd", "func mutate(increment: int) -> int:\n"
                                                     "    var values := {\"score\": 1}\n"
                                                     "    values.score = 2\n"
                                                     "    values.score += increment\n"
                                                     "    return values.score\n");
    REQUIRE(local.success);
    REQUIRE(local.unit.source.find("godot::Variant &_gdpp_dictionary_slot_") != std::string::npos);
    REQUIRE(local.unit.source.find("const godot::Variant "
                                   "_gdpp_proven_dictionary_read_key_") != std::string::npos);
    REQUIRE(local.unit.source.find("checked_dictionary_get_named(") == std::string::npos);
    REQUIRE(local.unit.source.find("unchecked_dictionary_set_named(") == std::string::npos);
    const auto compound = local.unit.source.find("gdpp::runtime::compound_assign_integer(");
    REQUIRE(compound != std::string::npos);
    REQUIRE(local.unit.source.find("gdpp::runtime::ScriptSourceLocation{_gdpp_source_path, 4, ",
                                   compound) != std::string::npos);

    const auto require_checked = [&](const std::string& path, const std::string& source) {
        const auto result = compiler.compile(path, source);
        REQUIRE(result.success);
        REQUIRE(result.unit.source.find("godot::Variant &_gdpp_dictionary_slot_") ==
                std::string::npos);
        REQUIRE(result.unit.source.find("_gdpp_proven_dictionary_read_key_") == std::string::npos);
        REQUIRE(result.unit.source.find("checked_dictionary_get_named(") != std::string::npos);
        REQUIRE(result.unit.source.find("unchecked_dictionary_set_named(") != std::string::npos);
    };
    require_checked("dictionary_parameter.gd",
                    "func mutate(values: Dictionary, increment: int) -> void:\n"
                    "    values.score += increment\n");
    require_checked("escaped_dictionary.gd", "func consume(_values: Dictionary) -> void:\n"
                                             "    pass\n"
                                             "func mutate(increment: int) -> void:\n"
                                             "    var values := {\"score\": 1}\n"
                                             "    consume(values)\n"
                                             "    values.score += increment\n");
    require_checked("aliased_dictionary.gd", "func mutate(increment: int) -> void:\n"
                                             "    var values := {\"score\": 1}\n"
                                             "    var alias := values\n"
                                             "    alias.score = 2\n"
                                             "    values.score += increment\n");
    require_checked("readonly_dictionary.gd", "func mutate(increment: int) -> void:\n"
                                              "    var values := {\"score\": 1}\n"
                                              "    values.make_read_only()\n"
                                              "    values.score += increment\n");
    require_checked("unknown_dictionary_key.gd", "func mutate(increment: int) -> void:\n"
                                                 "    var values := {\"score\": 1}\n"
                                                 "    values.other += increment\n");
    require_checked("reassigned_dictionary.gd", "func mutate(increment: int) -> void:\n"
                                                "    var values := {\"score\": 1}\n"
                                                "    values = {\"score\": 2}\n"
                                                "    values.score += increment\n");
    require_checked("subscripted_dictionary.gd", "func mutate(increment: int) -> void:\n"
                                                 "    var values := {\"score\": 1}\n"
                                                 "    var observed := values[\"score\"]\n"
                                                 "    values.score += increment + observed\n");
    require_checked("captured_dictionary.gd", "func mutate(increment: int) -> void:\n"
                                              "    var values := {\"score\": 1}\n"
                                              "    var callback := func() -> void:\n"
                                              "        values.score += 1\n"
                                              "    callback.call()\n"
                                              "    values.score += increment\n");
    require_checked("typed_local_dictionary.gd",
                    "func mutate(increment: int) -> void:\n"
                    "    var values: Dictionary[String, int] = {\"score\": 1}\n"
                    "    values.score += increment\n");
}

TEST_CASE("typed Dictionary named access enforces its key and value contracts") {
    const gdpp::Compiler compiler;
    const auto valid =
        compiler.compile("typed_dictionary_named_access.gd",
                         "func read(values: Dictionary[String, int]) -> int:\n"
                         "    return values.score\n"
                         "func write(values: Dictionary[StringName, int], value: int) -> void:\n"
                         "    values.score = value\n");
    const auto invalid = compiler.compile("invalid_typed_dictionary_named_access.gd",
                                          "func write(values: Dictionary[int, int]) -> void:\n"
                                          "    values.score = 1\n");

    REQUIRE(valid.success);
    REQUIRE(valid.unit.source.find("checked_dictionary_get_named(") != std::string::npos);
    REQUIRE(valid.unit.source.find("strict_builtin_storage<int64_t>(") != std::string::npos);
    REQUIRE(!invalid.success);
    REQUIRE(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4002"; }));
}

TEST_CASE("dynamic named access preserves Dictionary dot syntax across Variant boundaries") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dynamic_dictionary_named_access.gd",
                         "func read_login(response: Variant) -> Variant:\n"
                         "    return response.uuid\n"
                         "func update_login(response: Variant, token: String) -> Variant:\n"
                         "    response.uuid = token\n"
                         "    response.profile.score += 1\n"
                         "    return response.profile.score\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named(response") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_named") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"uuid\")") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"profile\")") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"score\")") != std::string::npos);
}

TEST_CASE("dynamic nested value assignments write every changed value back to its owner") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dynamic_nested_assignment.gd",
                         "extends Node\n"
                         "var local_variant: Variant = Vector2.ZERO\n"
                         "func mutate(target: Variant, records: Array, index: int) -> void:\n"
                         "    target.position.y -= 1.0\n"
                         "    target.transform.origin.x = 4.0\n"
                         "    local_variant.x = 3.0\n"
                         "    records[index].tint.a = 0.25\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;

    // Node2D.position is a Vector2 copy. The component update must be followed by a write of the
    // changed Vector2 into position, and a Variant root must then be stored back as well.
    REQUIRE(source.find("godot::StringName(\"y\")") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"position\"), _gdpp_dynamic_child_") !=
            std::string::npos);
    REQUIRE(source.find("_gdpp_dynamic_root_") != std::string::npos);
    REQUIRE(source.find("_gdpp_assignment_receiver_0 = _gdpp_dynamic_root_") != std::string::npos);

    // Deeper Transform3D.origin.x-style chains must reverse through every value layer.
    REQUIRE(source.find("godot::StringName(\"origin\"), _gdpp_dynamic_child_") !=
            std::string::npos);
    REQUIRE(source.find("godot::StringName(\"transform\"), _gdpp_dynamic_child_") !=
            std::string::npos);

    // Variant-held value roots and Array[index] records exercise the two distinct final stores.
    REQUIRE(source.find("_gdpp_assignment_receiver_2 = _gdpp_dynamic_root_") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::set_key(_gdpp_dynamic_root_") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"tint\"), _gdpp_dynamic_child_") != std::string::npos);
}

TEST_CASE("plain script variables remain dynamically visible on generated native objects") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("plain_dynamic_property.gd",
                                         "extends Node\n"
                                         "var open_bag: bool = false\n"
                                         "func read_other(target: Variant) -> Variant:\n"
                                         "    return target.open_bag\n"
                                         "func write_other(target: Variant, value: bool) -> void:\n"
                                         "    target.open_bag = value\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_get_open_bag") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_set_open_bag") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_USAGE_SCRIPT_VARIABLE") != std::string::npos);
}

TEST_CASE("compiler lowers Variant iteration and Callable calls with ordered temporaries") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "dynamic_protocols.gd", "func sum(values: Variant) -> int:\n"
                                "    var total: int = 0\n"
                                "    for value in values:\n"
                                "        if value == 2:\n"
                                "            continue\n"
                                "        total += value\n"
                                "    return total\n"
                                "func invoke(callback: Callable, value: int) -> Variant:\n"
                                "    return callback.call(value)\n"
                                "func invoke_dynamic(callback: Variant, value: int) -> Variant:\n"
                                "    return callback.call(value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_init") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_next") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::iter_get") != std::string::npos);
    REQUIRE(result.unit.source.find("for (bool _gdpp_dynamic_available_") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant value =") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_callable_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::call_dynamic") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::StringName(\"call\")") != std::string::npos);
}

TEST_CASE("compiler retains local lambda adapters for direct native calls") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "local_callable.gd", "func invoke(value: int) -> int:\n"
                             "    var operation := func(item: int) -> int: return item * 3 + 1\n"
                             "    return operation.call(value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("auto operation = gdpp::runtime::make_local_callable<") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("[=](const auto &_gdpp_lambda_arguments_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::local_callable_typed_argument<int64_t, 0>(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(") mutable -> godot::Variant {") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Callable operation") == std::string::npos);
}

TEST_CASE("local lambda ABI keeps dynamic and structural parameters on strict Variant boundaries") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "local_callable_boundaries.gd",
        "func make() -> Callable:\n"
        "    return func(number: int, dynamic: Variant, values: Array[int]) -> int:\n"
        "        return number + int(dynamic) + values[0]\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("make_local_callable<3, 3, false>") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::local_callable_typed_argument<int64_t, 0>(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::local_callable_argument<1>(") !=
            std::string::npos);
    const auto structural = result.unit.source.find("gdpp::runtime::local_callable_argument<2>(");
    REQUIRE(structural != std::string::npos);
    REQUIRE(result.unit.source.rfind("gdpp::runtime::strict_typed_storage<", structural) !=
            std::string::npos);
}

TEST_CASE("compiler emits ordered portable operations for typed integers") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "integer_operators.gd",
        "func calculate(left: int, right: int) -> Array:\n"
        "    return [left + right, left - right, left * right, left / right, left % right, "
        "left << right, left >> right, left & right, left | right, left ^ right, "
        "left < right, -left, ~right]\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <gdpp/numeric/integer_semantics.hpp>") !=
            std::string::npos);
    for (const auto* helper :
         {"gdpp::integer::add(", "gdpp::integer::subtract(", "gdpp::integer::multiply(",
          "gdpp::runtime::integer_divide(", "gdpp::runtime::integer_modulo(",
          "gdpp::integer::shift_left(", "gdpp::integer::shift_right(", "gdpp::integer::bit_and(",
          "gdpp::integer::bit_or(", "gdpp::integer::bit_xor(", "gdpp::integer::negate(",
          "gdpp::integer::bit_not("}) {
        REQUIRE(result.unit.source.find(helper) != std::string::npos);
    }
    const auto left = result.unit.source.find("const int64_t _gdpp_integer_left_");
    const auto right = result.unit.source.find("const int64_t _gdpp_integer_right_");
    REQUIRE(left != std::string::npos);
    REQUIRE(right != std::string::npos);
    REQUIRE(left < right);
}

TEST_CASE("compiler routes typed integer compound assignments through portable operations") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "integer_compound.gd",
        "func mutate(value: int, right: int, values: Array[int], index: int) -> int:\n"
        "    value += right\n"
        "    value -= right\n"
        "    value *= right\n"
        "    value /= right\n"
        "    value %= right\n"
        "    value <<= right\n"
        "    value >>= right\n"
        "    value &= right\n"
        "    value |= right\n"
        "    value ^= right\n"
        "    value **= right\n"
        "    values[index] += right\n"
        "    return value\n");

    REQUIRE(result.success);
    for (const auto* helper :
         {"gdpp::integer::add(", "gdpp::integer::subtract(", "gdpp::integer::multiply(",
          "gdpp::runtime::integer_divide(", "gdpp::runtime::integer_modulo(",
          "gdpp::integer::shift_left(", "gdpp::integer::shift_right(", "gdpp::integer::bit_and(",
          "gdpp::integer::bit_or(", "gdpp::integer::bit_xor("}) {
        REQUIRE(result.unit.source.find(helper) != std::string::npos);
    }
    REQUIRE(result.unit.source.find("godot::Variant::OP_POWER") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_subscript_current_") != std::string::npos);
}

TEST_CASE("compiler preserves native scalar paths across dynamic boundaries") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("dynamic_scalar_fast_paths.gd",
                         "func update(values: Dictionary, callback: Callable) -> int:\n"
                         "    values.integer_value += 1\n"
                         "    values.float_value += 0.5\n"
                         "    var total: int = 0\n"
                         "    total += callback.call(2)\n"
                         "    return total + int(values.integer_value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::compound_assign_integer(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::compound_assign(") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::integer::add(") != std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_callable_argument_") != std::string::npos);
    REQUIRE(result.unit.source.find("const godot::Variant _gdpp_callable_argument_") ==
            std::string::npos);
}

TEST_CASE("compiler transfers consumed assignment snapshots without reference-backed copies") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("consumed_assignment.gd",
                         "func update(iterations: int) -> int:\n"
                         "    var dynamic: Variant = 1\n"
                         "    var text := \"gdpp\"\n"
                         "    var total := 0\n"
                         "    for index in range(iterations):\n"
                         "        dynamic = int(dynamic) + index\n"
                         "        text = text.to_upper() if (index & 1) == 0 else text.to_lower()\n"
                         "        total += int(dynamic) + text.length()\n"
                         "    return total\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("auto _gdpp_assignment_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("const auto _gdpp_assignment_value_") == std::string::npos);
    REQUIRE(result.unit.source.find("auto _gdpp_assignment_result_") != std::string::npos);
    REQUIRE(result.unit.source.find("= std::move(_gdpp_assignment_value_") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::assign_native_storage("
                                    "text, std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("explicit_variant_cast<int64_t>("
                                    "gdpp::runtime::to_variant(dynamic)") != std::string::npos);
}

TEST_CASE("compiler rejects direct Callable and unknown expression invocation") {
    const gdpp::Compiler compiler;
    const auto direct =
        compiler.compile("direct_callable.gd", "func invoke(callback: Callable) -> Variant:\n"
                                               "    return callback()\n");
    const auto unknown = compiler.compile("unknown_call.gd", "func invoke() -> Variant:\n"
                                                             "    return missing_function()\n");
    const auto expression =
        compiler.compile("expression_call.gd", "func invoke() -> Variant:\n"
                                               "    return [Callable()][0]()\n");

    REQUIRE(!direct.success);
    REQUIRE(!unknown.success);
    REQUIRE(!expression.success);
    bool found_direct = false;
    for (const auto& diagnostic : direct.diagnostics)
        found_direct = found_direct || diagnostic.code == "GDS4070";
    bool found_unknown = false;
    for (const auto& diagnostic : unknown.diagnostics)
        found_unknown = found_unknown || diagnostic.code == "GDS4071";
    bool found_expression = false;
    for (const auto& diagnostic : expression.diagnostics)
        found_expression = found_expression || diagnostic.code == "GDS4072";
    REQUIRE(found_direct);
    REQUIRE(found_unknown);
    REQUIRE(found_expression);
}

TEST_CASE("compiler generates nonrecursive Godot 4 property accessors") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("health.gd", "extends Node\n"
                                                      "@export var health: int = 100:\n"
                                                      "    set(value):\n"
                                                      "        health = value\n"
                                                      "    get:\n"
                                                      "        return health\n"
                                                      "var internal_state: int = 1:\n"
                                                      "    get:\n"
                                                      "        return internal_state\n"
                                                      "func update(value: int) -> int:\n"
                                                      "    health = value\n"
                                                      "    return health\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("int64_t _gdpp_get_health()") != std::string::npos);
    REQUIRE(result.unit.source.find("int64_t GDPPNative_Health::_gdpp_get_health()") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("health = std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_set_health(std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(" = _gdpp_get_health();") != std::string::npos);
    REQUIRE(result.unit.source.find("return health;\n    return health;") == std::string::npos);
    REQUIRE(result.unit.source.find("return _gdpp_get_health();\n    return {};") ==
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::PROPERTY_USAGE_SCRIPT_VARIABLE") != std::string::npos);
}

TEST_CASE("compiler generates validated method-bound property accessors") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("bound.gd", "extends Node\n"
                                     "var active: bool = true: set = set_active, get = is_active\n"
                                     "func set_active(value: bool) -> void:\n"
                                     "    active = value\n"
                                     "func is_active() -> bool:\n"
                                     "    return active\n"
                                     "func disable() -> bool:\n"
                                     "    active = false\n"
                                     "    return active\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("return is_active();") != std::string::npos);
    REQUIRE(result.unit.source.find("set_active(value);") != std::string::npos);
    REQUIRE(result.unit.source.find("active = std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(" = false;") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_set_active(std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(" = _gdpp_get_active();") != std::string::npos);
}

TEST_CASE("compiler preserves coroutine property accessor ABI") {
    const gdpp::Compiler compiler;
    const auto inline_accessors =
        compiler.compile("coroutine_property.gd", "extends Node\n"
                                                  "signal resumed(value)\n"
                                                  "var observed := -1\n"
                                                  "var value: int:\n"
                                                  "    get:\n"
                                                  "        return await resumed\n"
                                                  "    set(next):\n"
                                                  "        await resumed\n"
                                                  "        observed = next\n"
                                                  "func read() -> int:\n"
                                                  "    return await value\n");
    const auto bound_accessors =
        compiler.compile("bound_coroutine_property.gd", "extends Node\n"
                                                        "signal resumed(value)\n"
                                                        "var value: int: get = read_value\n"
                                                        "func read_value() -> int:\n"
                                                        "    return await resumed\n"
                                                        "func consume() -> int:\n"
                                                        "    return await value\n");
    const auto static_accessors =
        compiler.compile("static_coroutine_property.gd",
                         "class_name StaticCoroutineProperty\n"
                         "extends Node\n"
                         "static var observed := -1\n"
                         "static var value: int:\n"
                         "    get:\n"
                         "        await (Engine.get_main_loop() as SceneTree).process_frame\n"
                         "        return 44\n"
                         "    set(next):\n"
                         "        await (Engine.get_main_loop() as SceneTree).process_frame\n"
                         "        observed = next\n"
                         "func consume() -> int:\n"
                         "    StaticCoroutineProperty.value = 43\n"
                         "    return await StaticCoroutineProperty.value\n");

    REQUIRE(inline_accessors.success);
    REQUIRE(inline_accessors.unit.header.find("godot::Variant _gdpp_get_value()") !=
            std::string::npos);
    REQUIRE(inline_accessors.unit.source.find(
                "godot::Variant GDPPNative_CoroutineProperty::_gdpp_get_value()") !=
            std::string::npos);
    REQUIRE(inline_accessors.unit.source.find("gdpp::runtime::begin_coroutine(this)") !=
            std::string::npos);
    REQUIRE(inline_accessors.unit.source.find(
                "gdpp::runtime::complete_coroutine(_gdpp_property_coroutine_state") !=
            std::string::npos);
    REQUIRE(inline_accessors.unit.source.find(
                "void GDPPNative_CoroutineProperty::_gdpp_set_value") != std::string::npos);
    REQUIRE(bound_accessors.success);
    REQUIRE(bound_accessors.unit.header.find("godot::Variant _gdpp_get_value()") !=
            std::string::npos);
    REQUIRE(bound_accessors.unit.source.find("return read_value();") != std::string::npos);
    REQUIRE(static_accessors.success);
    REQUIRE(static_accessors.unit.header.find("static godot::Variant _gdpp_get_value()") !=
            std::string::npos);
    REQUIRE(static_accessors.unit.source.find("gdpp::runtime::begin_coroutine(nullptr)") !=
            std::string::npos);
    REQUIRE(static_accessors.unit.source.find(
                "GDPPNative_StaticCoroutineProperty::_gdpp_set_value(") != std::string::npos);
    REQUIRE(static_accessors.unit.source.find(
                "GDPPNative_StaticCoroutineProperty::_gdpp_get_value()") != std::string::npos);
    REQUIRE(static_accessors.unit.source.find(
                "godot::StringName(\"GDPPNative_StaticCoroutineProperty\")::_gdpp_") ==
            std::string::npos);
}

TEST_CASE("compiler isolates nested coroutine lambda continuation state") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("nested_coroutine_lambda.gd", "extends Node\n"
                                                       "signal resumed(value)\n"
                                                       "func run() -> int:\n"
                                                       "    await resumed\n"
                                                       "    var capture := func() -> int:\n"
                                                       "        return await resumed\n"
                                                       "    return await capture.call()\n");

    REQUIRE(result.success);
    const auto lambda_state = result.unit.source.find("const auto _gdpp_lambda_coroutine_state_");
    REQUIRE(lambda_state != std::string::npos);
    const auto lambda_end = result.unit.source.find("\n});", lambda_state);
    REQUIRE(lambda_end != std::string::npos);
    const auto lambda_source = result.unit.source.substr(lambda_state, lambda_end - lambda_state);
    REQUIRE(lambda_source.find(
                "return gdpp::runtime::coroutine_result(_gdpp_lambda_coroutine_state_") !=
            std::string::npos);
}

TEST_CASE("compiler rejects invalid bound property accessor signatures") {
    const gdpp::Compiler compiler;
    const auto missing = compiler.compile("missing.gd", "var value: int: get = missing_getter\n");
    const auto setter_arity = compiler.compile("arity.gd", "var value: int: set = set_value\n"
                                                           "func set_value() -> void:\n"
                                                           "    pass\n");
    const auto getter_arity =
        compiler.compile("getter_arity.gd", "var value: int: get = get_value\n"
                                            "func get_value(extra: int) -> int:\n"
                                            "    return extra\n");
    const auto static_mismatch =
        compiler.compile("static.gd", "static var value: int: set = set_value\n"
                                      "func set_value(next: int) -> void:\n"
                                      "    value = next\n");

    REQUIRE(!missing.success);
    REQUIRE(!setter_arity.success);
    REQUIRE(!getter_arity.success);
    REQUIRE(!static_mismatch.success);
}

TEST_CASE("compiler rejects malformed property accessors") {
    const gdpp::Compiler compiler;
    const auto missing_return =
        compiler.compile("getter.gd", "var value: int:\n    get:\n        pass\n");
    const auto invalid_setter_return =
        compiler.compile("setter.gd", "var value: int:\n    set(next):\n        return next\n");

    REQUIRE(!missing_return.success);
    REQUIRE(!invalid_setter_return.success);
    REQUIRE(missing_return.unit.header.empty());
}

TEST_CASE("compiler returns structured errors instead of partial output") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("broken.gd", "const answer\n");

    REQUIRE(!result.success);
    REQUIRE(!result.diagnostics.empty());
    REQUIRE(result.unit.header.empty());
}

TEST_CASE("compiler generates object and builtin type tests") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("type_tests.gd", "extends Node\n"
                                          "func is_node(value: Variant) -> bool:\n"
                                          "    return value is Node\n"
                                          "func is_string(value: Variant) -> bool:\n"
                                          "    return value is String\n"
                                          "func is_integer(value: int) -> bool:\n"
                                          "    return value is int\n"
                                          "func integer_is_node(value: int) -> bool:\n"
                                          "    return value is Node\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node>("
                                    "(value).get_validated_object()) != nullptr") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("(value).get_type() == godot::Variant::STRING") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(value), true") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(value), false") != std::string::npos);
}

TEST_CASE("compiler lowers negated type tests and checked object downcasts") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("downcast.gd", "extends Node\n"
                                                        "func node_2d(value: Node) -> Node2D:\n"
                                                        "    return value\n"
                                                        "func differs(value: Node) -> bool:\n"
                                                        "    return value is not Node2D\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node2D>(value)") !=
            std::string::npos);
    REQUIRE(
        result.unit.source.find("!((godot::Object::cast_to<godot::Node2D>(value) != nullptr))") !=
        std::string::npos);
}

TEST_CASE("semantic flow narrows type-tested values in if and while bodies") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("flow_type_tests.gd", "extends Node\n"
                                               "func object_name(value: Variant) -> String:\n"
                                               "    if value is Node:\n"
                                               "        return value.name\n"
                                               "    return \"\"\n"
                                               "func array_size(value: Variant) -> int:\n"
                                               "    while value is Array:\n"
                                               "        return value.size()\n"
                                               "    return 0\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node>") != std::string::npos);
    REQUIRE(result.unit.source.find("->get_name()") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<godot::Array>("
                                    "gdpp::runtime::to_variant(value), godot::Variant::ARRAY") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".size()") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") == std::string::npos);
}

TEST_CASE("generated object method calls reject null and freed receivers") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("safe_receiver.gd", "extends Node\n"
                                             "func typed_count(value: Node) -> int:\n"
                                             "    return value.get_child_count()\n"
                                             "func narrowed_count(value: Variant) -> int:\n"
                                             "    if value is Node:\n"
                                             "        return value.get_child_count()\n"
                                             "    return 0\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::is_instance_valid") != std::string::npos);
    REQUIRE(
        result.unit.source.find("Cannot call method 'get_child_count' on a null or freed value.") !=
        std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 3, 12") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 6, 16") != std::string::npos);
}

TEST_CASE("generated object property reads reject null and freed receivers") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("safe_property.gd", "extends Node\n"
                                             "func typed_name(value: Node) -> String:\n"
                                             "    return value.name\n"
                                             "func narrowed_name(value: Variant) -> String:\n"
                                             "    if value is Node:\n"
                                             "        return value.name\n"
                                             "    return \"\"\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_property_receiver_") != std::string::npos);
    REQUIRE(result.unit.source.find("Cannot access member 'name' on a null or freed value.") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 3, 12") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_source_path, 6, 16") != std::string::npos);
}

TEST_CASE("generated object property writes reject null and freed receivers") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("safe_property_write.gd",
                         "extends Node\n"
                         "class Probe extends RefCounted:\n"
                         "    var score: int:\n"
                         "        get:\n"
                         "            return 1\n"
                         "        set(value):\n"
                         "            pass\n"
                         "func assign(node: Node, probe: Probe, sprite: Sprite2D) -> void:\n"
                         "    node.name = \"updated\"\n"
                         "    probe.score = 7\n"
                         "    sprite.position.x = 4.0\n"
                         "func assign_and_return(node: Node) -> int:\n"
                         "    node.name = \"returned\"\n"
                         "    return 1\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_property_receiver_") != std::string::npos);
    REQUIRE(result.unit.source.find("Cannot assign member 'name' on a null or freed object at "
                                    "safe_property_write.gd:9") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_named(_gdpp_attached_property_target_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("Cannot assign member 'position' on a null or freed object at "
                                    "safe_property_write.gd:11") != std::string::npos);
    REQUIRE(result.unit.source.find("ERR_FAIL_EDMSG") != std::string::npos);
    REQUIRE(result.unit.source.find("ERR_FAIL_V_EDMSG") != std::string::npos);
}

TEST_CASE("compound property writes evaluate receivers current values and right sides once") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("compound_receiver_order.gd",
                                         "extends Node\n"
                                         "class Probe extends RefCounted:\n"
                                         "    var score: int = 0\n"
                                         "func take_node() -> Node:\n"
                                         "    return self\n"
                                         "func take_sprite() -> Sprite2D:\n"
                                         "    return null\n"
                                         "func take_probe() -> Probe:\n"
                                         "    return Probe.new()\n"
                                         "func next_delta() -> int:\n"
                                         "    return 1\n"
                                         "func apply() -> void:\n"
                                         "    take_node().process_priority += next_delta()\n"
                                         "    take_sprite().position.x += float(next_delta())\n"
                                         "    take_probe().score += next_delta()\n");

    REQUIRE(result.success);
    const auto begin = result.unit.source.find("void GDPPNative_CompoundReceiverOrder::apply()");
    const auto end = result.unit.source.find(
        "void GDPPNative_CompoundReceiverOrder::_gdpp_variant_call_take_node", begin);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    const std::string_view body{result.unit.source.data() + begin, end - begin};
    const auto occurrences = [&](const std::string_view needle) {
        std::size_t count = 0;
        for (std::size_t offset = body.find(needle); offset != std::string_view::npos;
             offset = body.find(needle, offset + needle.size())) {
            ++count;
        }
        return count;
    };

    REQUIRE_EQ(occurrences("take_node()"), std::size_t{1});
    REQUIRE_EQ(occurrences("take_sprite()"), std::size_t{1});
    REQUIRE_EQ(occurrences("take_probe()"), std::size_t{1});
    REQUIRE_EQ(occurrences("next_delta()"), std::size_t{3});
    REQUIRE_EQ(occurrences("const auto _gdpp_property_current_"), std::size_t{3});
    REQUIRE_EQ(occurrences("const auto _gdpp_property_right_"), std::size_t{3});
    REQUIRE_EQ(occurrences("->get_process_priority()"), std::size_t{1});
    REQUIRE_EQ(occurrences("->set_process_priority("), std::size_t{1});
    REQUIRE_EQ(occurrences("->get_position()"), std::size_t{1});
    REQUIRE_EQ(occurrences("->set_position("), std::size_t{1});
    REQUIRE_EQ(occurrences("gdpp::runtime::get_named("), std::size_t{1});
    REQUIRE_EQ(occurrences("gdpp::runtime::set_named("), std::size_t{1});

    const auto node_receiver = body.find("take_node()");
    const auto node_current = body.find("const auto _gdpp_property_current_", node_receiver);
    const auto node_read = body.find("->get_process_priority()", node_current);
    const auto node_right = body.find("const auto _gdpp_property_right_", node_read);
    const auto first_right = body.find("next_delta()", node_right);
    const auto node_write = body.find("->set_process_priority(", first_right);
    REQUIRE(node_receiver < node_current);
    REQUIRE(node_current < node_read);
    REQUIRE(node_read < node_right);
    REQUIRE(node_right < first_right);
    REQUIRE(first_right < node_write);

    const auto sprite_receiver = body.find("take_sprite()");
    const auto position_read = body.find("->get_position()", sprite_receiver);
    const auto position_current = body.find("const auto _gdpp_property_current_", position_read);
    const auto position_right = body.find("const auto _gdpp_property_right_", position_current);
    const auto second_right = body.find("next_delta()", position_right);
    const auto position_write = body.find("->set_position(", second_right);
    REQUIRE(sprite_receiver < position_read);
    REQUIRE(position_read < position_current);
    REQUIRE(position_current < position_right);
    REQUIRE(position_right < second_right);
    REQUIRE(second_right < position_write);
}

TEST_CASE("semantic flow narrows short-circuit logical operands") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("flow_short_circuit.gd",
                         "extends Node\n"
                         "func positive(value: Variant) -> bool:\n"
                         "    return value is int and value > 0\n"
                         "func named(value: Variant) -> bool:\n"
                         "    return value is not Node or value.name == &\"ready\"\n"
                         "func positioned(value: Variant) -> bool:\n"
                         "    return value is Node and value is Node2D and value.position.x > 0\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<int64_t>("
                                    "gdpp::runtime::to_variant(value), godot::Variant::INT") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node>") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node2D>") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") == std::string::npos);
}

TEST_CASE("semantic flow invalidates a refinement after direct reassignment") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("flow_assignment.gd", "extends Node\n"
                                               "func replace(value: Variant) -> Variant:\n"
                                               "    if value is Node:\n"
                                               "        value = 40\n"
                                               "        value += 2\n"
                                               "        return value.name\n"
                                               "    return value\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::binary") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") != std::string::npos);
}

TEST_CASE("semantic flow does not leak transient branch facts into deferred lambdas") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("flow_lambda.gd", "extends Node\n"
                                           "func defer_name(value: Variant) -> Callable:\n"
                                           "    if value is Node:\n"
                                           "        return func() -> Variant: return value.name\n"
                                           "    return func() -> Variant: return null\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named(value") != std::string::npos);
}

TEST_CASE("semantic flow carries the sole fallthrough refinement past guards") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("flow_postdominator.gd",
                                         "extends Node\n"
                                         "func after_negative_guard(value: Variant) -> String:\n"
                                         "    if value is not Node:\n"
                                         "        return \"\"\n"
                                         "    return value.name\n"
                                         "func after_else_guard(value: Variant) -> String:\n"
                                         "    if value is Node:\n"
                                         "        pass\n"
                                         "    else:\n"
                                         "        return \"\"\n"
                                         "    return value.name\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node>") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") == std::string::npos);
}

TEST_CASE("semantic flow narrows each lazy conditional-expression arm") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("flow_conditional.gd",
                                         "extends Node\n"
                                         "func direct(value: Variant) -> String:\n"
                                         "    return value.name if value is Node else \"\"\n"
                                         "func negated(value: Variant) -> String:\n"
                                         "    return \"\" if value is not Node else value.name\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node>") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") == std::string::npos);
}

TEST_CASE("semantic flow narrows structural match subjects and guarded bindings") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("flow_match.gd", "extends Node\n"
                                          "func container_size(value: Variant) -> int:\n"
                                          "    match value:\n"
                                          "        []: return value.size()\n"
                                          "        {}: return value.size()\n"
                                          "        _: return -1\n"
                                          "func guarded_name(value: Variant) -> String:\n"
                                          "    match value:\n"
                                          "        var item when item is Node: return item.name\n"
                                          "        _: return \"\"\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<godot::Array>("
                                    "gdpp::runtime::to_variant(value), godot::Variant::ARRAY") !=
            std::string::npos);
    REQUIRE(
        result.unit.source.find("gdpp::runtime::strict_builtin_storage<godot::Dictionary>("
                                "gdpp::runtime::to_variant(value), godot::Variant::DICTIONARY") !=
        std::string::npos);
    REQUIRE(result.unit.source.find("godot::Object::cast_to<godot::Node>") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::call_dynamic") == std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_named") == std::string::npos);
}

TEST_CASE("typed scene nodes retain dynamic script dispatch for unknown members") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("scene_dispatch.gd", "extends Node\n"
                                              "func start(screen: Node2D) -> Variant:\n"
                                              "    screen.enabled = true\n"
                                              "    return screen.initialize()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::set_named") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::call_dynamic") != std::string::npos);
}

TEST_CASE("compiler rejects invalid type-test operands transactionally") {
    const gdpp::Compiler compiler;
    const auto invalid_target =
        compiler.compile("invalid_target.gd", "func accepts(value: Variant) -> bool:\n"
                                              "    return value is 42\n");
    const auto invalid_value = compiler.compile("invalid_value.gd", "func accepts() -> bool:\n"
                                                                    "    return Node is Node\n");

    REQUIRE(!invalid_target.success);
    REQUIRE(!invalid_value.success);
    REQUIRE(invalid_target.unit.header.empty());
    REQUIRE(invalid_target.unit.source.empty());
    REQUIRE(invalid_value.unit.header.empty());
    REQUIRE(invalid_value.unit.source.empty());
    bool found_target_diagnostic = false;
    for (const auto& diagnostic : invalid_target.diagnostics)
        found_target_diagnostic = found_target_diagnostic || diagnostic.code == "GDS2001";
    bool found_value_diagnostic = false;
    for (const auto& diagnostic : invalid_value.diagnostics)
        found_value_diagnostic = found_value_diagnostic || diagnostic.code == "GDS4068";
    REQUIRE(found_target_diagnostic);
    REQUIRE(found_value_diagnostic);
}

TEST_CASE("semantic flow analysis requires concrete returns on every reachable path") {
    const gdpp::Compiler compiler;
    const auto exhaustive_if =
        compiler.compile("if_returns.gd", "func choose(value: bool) -> int:\n"
                                          "    if value:\n"
                                          "        return 1\n"
                                          "    else:\n"
                                          "        return 2\n");
    const auto exhaustive_match =
        compiler.compile("match_returns.gd", "func choose(value: int) -> String:\n"
                                             "    match value:\n"
                                             "        0:\n"
                                             "            return \"zero\"\n"
                                             "        _:\n"
                                             "            return \"other\"\n");
    const auto infinite_loop = compiler.compile("infinite.gd", "func run_forever() -> int:\n"
                                                               "    while true:\n"
                                                               "        pass\n");
    const auto partial_if = compiler.compile("partial.gd", "func choose(value: bool) -> int:\n"
                                                           "    if value:\n"
                                                           "        return 1\n");

    REQUIRE(exhaustive_if.success);
    REQUIRE(exhaustive_match.success);
    REQUIRE(infinite_loop.success);
    REQUIRE(!partial_if.success);
    bool found_missing_return = false;
    for (const auto& diagnostic : partial_if.diagnostics)
        found_missing_return = found_missing_return || diagnostic.code == "GDS4009";
    REQUIRE(found_missing_return);
}

TEST_CASE("semantic flow analysis warns about unreachable statements and rejects partial getters") {
    const gdpp::Compiler compiler;
    const auto unreachable = compiler.compile("unreachable.gd", "func answer() -> int:\n"
                                                                "    return 42\n"
                                                                "    print(\"never\")\n");
    const auto complete_getter = compiler.compile("complete_getter.gd", "var enabled: bool = true\n"
                                                                        "var value: int:\n"
                                                                        "    get:\n"
                                                                        "        if enabled:\n"
                                                                        "            return 1\n"
                                                                        "        else:\n"
                                                                        "            return 2\n");
    const auto partial_getter = compiler.compile("partial_getter.gd", "var enabled: bool = true\n"
                                                                      "var value: int:\n"
                                                                      "    get:\n"
                                                                      "        if enabled:\n"
                                                                      "            return 1\n");

    REQUIRE(unreachable.success);
    REQUIRE(complete_getter.success);
    REQUIRE(!partial_getter.success);
    bool found_unreachable = false;
    for (const auto& diagnostic : unreachable.diagnostics)
        found_unreachable =
            found_unreachable || (diagnostic.code == "GDS4069" &&
                                  diagnostic.severity == gdpp::DiagnosticSeverity::warning);
    bool found_partial_getter = false;
    for (const auto& diagnostic : partial_getter.diagnostics)
        found_partial_getter = found_partial_getter || diagnostic.code == "GDS4050";
    REQUIRE(found_unreachable);
    REQUIRE(found_partial_getter);
}

TEST_CASE("Godot numeric class names map to their actual header names") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("sprite.gd", "extends Node2D\nclass_name SpriteActor\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <godot_cpp/classes/node2d.hpp>") !=
            std::string::npos);
}

TEST_CASE("Godot ClassDB maps to the collision-safe godot-cpp singleton") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("core_types.gd", "extends Node\n"
                                          "func available(name: StringName) -> bool:\n"
                                          "    return ClassDB.class_exists(name)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <godot_cpp/classes/class_db_singleton.hpp>") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("#include <godot_cpp/classes/class_db.hpp>") ==
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::ClassDBSingleton::get_singleton()") !=
            std::string::npos);
}

TEST_CASE("Godot calls in static initializers retain ordered native invocation") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "static_native_call.gd",
        "extends Node\n"
        "static var enum_names := ClassDB.class_get_enum_constants(\"Node\", \"ProcessMode\")\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("[]()") != std::string::npos);
    REQUIRE(result.unit.source.find("->class_get_enum_constants(") != std::string::npos);
    REQUIRE(result.unit.source.find("->class_get_enum_constants;") == std::string::npos);
}

TEST_CASE("Godot properties include concrete native getter result classes") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("scene_root.gd", "extends Node\n"
                                                          "func scene_root() -> Node:\n"
                                                          "    return get_tree().root\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <godot_cpp/classes/scene_tree.hpp>") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("#include <godot_cpp/classes/window.hpp>") !=
            std::string::npos);
}

TEST_CASE("semantic analysis rejects duplicate declarations and invalid typed initializers") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("invalid.gd", "var count: int = \"not a number\"\n"
                                                       "var count: int = 2\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.source.empty());
    REQUIRE(result.diagnostics.size() >= std::size_t{2});
}

TEST_CASE("semantic analysis rejects constant writes and loop control outside loops") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("invalid_flow.gd", "const LIMIT := 10\n"
                                                            "func mutate() -> void:\n"
                                                            "    LIMIT = 11\n"
                                                            "    break\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.header.empty());
}

TEST_CASE("local constants remain typed read-only values through native code generation") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile("local_constant.gd", "extends Node\n"
                                                             "func report() -> void:\n"
                                                             "    const LIMIT : = 123_\n"
                                                             "    var state := {score = LIMIT,}\n"
                                                             "    state.return = LIMIT\n"
                                                             "    print(state.score, LIMIT,)\n");
    const auto invalid = compiler.compile("local_constant_write.gd", "func mutate() -> void:\n"
                                                                     "    const LIMIT = 1\n"
                                                                     "    LIMIT = 2\n");

    REQUIRE(valid.success);
    REQUIRE(valid.unit.source.find("const int64_t LIMIT = static_cast<int64_t>(123);") !=
            std::string::npos);
    REQUIRE(!invalid.success);
    REQUIRE(invalid.unit.source.empty());
    REQUIRE(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4006"; }));
}

TEST_CASE("static constructors are validated and run through the class initialization guard") {
    const gdpp::Compiler compiler;
    const auto valid = compiler.compile("static_init.gd", "static var initialized: bool = false\n"
                                                          "static func _static_init() -> void:\n"
                                                          "    initialized = true\n");
    const auto tool =
        compiler.compile("tool_static_init.gd", "@tool\n"
                                                "static var initialized := false\n"
                                                "static func _static_init() -> void:\n"
                                                "    initialized = true\n");
    const auto non_static = compiler.compile("non_static_init.gd", "func _static_init() -> void:\n"
                                                                   "    pass\n");
    const auto returning =
        compiler.compile("returning_static_init.gd", "static func _static_init():\n"
                                                     "    return true\n");
    const auto loop_conflict = compiler.compile("loop_conflict.gd", "func iterate() -> void:\n"
                                                                    "    var item = 1\n"
                                                                    "    for item in 2:\n"
                                                                    "        pass\n");

    REQUIRE(valid.success);
    REQUIRE(valid.unit.header.find("static bool _gdpp_ensure_static_initialized()") !=
            std::string::npos);
    REQUIRE(valid.unit.source.find("_gdpp_static_initialization().run(") != std::string::npos);
    REQUIRE(valid.unit.source.find("static thread_local bool active = false") == std::string::npos);
    REQUIRE(valid.unit.source.find("if (gdpp::runtime::is_editor_hint()) return true;") !=
            std::string::npos);
    REQUIRE(valid.unit.source.find("static thread_local bool editor_value{}") != std::string::npos);
    const auto valid_guard = valid.unit.source.find("::_gdpp_ensure_static_initialized() {");
    REQUIRE(valid_guard != std::string::npos);
    REQUIRE(valid.unit.source.find("            _static_init();", valid_guard) !=
            std::string::npos);
    const auto static_initializer = valid.unit.source.find("::_static_init() {");
    REQUIRE(static_initializer != std::string::npos);
    REQUIRE(valid.unit.source.find("ScriptFaultPolicy::inherit_existing", static_initializer) !=
            std::string::npos);
    const auto valid_bind = valid.unit.source.find("::_bind_methods() {");
    const auto valid_bind_end = valid.unit.source.find("}\n\n", valid_bind);
    REQUIRE(valid_bind != std::string::npos);
    REQUIRE(valid_bind_end != std::string::npos);
    REQUIRE(
        valid.unit.source.substr(valid_bind, valid_bind_end - valid_bind).find("_static_init();") ==
        std::string::npos);
    REQUIRE(valid.unit.source.find("D_METHOD(\"_static_init\"") == std::string::npos);
    REQUIRE(tool.success);
    const auto tool_guard = tool.unit.source.find("::_gdpp_ensure_static_initialized() {");
    REQUIRE(tool_guard != std::string::npos);
    REQUIRE(tool.unit.source.find("            _static_init();", tool_guard) != std::string::npos);
    REQUIRE(tool.unit.source.find("editor_value") == std::string::npos);
    const auto tool_bind = tool.unit.source.find("::_bind_methods() {");
    const auto tool_bind_end = tool.unit.source.find("}\n\n", tool_bind);
    REQUIRE(tool_bind != std::string::npos);
    REQUIRE(tool_bind_end != std::string::npos);
    REQUIRE(tool.unit.source.substr(tool_bind, tool_bind_end - tool_bind).find("_static_init();") ==
            std::string::npos);
    REQUIRE(!non_static.success);
    REQUIRE(!returning.success);
    REQUIRE(!loop_conflict.success);
    REQUIRE(std::any_of(non_static.diagnostics.begin(), non_static.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4123"; }));
    REQUIRE(std::any_of(loop_conflict.diagnostics.begin(), loop_conflict.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4125"; }));
}

TEST_CASE("semantic analysis rejects every non-lvalue assignment target") {
    const gdpp::Compiler compiler;
    const auto literal = compiler.compile("literal_target.gd", "func bad() -> void:\n    1 = 2\n");
    const auto call = compiler.compile(
        "call_target.gd",
        "func value() -> int:\n    return 1\nfunc bad() -> void:\n    value() = 2\n");

    REQUIRE(!literal.success);
    REQUIRE(!call.success);
    REQUIRE(literal.unit.source.empty());
    REQUIRE(call.unit.source.empty());
}

TEST_CASE("inferred collection types and numeric-looking strings generate native types") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("collections.gd", "extends Node\n"
                                           "var count := 1\n"
                                           "var text := \"123\"\n"
                                           "func visit() -> void:\n"
                                           "    var values := [1, 2]\n"
                                           "    var labels := {\"one\": 1}\n"
                                           "    for value in values:\n"
                                           "        print(labels[\"one\"], value)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("count = static_cast<int64_t>(1)") != std::string::npos);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::assign_native_storage(text, godot::String(\"123\"))") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Array values") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_dictionary_") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant value = _gdpp_array_iterable_") !=
            std::string::npos);
}

TEST_CASE("empty strings remain empty through lexing semantic analysis and code generation") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("empty.gd", "var text := \"\"\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::assign_native_storage(text, godot::String(\"\"))") !=
            std::string::npos);
}

TEST_CASE("compiler optimization can be enabled or disabled without changing the frontend") {
    const gdpp::Compiler compiler;
    const std::string source = "func answer() -> int:\n    return 40 + 2\n";
    const auto optimized = compiler.compile("fold.gd", source);
    gdpp::CompileOptions debug_options;
    debug_options.optimize = false;
    const auto unoptimized = compiler.compile("fold.gd", source, debug_options);

    REQUIRE(optimized.success);
    REQUIRE(unoptimized.success);
    REQUIRE_EQ(optimized.optimization.constants_folded, std::size_t{1});
    REQUIRE_EQ(unoptimized.optimization.constants_folded, std::size_t{0});
    REQUIRE(optimized.unit.source.find("const auto _gdpp_return_value_") != std::string::npos);
    REQUIRE(optimized.unit.source.find(" = static_cast<int64_t>(42);") != std::string::npos);
    REQUIRE(unoptimized.unit.source.find("gdpp::integer::add(") != std::string::npos);
    REQUIRE(unoptimized.unit.source.find(" = static_cast<int64_t>(40);") != std::string::npos);
    REQUIRE(unoptimized.unit.source.find(" = static_cast<int64_t>(2);") != std::string::npos);
}

TEST_CASE("compiler optimization removes constant dead branches without changing live output") {
    const gdpp::Compiler compiler;
    const std::string source = "func choose() -> int:\n"
                               "    if 20 + 22 == 42:\n"
                               "        return 7\n"
                               "    else:\n"
                               "        print(\"dead-branch\")\n"
                               "        return 9\n"
                               "func skip_loop() -> void:\n"
                               "    while false:\n"
                               "        print(\"dead-loop\")\n";
    const auto optimized = compiler.compile("control_flow.gd", source);
    gdpp::CompileOptions unoptimized_options;
    unoptimized_options.optimize = false;
    const auto unoptimized = compiler.compile("control_flow.gd", source, unoptimized_options);

    REQUIRE(optimized.success);
    REQUIRE(unoptimized.success);
    REQUIRE_EQ(optimized.optimization.branches_simplified, std::size_t{2});
    REQUIRE(optimized.mir_optimization.branches_simplified >= 1U);
    REQUIRE(optimized.mir_optimization.blocks_removed >= 1U);
    REQUIRE_EQ(unoptimized.optimization.branches_simplified, std::size_t{0});
    REQUIRE_EQ(unoptimized.mir_optimization.blocks_removed, std::size_t{0});
    REQUIRE(optimized.unit.source.find("dead-branch") == std::string::npos);
    REQUIRE(optimized.unit.source.find("dead-loop") == std::string::npos);
    REQUIRE(unoptimized.unit.source.find("dead-branch") != std::string::npos);
    REQUIRE(unoptimized.unit.source.find("dead-loop") != std::string::npos);
}

TEST_CASE("Godot API inheritance resolves native methods properties and builtin types") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("mover.gd", "extends Node2D\n"
                                                     "func move_by(delta: Vector2) -> void:\n"
                                                     "    position += delta\n"
                                                     "    queue_redraw()\n"
                                                     "func magnitude(value: Vector2) -> float:\n"
                                                     "    return value.length()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <godot_cpp/variant/vector2.hpp>") !=
            std::string::npos);
    REQUIRE(result.unit.header.find("godot::Vector2 delta") != std::string::npos);
    REQUIRE(result.unit.source.find("set_position(std::move(_gdpp_assignment_result_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("= get_position(); const auto _gdpp_property_right_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("queue_redraw()") != std::string::npos);
    REQUIRE(result.unit.source.find(".length()") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_call_receiver_") != std::string::npos);
}

TEST_CASE("builtin unary operators use Variant evaluation when godot-cpp has no operator") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("unary_builtin.gd", "extends Node\n"
                                             "func positive(value: Vector2) -> Vector2:\n"
                                             "    return +value\n"
                                             "func negative(value: Vector2) -> Vector2:\n"
                                             "    return -value\n"
                                             "func integer(value: int) -> int:\n"
                                             "    return +value\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("godot::Variant::OP_POSITIVE") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Variant::OP_NEGATE") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<godot::Vector2>("
                                    "gdpp::runtime::to_variant(gdpp::runtime::unary(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(" = (+value);") != std::string::npos);
}

TEST_CASE("static function values use owner-free callables with default argument semantics") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("static_callable.gd", "extends Resource\n"
                                               "static func compare(left, right = 0) -> bool:\n"
                                               "    return left < right\n"
                                               "static func sorter(values: Array) -> void:\n"
                                               "    values.sort_custom(compare)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_callable(nullptr, 1, 2") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("GDPPNative_StaticCallable::compare(") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Callable(this, godot::StringName(\"compare\"))") ==
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::default_argument()") != std::string::npos);
}

TEST_CASE("internal static function values use owner-free callables") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("internal_static_callable.gd",
                         "extends Resource\n"
                         "class Sorter:\n"
                         "    static func compare(left: int, right: int = 0) -> bool:\n"
                         "        return left < right\n"
                         "func sort_values(values: Array) -> void:\n"
                         "    values.sort_custom(Sorter.compare)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_callable(nullptr, 1, 2") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("::compare(") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Callable(this") == std::string::npos);
}

TEST_CASE("unknown lowercase identifiers fail before native code generation") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("typo.gd", "extends Node\n"
                                                    "func broken() -> void:\n"
                                                    "    print(misspelled_value)\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.source.empty());
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4122"; }));
}

TEST_CASE("expression statements explicitly discard native results") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("discard.gd", "extends Node\n"
                                                       "func consume(value: float) -> void:\n"
                                                       "    int(value)\n"
                                                       "    Vector2(1, 2)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "static_cast<void>(gdpp::runtime::explicit_variant_cast<int64_t>") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<void>(([&]() -> godot::Vector2") !=
            std::string::npos);
}

TEST_CASE("Godot API method arity errors stop native code generation") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("invalid_api.gd", "extends Node\n"
                                                           "func invalid() -> void:\n"
                                                           "    add_child()\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.source.empty());
    REQUIRE(!result.diagnostics.empty());
}

TEST_CASE("Godot API argument type errors are diagnosed before C++ generation") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("invalid_api_type.gd", "extends Node\n"
                                                                "func invalid() -> void:\n"
                                                                "    add_child(42)\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.header.empty());
}

TEST_CASE("compiler gates Godot 4.7 APIs using the selected target") {
    const std::string source = "extends Node\n"
                               "func hdr_enabled() -> bool:\n"
                               "    return DisplayServer.window_is_hdr_output_enabled()\n";
    const gdpp::Compiler compiler;
    const auto baseline = compiler.compile("versioned.gd", source);
    gdpp::CompileOptions latest_options;
    latest_options.target_version = gdpp::GodotVersion::v4_7;
    const auto latest = compiler.compile("versioned.gd", source, latest_options);

    REQUIRE(!baseline.success);
    REQUIRE(latest.success);
    REQUIRE(latest.unit.source.find("window_is_hdr_output_enabled") != std::string::npos);
}

TEST_CASE("typed Godot object parameters generate pointer calls") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("objects.gd", "extends Node\n"
                                                       "func release(node: Node) -> void:\n"
                                                       "    node.queue_free()\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("gdpp::runtime::ObjectStorage<godot::Node> node") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("->queue_free()") != std::string::npos);
}

TEST_CASE("compiler validates every reflected engine signal argument contract") {
    const gdpp::Compiler compiler;
    const auto valid =
        compiler.compile("engine_signal.gd", "extends Control\n"
                                             "func relay(event: InputEvent) -> void:\n"
                                             "    gui_input.emit(event)\n");
    const auto wrong_count = compiler.compile("engine_signal_count.gd", "extends Control\n"
                                                                        "func relay() -> void:\n"
                                                                        "    gui_input.emit()\n");
    const auto wrong_type = compiler.compile("engine_signal_type.gd", "extends Control\n"
                                                                      "func relay() -> void:\n"
                                                                      "    gui_input.emit(42)\n");

    REQUIRE(valid.success);
    REQUIRE(!wrong_count.success);
    REQUIRE(std::any_of(wrong_count.diagnostics.begin(), wrong_count.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4164"; }));
    REQUIRE(!wrong_type.success);
    REQUIRE(std::any_of(wrong_type.diagnostics.begin(), wrong_type.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4002"; }));
}

TEST_CASE("non-RefCounted object storage preserves weak identity in every generated frame") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "weak_objects.gd", "extends Node\n"
                           "var retained: Node\n"
                           "func exercise(parameter: Node, signal_value: Signal) -> Callable:\n"
                           "    var local := Node.new()\n"
                           "    retained = local\n"
                           "    return func() -> int:\n"
                           "        retained = parameter\n"
                           "        return local.get_child_count()\n"
                           "func suspend(signal_value: Signal) -> int:\n"
                           "    var suspended := Node.new()\n"
                           "    await signal_value\n"
                           "    return suspended.get_child_count()\n");

    REQUIRE(result.success);
    const std::string storage = "gdpp::runtime::ObjectStorage<godot::Node>";
    REQUIRE(result.unit.header.find(storage + " retained{}") != std::string::npos);
    REQUIRE(result.unit.header.find(storage + " parameter") != std::string::npos);
    REQUIRE(result.unit.source.find(storage + " local") != std::string::npos);
    REQUIRE(result.unit.source.find("[=]() mutable -> godot::Variant") != std::string::npos);
    REQUIRE(result.unit.source.find(storage + " suspended") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::to_variant(_gdpp_call_receiver_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("godot::Node* local") == std::string::npos);
}

TEST_CASE("Godot builtin constructors resolve overloads and native value types") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("vectors.gd", "extends Node\n"
                                                       "var origin := Vector2(1.0, 2.0)\n"
                                                       "var accent := Color(0.1, 0.2, 0.3, 1.0)\n"
                                                       "func distance(value: Vector2) -> float:\n"
                                                       "    return value.distance_to(origin)\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("const auto _gdpp_call_result_") != std::string::npos);
    REQUIRE(result.unit.source.find(" = godot::Vector2(static_cast<godot::real_t>(") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<godot::real_t>(_gdpp_call_argument_") !=
            std::string::npos);
    REQUIRE(result.unit.source.find(".distance_to(") != std::string::npos);
}

TEST_CASE("builtin constructors retain call syntax when a value shadows their type name") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("shadowed_constructor.gd",
                                         "extends Node\n"
                                         "func convert(value: float, int := false) -> Variant:\n"
                                         "    if int:\n"
                                         "        return int(value)\n"
                                         "    return int\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::explicit_variant_cast<int64_t>(") !=
            std::string::npos);
}

TEST_CASE("invalid Godot builtin constructor overloads are diagnosed") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("invalid_vector.gd", "var value := Vector2(\"x\")\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.source.empty());
}

TEST_CASE("Godot singleton calls lower to native get_singleton access") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("input_reader.gd", "extends Node\n"
                                            "func pressed() -> bool:\n"
                                            "    return Input.is_action_pressed(\"ui_accept\")\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.header.find("#include <godot_cpp/classes/input.hpp>") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::Input::get_singleton()") != std::string::npos);
    REQUIRE(result.unit.source.find("->is_action_pressed(") != std::string::npos);
}

TEST_CASE("Godot scalar API returns cross the native ABI with explicit conversions") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("native_returns.gd", "extends Node\n"
                                              "func ticks() -> int:\n"
                                              "    return Time.get_ticks_usec()\n"
                                              "func random_value() -> int:\n"
                                              "    return randi()\n"
                                              "func screen_index() -> int:\n"
                                              "    return get_window().current_screen\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("->get_ticks_usec())") != std::string::npos);
    REQUIRE(result.unit.source.find("static_cast<int64_t>(godot::UtilityFunctions::randi())") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("->get_current_screen())") != std::string::npos);
}

TEST_CASE("third-party GDExtension singletons resolve through Engine at runtime") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("steam.gd", "extends Node\n"
                                                     "func poll() -> void:\n"
                                                     "    Steam.run_callbacks()\n"
                                                     "func singleton() -> Variant:\n"
                                                     "    return Steam\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(
                "gdpp::runtime::find_engine_singleton_at(godot::StringName(\"Steam\"), "
                "gdpp::runtime::ScriptSourceLocation{_gdpp_source_path, 3, 5})") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::call_dynamic") != std::string::npos);
    REQUIRE(result.unit.source.find("static const godot::StringName _gdpp_dynamic_method_") !=
            std::string::npos);
    const auto singleton = result.unit.source.find("::singleton(");
    const auto lookup = result.unit.source.find("find_engine_singleton_at(", singleton);
    const auto failure = result.unit.source.find("if (script_function_failed())", lookup);
    const auto returned = result.unit.source.find("return _gdpp_return_value_", failure);
    REQUIRE(singleton != std::string::npos);
    REQUIRE(singleton < lookup);
    REQUIRE(lookup < failure);
    REQUIRE(failure < returned);
}

TEST_CASE("Godot builtin static methods lower to native scope calls") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("colors.gd", "extends Node\n"
                                                      "var accent := Color.html(\"ff8800\")\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find(" = godot::Color::html(_gdpp_call_argument_") !=
            std::string::npos);
}

TEST_CASE("compiler lowers Godot 4.7 global utilities constants enums and range") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_7;
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("globals.gd",
                                         "extends Node\n"
                                         "var angle: float = deg_to_rad(180.0)\n"
                                         "var side: Side = Side.SIDE_LEFT\n"
                                         "var maximum: int = INT64_MAX\n"
                                         "var circle: float = PI * 2.0\n"
                                         "func label(value: float) -> String:\n"
                                         "    return str(clampf(value, 0.0, 1.0))\n"
                                         "func indices() -> Array:\n"
                                         "    return range(1, 8, 2)\n",
                                         options);

    REQUIRE(result.success);
    REQUIRE(
        result.unit.source.find("godot::UtilityFunctions::deg_to_rad(_gdpp_utility_argument_") !=
        std::string::npos);
    REQUIRE(result.unit.source.find("side = 0") != std::string::npos);
    REQUIRE(result.unit.source.find("9223372036854775807") != std::string::npos);
    REQUIRE(result.unit.source.find("Math_PI") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::UtilityFunctions::clampf") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::UtilityFunctions::str") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::make_range") != std::string::npos);
}

TEST_CASE("compiler matches zero-arity varargs constant preload and warning ranges") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_7;
    const gdpp::Compiler compiler;
    const auto utilities = compiler.compile("zero_varargs.gd",
                                            "func test() -> String:\n"
                                            "    print()\n"
                                            "    return str()\n",
                                            options);
    const auto preload = compiler.compile("constant_preload.gd",
                                          "func load_scene():\n"
                                          "    const ROOT = \"res://effects/\"\n"
                                          "    const PATH = ROOT + \"spark.tscn\"\n"
                                          "    return preload(PATH)\n",
                                          options);
    const auto warnings = compiler.compile("warning_ranges.gd",
                                           "@warning_ignore_start(\"unreachable_code\")\n"
                                           "func ignored() -> void:\n"
                                           "    return\n"
                                           "    print(1)\n"
                                           "@warning_ignore_restore(\"unreachable_code\")\n"
                                           "func reported() -> void:\n"
                                           "    return\n"
                                           "    print(2)\n",
                                           options);
    const auto static_instance = compiler.compile("static_instance.gd",
                                                  "class Worker:\n"
                                                  "    static func run() -> void:\n"
                                                  "        pass\n"
                                                  "func test() -> void:\n"
                                                  "    var worker := Worker.new()\n"
                                                  "    worker.run()\n",
                                                  options);

    REQUIRE(utilities.success);
    REQUIRE(preload.success);
    REQUIRE(warnings.success);
    REQUIRE(static_instance.success);
    REQUIRE(utilities.unit.source.find("godot::UtilityFunctions::print(godot::String())") !=
            std::string::npos);
    REQUIRE(utilities.unit.source.find("const auto _gdpp_return_value_") != std::string::npos);
    REQUIRE(utilities.unit.source.find(" = godot::String();") != std::string::npos);
    REQUIRE(preload.unit.source.find("gdpp::runtime::load_resource(") != std::string::npos);
    REQUIRE(preload.unit.source.find("PATH") != std::string::npos);
    REQUIRE(preload.unit.source.find("ROOT()") == std::string::npos);
    REQUIRE(preload.unit.source.find("PATH()") == std::string::npos);
    REQUIRE_EQ(std::count_if(
                   warnings.diagnostics.begin(), warnings.diagnostics.end(),
                   [](const gdpp::Diagnostic& diagnostic) { return diagnostic.code == "GDS4069"; }),
               std::ptrdiff_t{1});
    REQUIRE(std::any_of(static_instance.diagnostics.begin(), static_instance.diagnostics.end(),
                        [](const gdpp::Diagnostic& diagnostic) {
                            return diagnostic.severity == gdpp::DiagnosticSeverity::warning &&
                                   diagnostic.code == "GDS4130";
                        }));
}

TEST_CASE("compiler lowers GDScript debug stack queries on the 4.4 baseline") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_4;
    const auto result =
        gdpp::Compiler{}.compile("stack.gd",
                                 "func caller() -> Dictionary:\n"
                                 "    var stack := get_stack()\n"
                                 "    return stack[0] if not stack.is_empty() else {}\n",
                                 options);

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::get_stack()") != std::string::npos);
}

TEST_CASE("compiler lowers the complete GDScript debug utility family") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_4;
    const auto result = gdpp::Compiler{}.compile("debug_utilities.gd",
                                                 "func report(value) -> void:\n"
                                                 "    print_debug(\"value=\", value, 1, 2, 3, 4)\n"
                                                 "    print_debug()\n"
                                                 "    print_stack()\n",
                                                 options);

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::print_debug(") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::print_debug()") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::print_stack()") != std::string::npos);
    REQUIRE(result.unit.source.find("godot::UtilityFunctions::print_debug") == std::string::npos);
}

TEST_CASE("compiler lowers GDScript instance dictionary transport with native contracts") {
    gdpp::CompileOptions options;
    options.target_version = gdpp::GodotVersion::v4_4;
    const auto result = gdpp::Compiler{}.compile("instance_dictionary.gd",
                                                 "func snapshot(instance: Object) -> Dictionary:\n"
                                                 "    return inst_to_dict(instance)\n"
                                                 "func restore(state: Dictionary) -> Object:\n"
                                                 "    return dict_to_inst(state)\n",
                                                 options);

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::instance_to_dictionary(") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::dictionary_to_instance(") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_native_object_value_storage<"
                                    "godot::Object>") != std::string::npos);
    REQUIRE(
        result.unit.source.find("gdpp::runtime::ScriptSourceLocation{_gdpp_source_path, 2, 12}") !=
        std::string::npos);
    REQUIRE(
        result.unit.source.find("gdpp::runtime::ScriptSourceLocation{_gdpp_source_path, 4, 12}") !=
        std::string::npos);
}

TEST_CASE("warning ignores scope every semantic warning on a function") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "warning_scope.gd", "class Worker:\n"
                            "    static func run() -> void:\n"
                            "        pass\n"
                            "@warning_ignore(\"static_called_on_instance\", \"redundant_await\", "
                            "\"unreachable_pattern\", \"unreachable_code\")\n"
                            "func suppressed() -> void:\n"
                            "    var worker := Worker.new()\n"
                            "    worker.run()\n"
                            "    await 42\n"
                            "    match 1:\n"
                            "        _:\n"
                            "            pass\n"
                            "        1:\n"
                            "            pass\n"
                            "    return\n"
                            "    print(1)\n"
                            "func reported() -> void:\n"
                            "    var worker := Worker.new()\n"
                            "    worker.run()\n"
                            "    await 42\n"
                            "    match 1:\n"
                            "        _:\n"
                            "            pass\n"
                            "        1:\n"
                            "            pass\n"
                            "    return\n"
                            "    print(1)\n");

    REQUIRE(result.success);
    const auto warning_count = [&](const std::string_view code) {
        return std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                             [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    REQUIRE_EQ(warning_count("GDS4130"), std::ptrdiff_t{1});
    REQUIRE_EQ(warning_count("GDS4093"), std::ptrdiff_t{1});
    REQUIRE_EQ(warning_count("GDS4044"), std::ptrdiff_t{1});
    REQUIRE_EQ(warning_count("GDS4069"), std::ptrdiff_t{1});
}

TEST_CASE("dynamic logical operators short circuit and utility arguments keep source order") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("evaluation_order.gd", "extends Node\n"
                                                "func probe(value: int) -> int:\n"
                                                "    return value\n"
                                                "func dynamic_and(left: Variant) -> bool:\n"
                                                "    return left and probe(1)\n"
                                                "func ordered() -> Variant:\n"
                                                "    return max(probe(1), probe(2))\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("_gdpp_logic_left_") != std::string::npos);
    REQUIRE(result.unit.source.find("if (!_gdpp_logic_left_") != std::string::npos);
    REQUIRE(result.unit.source.find("_gdpp_logic_right_") != std::string::npos);
    const auto first = result.unit.source.find("_gdpp_utility_argument_", 0);
    REQUIRE(first != std::string::npos);
    const auto second = result.unit.source.find("_gdpp_utility_argument_", first + 1);
    REQUIRE(second != std::string::npos);
    REQUIRE(first < second);
    REQUIRE(result.unit.source.find(" = static_cast<int64_t>(1);", first) != std::string::npos);
    REQUIRE(result.unit.source.find(" = static_cast<int64_t>(2);", second) != std::string::npos);
}

TEST_CASE("compiler sequences every eager binary operand before evaluation") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "binary_order.gd",
        "extends Resource\n"
        "func mark_float(value: float) -> float:\n"
        "    return value\n"
        "func mark_string(value: String) -> String:\n"
        "    return value\n"
        "func mark_vector(value: Vector2) -> Vector2:\n"
        "    return value\n"
        "func mark_variant(value: Variant) -> Variant:\n"
        "    return value\n"
        "func ordered() -> Array:\n"
        "    var arithmetic := mark_float(1.0) + mark_float(2.0)\n"
        "    var comparison := mark_string(\"a\") < mark_string(\"b\")\n"
        "    var builtin := mark_vector(Vector2.ONE) + mark_vector(Vector2.RIGHT)\n"
        "    var dynamic := mark_variant(1) + mark_variant(2)\n"
        "    var membership := mark_variant(\"x\") in mark_variant([\"x\"])\n"
        "    var power := mark_float(2.0) ** mark_float(3.0)\n"
        "    return [arithmetic, comparison, builtin, dynamic, membership, power]\n");

    REQUIRE(result.success);
    const auto first_left = result.unit.source.find("const auto _gdpp_binary_left_");
    const auto first_right = result.unit.source.find("const auto _gdpp_binary_right_", first_left);
    REQUIRE(first_left != std::string::npos);
    REQUIRE(first_right != std::string::npos);
    REQUIRE(first_left < first_right);
    for (const auto* operation : {"OP_ADD", "OP_IN", "OP_POWER"})
        REQUIRE(result.unit.source.find(operation) != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::binary(godot::Variant::OP_IN, "
                                    "mark_variant(") == std::string::npos);
}

TEST_CASE("compiler contains fatal expression faults and preserves Godot assignment order") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("failure_order.gd",
                         "extends RefCounted\n"
                         "func marker(name: String) -> int:\n"
                         "    return 1\n"
                         "func target(values: Array[int]) -> Array[int]:\n"
                         "    marker(\"target\")\n"
                         "    return values\n"
                         "func index() -> int:\n"
                         "    marker(\"index\")\n"
                         "    return 0\n"
                         "func direct() -> void:\n"
                         "    var values := [1]\n"
                         "    var result = marker(\"left\") + values[4] + marker(\"right\")\n"
                         "    marker(str(result))\n"
                         "func callee() -> int:\n"
                         "    var values := [1]\n"
                         "    return values[4]\n"
                         "func caller() -> int:\n"
                         "    return marker(\"before\") + callee() + marker(\"after\")\n"
                         "func logical(values: Array) -> bool:\n"
                         "    return values[4] or marker(\"logical-right\")\n"
                         "func conditional(values: Array) -> int:\n"
                         "    return marker(\"selected\") if values[4] else marker(\"fallback\")\n"
                         "func assign() -> void:\n"
                         "    var values: Array[int] = [0]\n"
                         "    target(values)[index()] = marker(\"rhs\")\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;

    const auto direct = source.find("::direct()");
    const auto bounds = source.find("gdpp::runtime::checked_array_get", direct);
    const auto bounds_failure = source.find("script_function_failed()", bounds);
    const auto right = source.find("godot::String(\"right\")", bounds);
    REQUIRE(direct != std::string::npos);
    REQUIRE(bounds != std::string::npos);
    REQUIRE(bounds < bounds_failure);
    REQUIRE(bounds_failure < right);

    const auto callee = source.find("::callee()");
    const auto caller = source.find("::caller()");
    REQUIRE(source.find("gdpp::runtime::ScriptFunctionScope", callee) < caller);
    REQUIRE(source.find("gdpp::runtime::ScriptFunctionScope", caller) != std::string::npos);
    const auto callee_call = source.find("callee()", caller + 1);
    const auto caller_after = source.find("godot::String(\"after\")", callee_call);
    REQUIRE(callee_call < caller_after);

    const auto logical = source.find("::logical(");
    const auto logical_bounds = source.find("gdpp::runtime::checked_array_get", logical);
    const auto logical_failure = source.find("script_function_failed()", logical_bounds);
    const auto logical_right = source.find("godot::String(\"logical-right\")", logical_bounds);
    REQUIRE(logical_bounds < logical_failure);
    REQUIRE(logical_failure < logical_right);

    const auto conditional = source.find("::conditional(");
    const auto condition_bounds = source.find("gdpp::runtime::checked_array_get", conditional);
    const auto condition_failure = source.find("script_function_failed()", condition_bounds);
    const auto selected = source.find("godot::String(\"selected\")", condition_bounds);
    const auto fallback = source.find("godot::String(\"fallback\")", condition_bounds);
    REQUIRE(condition_bounds < condition_failure);
    REQUIRE(condition_failure < selected);
    REQUIRE(condition_failure < fallback);

    const auto assignment = source.find("::assign()");
    const auto target_call = source.find("target(", assignment + 1);
    const auto rhs_call = source.find("godot::String(\"rhs\")", target_call);
    const auto index_call = source.find("index()", rhs_call);
    const auto write = source.find("gdpp::runtime::checked_array_set", index_call);
    REQUIRE(assignment != std::string::npos);
    REQUIRE(target_call < rhs_call);
    REQUIRE(rhs_call < index_call);
    REQUIRE(index_call < write);
}

TEST_CASE("compiler polls script faults only at statically fallible expression boundaries") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "fault_effects.gd", "extends RefCounted\n"
                            "func pure(index: int) -> int:\n"
                            "    var text := \"gdpp\"\n"
                            "    text = text.to_upper() if (index & 1) == 0 else text.to_lower()\n"
                            "    var total := text.length()\n"
                            "    total += (index & 7) + 1\n"
                            "    return total\n"
                            "func converted_argument(index: Variant) -> String:\n"
                            "    return \"gdpp\".substr(index)\n"
                            "func fallible(values: Array[int], divisor: int) -> int:\n"
                            "    return values[9] / divisor\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;
    const auto pure = source.find("::pure(");
    const auto converted = source.find("::converted_argument(", pure);
    const auto fallible = source.find("::fallible(", converted);
    REQUIRE(pure != std::string::npos);
    REQUIRE(converted != std::string::npos);
    REQUIRE(fallible != std::string::npos);
    const auto pure_body = source.substr(pure, converted - pure);
    const auto converted_body = source.substr(converted, fallible - converted);
    const auto fallible_body = source.substr(fallible);

    REQUIRE(pure_body.find("if (script_function_failed())") == std::string::npos);
    REQUIRE(pure_body.find(".to_upper()") != std::string::npos);
    REQUIRE(pure_body.find(".to_lower()") != std::string::npos);
    REQUIRE(pure_body.find("gdpp::integer::add(") != std::string::npos);
    REQUIRE(converted_body.find("gdpp::runtime::strict_builtin_storage<int64_t>(") !=
            std::string::npos);
    REQUIRE(converted_body.find("if (script_function_failed())") != std::string::npos);
    REQUIRE(fallible_body.find("gdpp::runtime::checked_typed_array_get<") != std::string::npos);
    REQUIRE(fallible_body.find("gdpp::runtime::integer_divide(") != std::string::npos);
    REQUIRE(fallible_body.find("if (script_function_failed())") != std::string::npos);
}

TEST_CASE("specialized calls stop argument evaluation at the first fatal fault") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "call_fault_order.gd",
        "extends RefCounted\n"
        "signal pulse(first, second, third)\n"
        "func marker(name: String) -> int:\n"
        "    return 1\n"
        "func utility() -> void:\n"
        "    print(marker(\"utility-first\"), [1][4], marker(\"utility-last\"))\n"
        "func invoke_callable(callback: Callable) -> void:\n"
        "    callback.call(marker(\"callable-first\"), [1][4], marker(\"callable-last\"))\n"
        "func invoke_dynamic(target: Variant) -> void:\n"
        "    target.accept(marker(\"dynamic-first\"), [1][4], marker(\"dynamic-last\"))\n"
        "func emit_signal_arguments() -> void:\n"
        "    pulse.emit(marker(\"signal-first\"), [1][4], marker(\"signal-last\"))\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;
    const auto require_stopped_call = [&](const std::string& method, const std::string& last_marker,
                                          const std::string& invocation) {
        const auto start = source.find("::" + method + "(");
        const auto bounds = source.find("gdpp::runtime::checked_array_get", start);
        const auto failure = source.find("script_function_failed()", bounds);
        const auto last = source.find("godot::String(\"" + last_marker + "\")", bounds);
        const auto call = source.find(invocation, last);
        REQUIRE(start != std::string::npos);
        REQUIRE(bounds < failure);
        REQUIRE(failure < last);
        REQUIRE(last < call);
    };
    require_stopped_call("utility", "utility-last", "godot::UtilityFunctions::print(");
    require_stopped_call("invoke_callable", "callable-last", "gdpp::runtime::call_callable_at(");
    require_stopped_call("invoke_dynamic", "dynamic-last", "gdpp::runtime::call_dynamic_at(");
    require_stopped_call("emit_signal_arguments", "signal-last",
                         "gdpp::runtime::emit_local_signal_at(");
}

TEST_CASE("runtime failure boundaries retain precise script source locations") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("runtime_locations.gd", "extends RefCounted\n"
                                                 "signal completed(value)\n"
                                                 "func inspect(target: Variant) -> void:\n"
                                                 "    var property: Variant = target.value\n"
                                                 "    var keyed: Variant = target[\"key\"]\n"
                                                 "    target.value = keyed\n"
                                                 "    target[\"key\"] = property\n"
                                                 "    target.accept(property)\n"
                                                 "    var converted: int = int(target)\n"
                                                 "    var typed: Array[int] = target\n"
                                                 "    for entry in target:\n"
                                                 "        property = entry\n"
                                                 "    var divided := converted / 0\n"
                                                 "func invoke(callback: Callable) -> void:\n"
                                                 "    callback.call(1)\n"
                                                 "    completed.emit(2)\n");

    REQUIRE(result.success);
    const auto& source = result.unit.source;
    const std::string location{"gdpp::runtime::ScriptSourceLocation{_gdpp_source_path, "};
    REQUIRE(source.find("gdpp::runtime::get_named(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::get_key(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::set_named(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::set_key(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::explicit_variant_cast<int64_t>(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::strict_typed_storage<godot::TypedArray<int64_t>>(") !=
            std::string::npos);
    REQUIRE(source.find("gdpp::runtime::iter_init(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::integer_divide(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_callable_at(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::emit_local_signal_at(") != std::string::npos);
    REQUIRE(std::count(source.begin(), source.end(), '\n') > 10);

    std::size_t located_boundary_count = 0;
    for (auto position = source.find(location); position != std::string::npos;
         position = source.find(location, position + location.size()))
        ++located_boundary_count;
    REQUIRE(located_boundary_count >= std::size_t{12});
}

TEST_CASE("compiler handles generated logical guard chains with bounded stack depth") {
    std::string source{"extends RefCounted\nfunc validate() -> bool:\n    return true"};
    constexpr std::size_t operand_count = 1024;
    for (std::size_t index = 1; index < operand_count; ++index)
        source += " and true";
    source += '\n';

    const auto result = gdpp::Compiler{}.compile("large_logical_chain.gd", source);

    REQUIRE(result.success);
    REQUIRE(result.metrics.ast_expression_count >= operand_count);
    REQUIRE(result.unit.source.find("bool GDPPNative_LargeLogicalChain::validate()") !=
            std::string::npos);
}

TEST_CASE("flow refinement remains bounded across generated logical guard chains") {
    std::string source{
        "extends RefCounted\nfunc validate(value: Variant) -> bool:\n    return value is int"};
    constexpr std::size_t comparison_count = 512;
    for (std::size_t index = 0; index < comparison_count; ++index)
        source += " and value >= " + std::to_string(index);
    source += '\n';

    const auto result = gdpp::Compiler{}.compile("large_flow_chain.gd", source);

    REQUIRE(result.success);
    REQUIRE(result.metrics.ast_expression_count >= comparison_count * 2U);
    REQUIRE(result.unit.source.find("gdpp::runtime::strict_builtin_storage<int64_t>("
                                    "gdpp::runtime::to_variant(value), godot::Variant::INT") !=
            std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::binary") == std::string::npos);
}

TEST_CASE("compiler analyzes legal arithmetic chains without recursive stack growth") {
    std::string source{"extends RefCounted\nfunc total() -> int:\n    return 1"};
    constexpr std::size_t operand_count = 96;
    for (std::size_t index = 1; index < operand_count; ++index)
        source += " + 1";
    source += '\n';

    const auto result = gdpp::Compiler{}.compile("large_arithmetic_chain.gd", source);

    REQUIRE(result.success);
    REQUIRE(result.metrics.ast_expression_count >= operand_count);
    REQUIRE(result.optimization.constants_folded >= operand_count - 1U);
}

TEST_CASE("compiler owns enough worker stack for deeply nested postfix semantics") {
    std::string source{
        "extends RefCounted\nfunc lookup(value: Variant) -> Variant:\n    return value"};
    constexpr std::size_t access_count = 48;
    for (std::size_t index = 0; index < access_count; ++index)
        source += ".get(\"next\")";
    source += '\n';

    gdpp::CompileResult result;
    std::thread embedding_worker{
        [&]() { result = gdpp::Compiler{}.compile("nested_postfix.gd", source); }};
    embedding_worker.join();

    REQUIRE(result.success);
    REQUIRE(result.metrics.ast_expression_count >= access_count * 2U);
    REQUIRE(result.unit.source.find("GDPPNative_NestedPostfix::lookup") != std::string::npos);
}

TEST_CASE("instance Godot methods cannot be called through type references") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("invalid_static.gd", "extends Node\n"
                                                              "func invalid() -> void:\n"
                                                              "    Vector2.length()\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.source.empty());
}

TEST_CASE("compiler lowers constant GDScript utility functions through the native runtime") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(
        "language_utilities.gd",
        "extends Node\n"
        "func inspect(value: Variant, target: Node) -> Array:\n"
        "    return [convert(\"42\", TYPE_INT), type_exists(&\"Node\"), char(0x1f642), "
        "ord(\"🙂\"), Color8(255, 128, 0), Color8(255, 128, 0, 64), "
        "is_instance_of(value, TYPE_INT), is_instance_of(target, Node)]\n");

    REQUIRE(result.success);
    REQUIRE(result.unit.source.find("gdpp::runtime::convert_value") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::type_exists") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::character") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::ordinal") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::color8") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::is_instance_of") != std::string::npos);
    REQUIRE(result.unit.source.find("gdpp::runtime::to_variant(godot::StringName(\"Node\"))") !=
            std::string::npos);
}

TEST_CASE("compiler rejects invalid GDScript utility argument contracts before codegen") {
    const gdpp::Compiler compiler;
    const auto result =
        compiler.compile("invalid_language_utilities.gd", "func invalid() -> void:\n"
                                                          "    char(\"A\")\n"
                                                          "    ord(1)\n"
                                                          "    Color8(1, 2)\n"
                                                          "    type_exists(7)\n"
                                                          "    is_instance_of(1, [])\n"
                                                          "    inst_to_dict(1)\n"
                                                          "    dict_to_inst([])\n");

    REQUIRE(!result.success);
    REQUIRE(result.unit.source.empty());
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4144"; }));
}

TEST_CASE("compiler reports deterministic stage and IR size metrics") {
    const gdpp::Compiler compiler;
    const auto result = compiler.compile("metrics.gd", "extends Node\n"
                                                       "func sum(limit: int) -> int:\n"
                                                       "    var total := 0\n"
                                                       "    for value in range(limit):\n"
                                                       "        total += value\n"
                                                       "    return total\n");

    REQUIRE(result.success);
    REQUIRE(result.metrics.total_ns > 0);
    REQUIRE(result.metrics.lex_ns > 0);
    REQUIRE(result.metrics.parse_ns > 0);
    REQUIRE(result.metrics.token_count > 10);
    REQUIRE(result.metrics.ast_expression_count > 0);
    REQUIRE(result.metrics.ast_statement_count >= std::size_t{4});
    REQUIRE(result.metrics.hir_expression_count > 0);
    REQUIRE(result.metrics.hir_statement_count >= std::size_t{4});
    REQUIRE(result.metrics.mir_function_count == std::size_t{1});
    REQUIRE(result.metrics.mir_block_count >= std::size_t{4});
    REQUIRE(result.metrics.mir_instruction_count > 0);
}
