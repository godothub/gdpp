#include "support/test.hpp"

#include "gdpp/semantic/godot_api.hpp"

#include <string>
#include <string_view>

TEST_CASE("Godot API metadata exposes the 4.4 commercial baseline") {
    const auto& api = gdpp::GodotApi::instance();

    REQUIRE_EQ(api.version(), std::string_view{"4.4.0"});
    REQUIRE(api.class_count() > std::size_t{900});
    REQUIRE(api.method_count() > std::size_t{16000});
    REQUIRE(api.property_count() > std::size_t{3800});
    REQUIRE(api.signal_count() > std::size_t{300});
}

TEST_CASE("Godot API lookup follows inherited engine signals") {
    const auto& api = gdpp::GodotApi::instance();

    const auto* resized = api.find_signal("Button", "resized");
    const auto* pressed = api.find_signal("Button", "pressed");
    REQUIRE(resized != nullptr);
    REQUIRE_EQ(std::string_view{resized->owner}, std::string_view{"Control"});
    REQUIRE(pressed != nullptr);
    REQUIRE_EQ(std::string_view{pressed->owner}, std::string_view{"BaseButton"});
    const auto* input_event = api.find_signal("Control", "gui_input");
    REQUIRE(input_event != nullptr);
    REQUIRE_EQ(input_event->argument_count, std::uint16_t{1});
    REQUIRE_EQ(std::string_view{api.argument(*input_event, 0)->type},
               std::string_view{"InputEvent"});
    REQUIRE(api.argument(*input_event, 1) == nullptr);
}

TEST_CASE("generated Godot metadata validates every record and argument range") {
    for (const auto version : {gdpp::GodotVersion::v4_4, gdpp::GodotVersion::v4_5,
                               gdpp::GodotVersion::v4_6, gdpp::GodotVersion::v4_7}) {
        const auto& api = gdpp::GodotApi::for_version(version);
        REQUIRE(api.validate_metadata());
        REQUIRE(api.argument_count() > api.method_count());
        REQUIRE(api.constructor_count() > std::size_t{150});
    }
}

TEST_CASE("Godot API lookup follows inherited class constants and named enums") {
    const auto& api = gdpp::GodotApi::for_version(gdpp::GodotVersion::v4_5);

    REQUIRE(api.class_constant_count() > std::size_t{3000});
    const auto* transform_changed =
        api.find_class_constant("Camera3D", "NOTIFICATION_TRANSFORM_CHANGED");
    const auto* ready = api.find_class_constant("Button", "NOTIFICATION_READY");
    const auto* process_mode =
        api.find_class_enum_value("Node", "ProcessMode", "PROCESS_MODE_INHERIT");
    REQUIRE(transform_changed != nullptr);
    REQUIRE_EQ(std::string_view{transform_changed->owner}, std::string_view{"Node3D"});
    REQUIRE_EQ(transform_changed->value, std::int64_t{2000});
    REQUIRE(ready != nullptr);
    REQUIRE_EQ(std::string_view{ready->owner}, std::string_view{"Node"});
    REQUIRE_EQ(ready->value, std::int64_t{13});
    REQUIRE(api.has_class_enum("Node", "ProcessMode"));
    REQUIRE(process_mode != nullptr);
    REQUIRE_EQ(process_mode->value, std::int64_t{0});
}

TEST_CASE("Godot API registry gates 4.7 additions by target version") {
    const auto& api_4_6 = gdpp::GodotApi::for_version(gdpp::GodotVersion::v4_6);
    const auto& api_4_7 = gdpp::GodotApi::for_version(gdpp::GodotVersion::v4_7);

    REQUIRE_EQ(api_4_7.version(), std::string_view{"4.7.0"});
    REQUIRE(api_4_6.find_class("AccessibilityServer") == nullptr);
    REQUIRE(api_4_7.find_class("AccessibilityServer") != nullptr);
    REQUIRE(api_4_6.find_method("CodeEdit", "join_lines") == nullptr);
    REQUIRE(api_4_7.find_method("CodeEdit", "join_lines") != nullptr);
    REQUIRE(api_4_7.class_count() > api_4_6.class_count());
}

TEST_CASE("Godot target versions parse patches and reject unsupported minors") {
    REQUIRE_EQ(gdpp::parse_godot_version("4.4.2").value(), gdpp::GodotVersion::v4_4);
    REQUIRE_EQ(gdpp::parse_godot_version("4.5").value(), gdpp::GodotVersion::v4_5);
    REQUIRE_EQ(gdpp::parse_godot_version("4.6.3").value(), gdpp::GodotVersion::v4_6);
    REQUIRE_EQ(gdpp::parse_godot_version("4.7").value(), gdpp::GodotVersion::v4_7);
    REQUIRE(!gdpp::parse_godot_version("4.3"));
    REQUIRE(gdpp::supports_godot_version(4, 4));
    REQUIRE(gdpp::supports_godot_version(4, 6));
    REQUIRE(!gdpp::supports_godot_version(4, 3));
    REQUIRE(!gdpp::supports_godot_version(4, 8));
    REQUIRE_EQ(gdpp::best_godot_version(4, 4).value(), gdpp::GodotVersion::v4_4);
    REQUIRE_EQ(gdpp::best_godot_version(4, 5).value(), gdpp::GodotVersion::v4_5);
    REQUIRE_EQ(gdpp::best_godot_version(4, 6).value(), gdpp::GodotVersion::v4_6);
    REQUIRE(!gdpp::best_godot_version(4, 8));
    REQUIRE(!gdpp::best_godot_version(5, 0));
}

TEST_CASE("Godot API lookup follows engine inheritance and method arity") {
    const auto& api = gdpp::GodotApi::instance();
    const auto* method = api.find_method("Node2D", "queue_free");
    const auto* add_child = api.find_method("Node", "add_child");

    REQUIRE(api.inherits("Node2D", "Object"));
    REQUIRE(method != nullptr);
    REQUIRE_EQ(std::string_view{method->owner}, std::string_view{"Node"});
    REQUIRE(add_child != nullptr);
    REQUIRE_EQ(add_child->required_arguments, std::uint16_t{1});
    REQUIRE_EQ(add_child->maximum_arguments, std::uint16_t{3});
    const auto* child_argument = api.argument(*add_child, 0);
    const auto* volume_method = api.find_method("AudioStreamPlayer", "set_volume_db");
    REQUIRE(volume_method != nullptr);
    const auto* volume_argument = api.argument(*volume_method, 0);
    REQUIRE(child_argument != nullptr);
    REQUIRE_EQ(std::string_view{child_argument->type}, std::string_view{"Node"});
    REQUIRE(volume_argument != nullptr);
    REQUIRE_EQ(std::string_view{volume_argument->meta}, std::string_view{"float"});
}

TEST_CASE("Godot API lookup resolves properties and builtin value methods") {
    const auto& api = gdpp::GodotApi::instance();
    const auto* position = api.find_property("Node2D", "position");
    const auto* length = api.find_method("Vector2", "length");

    REQUIRE(position != nullptr);
    REQUIRE_EQ(std::string_view{position->getter}, std::string_view{"get_position"});
    REQUIRE_EQ(std::string_view{position->setter}, std::string_view{"set_position"});
    REQUIRE(length != nullptr);
    REQUIRE_EQ(gdpp::type_from_godot_api(length->return_type).kind, gdpp::TypeKind::floating);
    const auto typed_array = gdpp::type_from_godot_api("typedarray::Node");
    const auto typed_dictionary = gdpp::type_from_godot_api("typeddictionary::String;Variant");
    const auto encoded_resource_array =
        gdpp::type_from_godot_api("typedarray::24/17:CompositorEffect");
    const auto encoded_dictionary_array = gdpp::type_from_godot_api("typedarray::27/0:");
    REQUIRE_EQ(typed_array.kind, gdpp::TypeKind::array);
    REQUIRE_EQ(typed_array.name, std::string{"Array[Node]"});
    REQUIRE_EQ(typed_dictionary.kind, gdpp::TypeKind::dictionary);
    REQUIRE_EQ(typed_dictionary.name, std::string{"Dictionary[String, Variant]"});
    REQUIRE_EQ(encoded_resource_array.name, std::string{"Array[CompositorEffect]"});
    REQUIRE_EQ(encoded_dictionary_array.name, std::string{"Array[Dictionary]"});
    REQUIRE_EQ(gdpp::type_from_godot_api("enum::Error").kind, gdpp::TypeKind::integer);
}

TEST_CASE("Godot API properties use accessor ABI for every inspector resource alternative") {
    struct Alternative {
        std::string_view owner;
        std::string_view property;
        std::string_view accessor_type;
    };
    constexpr Alternative alternatives[]{
        {"World3D", "camera_attributes", "CameraAttributes"},
        {"TileData", "material", "Material"},
        {"PointLight2D", "texture", "Texture2D"},
        {"GPUParticles2D", "process_material", "Material"},
        {"VisualShaderNodeCubemap", "cube_map", "TextureLayered"},
        {"VisualShaderNodeTexture2DArray", "texture_array", "TextureLayered"},
        {"FogVolume", "material", "Material"},
        {"WorldEnvironment", "camera_attributes", "CameraAttributes"},
        {"GPUParticles3D", "process_material", "Material"},
        {"Sky", "sky_material", "Material"},
        {"LightmapGI", "camera_attributes", "CameraAttributes"},
        {"VoxelGI", "camera_attributes", "CameraAttributes"},
        {"Decal", "texture_albedo", "Texture2D"},
        {"Decal", "texture_normal", "Texture2D"},
        {"Decal", "texture_orm", "Texture2D"},
        {"Decal", "texture_emission", "Texture2D"},
        {"Light3D", "light_projector", "Texture2D"},
        {"Environment", "adjustment_color_correction", "Texture"},
        {"Camera3D", "attributes", "CameraAttributes"},
        {"GeometryInstance3D", "material_override", "Material"},
        {"GeometryInstance3D", "material_overlay", "Material"},
        {"CSGTorus3D", "material", "Material"},
        {"CSGSphere3D", "material", "Material"},
        {"CSGPolygon3D", "material", "Material"},
        {"CSGMesh3D", "mesh", "Mesh"},
        {"CSGMesh3D", "material", "Material"},
        {"CSGCylinder3D", "material", "Material"},
        {"CSGBox3D", "material", "Material"},
        {"PrimitiveMesh", "material", "Material"},
        {"ParticleProcessMaterial", "orbit_velocity_curve", "Texture2D"},
        {"ParticleProcessMaterial", "scale_curve", "Texture2D"},
        {"ParticleProcessMaterial", "scale_over_velocity_curve", "Texture2D"},
        {"CanvasItem", "material", "Material"},
    };

    for (const auto version : {gdpp::GodotVersion::v4_4, gdpp::GodotVersion::v4_5,
                               gdpp::GodotVersion::v4_6, gdpp::GodotVersion::v4_7}) {
        const auto& api = gdpp::GodotApi::for_version(version);
        for (const auto& alternative : alternatives) {
            const auto* property = api.find_property(alternative.owner, alternative.property);
            REQUIRE(property != nullptr);
            REQUIRE_EQ(
                api.property_getter_type(*property),
                (gdpp::Type{gdpp::TypeKind::object, std::string{alternative.accessor_type}}));
            REQUIRE_EQ(
                api.property_setter_type(*property),
                (gdpp::Type{gdpp::TypeKind::object, std::string{alternative.accessor_type}}));
        }
    }

    const auto& api_4_7 = gdpp::GodotApi::for_version(gdpp::GodotVersion::v4_7);
    const auto* area_texture = api_4_7.find_property("AreaLight3D", "area_texture");
    REQUIRE(area_texture != nullptr);
    REQUIRE_EQ(api_4_7.property_getter_type(*area_texture),
               (gdpp::Type{gdpp::TypeKind::object, "Texture2D"}));
    REQUIRE_EQ(api_4_7.property_setter_type(*area_texture),
               (gdpp::Type{gdpp::TypeKind::object, "Texture2D"}));
}

TEST_CASE("Godot API independently validates every property read and write contract") {
    for (const auto version : {gdpp::GodotVersion::v4_4, gdpp::GodotVersion::v4_5,
                               gdpp::GodotVersion::v4_6, gdpp::GodotVersion::v4_7}) {
        const auto& api = gdpp::GodotApi::for_version(version);
        std::size_t accessor_overrides = 0;
        for (std::size_t index = 0; index < api.property_count(); ++index) {
            const auto* property = api.property(index);
            REQUIRE(property != nullptr);
            const auto metadata_type = gdpp::type_from_godot_api(property->type);
            const auto getter_type = api.property_getter_type(*property);
            const auto setter_type = api.property_setter_type(*property);

            if (!property->direct && property->getter[0] != '\0') {
                const auto* getter = api.find_method(property->owner, property->getter);
                if (getter && getter->return_type[0] != '\0') {
                    REQUIRE_EQ(getter_type, gdpp::type_from_godot_api(getter->return_type));
                } else {
                    REQUIRE_EQ(getter_type, metadata_type);
                }
            } else {
                REQUIRE_EQ(getter_type, metadata_type);
            }

            if (!property->direct && property->setter[0] != '\0') {
                const auto* setter = api.find_method(property->owner, property->setter);
                if (setter) {
                    const auto value_index =
                        property->index >= 0 ? std::size_t{1} : std::size_t{0};
                    const auto* value = api.argument(*setter, value_index);
                    REQUIRE(value != nullptr);
                    REQUIRE_EQ(setter_type, gdpp::type_from_godot_api(value->type));
                } else {
                    REQUIRE_EQ(setter_type, metadata_type);
                }
            } else {
                REQUIRE_EQ(setter_type, metadata_type);
            }

            if (getter_type != metadata_type || setter_type != metadata_type)
                ++accessor_overrides;
        }
        REQUIRE(api.property(api.property_count()) == nullptr);
        REQUIRE(accessor_overrides >= std::size_t{33});
    }
}

TEST_CASE("Godot API metadata resolves global engine singletons") {
    const auto& api = gdpp::GodotApi::instance();
    const auto* input = api.find_singleton("Input");

    REQUIRE(api.singleton_count() > std::size_t{30});
    REQUIRE(input != nullptr);
    REQUIRE_EQ(std::string_view{input->type}, std::string_view{"Input"});
    REQUIRE(api.find_method(input->type, "is_action_pressed") != nullptr);
}

TEST_CASE("Godot 4.7 metadata exposes language utilities globals enums and operators") {
    const auto& api = gdpp::GodotApi::for_version(gdpp::GodotVersion::v4_7);

    REQUIRE_EQ(api.utility_function_count(), std::size_t{114});
    REQUIRE_EQ(api.global_constant_count(), std::size_t{11});
    REQUIRE(api.global_enum_value_count() > std::size_t{100});
    REQUIRE(api.builtin_operator_count() > std::size_t{500});

    const auto* clamp = api.find_utility_function("clampf");
    const auto* print = api.find_utility_function("print");
    const auto* str = api.find_utility_function("str");
    const auto* maximum = api.find_utility_function("max");
    REQUIRE(clamp != nullptr);
    REQUIRE_EQ(clamp->required_arguments, std::uint16_t{3});
    REQUIRE_EQ(std::string_view{clamp->return_type}, std::string_view{"float"});
    REQUIRE(clamp->is_constant);
    REQUIRE(print != nullptr);
    REQUIRE(str != nullptr);
    REQUIRE(maximum != nullptr);
    REQUIRE(!print->is_constant);
    REQUIRE_EQ(print->required_arguments, std::uint16_t{0});
    REQUIRE_EQ(str->required_arguments, std::uint16_t{0});
    REQUIRE_EQ(maximum->required_arguments, std::uint16_t{2});
    REQUIRE(api.find_global_constant("INT64_MAX") != nullptr);
    REQUIRE(api.has_global_enum("Side"));
    REQUIRE_EQ(api.find_global_enum_value("Side", "SIDE_LEFT")->value, std::int64_t{0});
    REQUIRE(api.find_builtin_operator("Vector3", "+", "Vector3") != nullptr);
    REQUIRE_EQ(std::string_view{api.find_builtin_operator("int", "**", "float")->return_type},
               std::string_view{"float"});
    REQUIRE(api.builtin_constant_count() > 0);
    const auto* gray = api.find_builtin_constant("Color", "GRAY");
    REQUIRE(gray != nullptr);
    REQUIRE_EQ(std::string_view{gray->type}, std::string_view{"Color"});
    REQUIRE_EQ(std::string_view{gray->value},
               std::string_view{"Color(0.74509805, 0.74509805, 0.74509805, 1)"});
    REQUIRE(api.is_editor_class("EditorPlugin"));
    REQUIRE(api.is_editor_class("EditorDebuggerPlugin"));
    REQUIRE(!api.is_editor_class("Node"));
}
