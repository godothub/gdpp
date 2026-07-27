#include "gdpp/semantic/type.hpp"

#include "gdpp/semantic/conversion.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace gdpp {

bool Type::is_numeric() const noexcept {
    return kind == TypeKind::integer || kind == TypeKind::floating || kind == TypeKind::enumeration;
}

bool Type::is_dynamic() const noexcept {
    return kind == TypeKind::unknown || kind == TypeKind::variant;
}

bool Type::is_packed_array() const noexcept {
    if (kind != TypeKind::builtin)
        return false;
    static const std::unordered_set<std::string> packed_arrays{
        "PackedByteArray",    "PackedInt32Array",   "PackedInt64Array",   "PackedFloat32Array",
        "PackedFloat64Array", "PackedStringArray",  "PackedVector2Array", "PackedVector3Array",
        "PackedColorArray",   "PackedVector4Array",
    };
    return packed_arrays.find(name) != packed_arrays.end();
}

Nullability Type::nullability() const noexcept {
    switch (kind) {
    case TypeKind::unknown:
    case TypeKind::variant:
    case TypeKind::object:
    case TypeKind::script_resource:
        return Nullability::nullable;
    case TypeKind::nil:
        return Nullability::null_only;
    case TypeKind::void_type:
        return Nullability::not_a_value;
    case TypeKind::boolean:
    case TypeKind::integer:
    case TypeKind::floating:
    case TypeKind::string:
    case TypeKind::string_name:
    case TypeKind::array:
    case TypeKind::dictionary:
    case TypeKind::enumeration:
    case TypeKind::builtin:
        return Nullability::non_nullable;
    }
    return Nullability::not_a_value;
}

TruthinessKind Type::truthiness() const noexcept {
    switch (kind) {
    case TypeKind::unknown:
    case TypeKind::variant:
        return TruthinessKind::dynamic_value;
    case TypeKind::nil:
        return TruthinessKind::always_false;
    case TypeKind::object:
    case TypeKind::script_resource:
        return TruthinessKind::object_validity;
    case TypeKind::void_type:
        return TruthinessKind::invalid;
    case TypeKind::boolean:
    case TypeKind::integer:
    case TypeKind::floating:
    case TypeKind::string:
    case TypeKind::string_name:
    case TypeKind::array:
    case TypeKind::dictionary:
    case TypeKind::enumeration:
    case TypeKind::builtin:
        return TruthinessKind::zero_value;
    }
    return TruthinessKind::invalid;
}

OwnershipKind Type::ownership() const noexcept {
    if (kind == TypeKind::variant || kind == TypeKind::unknown)
        return OwnershipKind::dynamic;
    if (kind == TypeKind::array || kind == TypeKind::dictionary || is_packed_array())
        return OwnershipKind::shared_container;
    if (kind == TypeKind::object || kind == TypeKind::script_resource)
        return OwnershipKind::object_reference;
    return OwnershipKind::value;
}

bool Type::accepts_null() const noexcept {
    return nullability() == Nullability::nullable || nullability() == Nullability::null_only;
}

bool Type::is_value() const noexcept { return nullability() != Nullability::not_a_value; }

std::string Type::display_name() const {
    if (!name.empty())
        return name;
    switch (kind) {
    case TypeKind::unknown:
        return "unknown";
    case TypeKind::variant:
        return "Variant";
    case TypeKind::nil:
        return "null";
    case TypeKind::boolean:
        return "bool";
    case TypeKind::integer:
        return "int";
    case TypeKind::floating:
        return "float";
    case TypeKind::string:
        return "String";
    case TypeKind::string_name:
        return "StringName";
    case TypeKind::array:
        return "Array";
    case TypeKind::dictionary:
        return "Dictionary";
    case TypeKind::enumeration:
        return name.empty() ? "enum" : name;
    case TypeKind::script_resource:
        return name.empty() ? "Script" : "Script[" + name + "]";
    case TypeKind::builtin:
        return "builtin";
    case TypeKind::object:
        return "Object";
    case TypeKind::void_type:
        return "void";
    }
    return "unknown";
}

Type type_from_annotation(const std::string& annotation) {
    static const std::unordered_map<std::string, TypeKind> builtins{
        {"Variant", TypeKind::variant}, {"bool", TypeKind::boolean},
        {"int", TypeKind::integer},     {"float", TypeKind::floating},
        {"String", TypeKind::string},   {"StringName", TypeKind::string_name},
        {"Array", TypeKind::array},     {"Dictionary", TypeKind::dictionary},
        {"void", TypeKind::void_type},
    };
    if (annotation.rfind("Array[", 0) == 0)
        return {TypeKind::array, annotation};
    if (annotation.rfind("Dictionary[", 0) == 0)
        return {TypeKind::dictionary, annotation};
    const auto found = builtins.find(annotation);
    if (found != builtins.end())
        return {found->second, annotation};
    static const std::unordered_set<std::string> value_types{
        "Vector2",
        "Vector2i",
        "Rect2",
        "Rect2i",
        "Vector3",
        "Vector3i",
        "Transform2D",
        "Vector4",
        "Vector4i",
        "Plane",
        "Quaternion",
        "AABB",
        "Basis",
        "Transform3D",
        "Projection",
        "Color",
        "NodePath",
        "RID",
        "Callable",
        "Signal",
        "PackedByteArray",
        "PackedInt32Array",
        "PackedInt64Array",
        "PackedFloat32Array",
        "PackedFloat64Array",
        "PackedStringArray",
        "PackedVector2Array",
        "PackedVector3Array",
        "PackedColorArray",
        "PackedVector4Array",
    };
    if (value_types.find(annotation) != value_types.end()) {
        return {TypeKind::builtin, annotation};
    }
    return {TypeKind::object, annotation};
}

std::optional<VariantType> variant_type_of(const Type& type) noexcept {
    switch (type.kind) {
    case TypeKind::nil:
        return VariantType::nil;
    case TypeKind::boolean:
        return VariantType::boolean;
    case TypeKind::integer:
    case TypeKind::enumeration:
        return VariantType::integer;
    case TypeKind::floating:
        return VariantType::floating;
    case TypeKind::string:
        return VariantType::string;
    case TypeKind::string_name:
        return VariantType::string_name;
    case TypeKind::array:
        return VariantType::array;
    case TypeKind::dictionary:
        return VariantType::dictionary;
    case TypeKind::object:
    case TypeKind::script_resource:
        return VariantType::object;
    case TypeKind::builtin:
        break;
    case TypeKind::unknown:
    case TypeKind::variant:
    case TypeKind::void_type:
        return std::nullopt;
    }

    static const std::unordered_map<std::string, VariantType> builtin_types{
        {"Vector2", VariantType::vector2},
        {"Vector2i", VariantType::vector2i},
        {"Rect2", VariantType::rect2},
        {"Rect2i", VariantType::rect2i},
        {"Vector3", VariantType::vector3},
        {"Vector3i", VariantType::vector3i},
        {"Transform2D", VariantType::transform2d},
        {"Vector4", VariantType::vector4},
        {"Vector4i", VariantType::vector4i},
        {"Plane", VariantType::plane},
        {"Quaternion", VariantType::quaternion},
        {"AABB", VariantType::aabb},
        {"Basis", VariantType::basis},
        {"Transform3D", VariantType::transform3d},
        {"Projection", VariantType::projection},
        {"Color", VariantType::color},
        {"NodePath", VariantType::node_path},
        {"RID", VariantType::rid},
        {"Callable", VariantType::callable},
        {"Signal", VariantType::signal},
        {"PackedByteArray", VariantType::packed_byte_array},
        {"PackedInt32Array", VariantType::packed_int32_array},
        {"PackedInt64Array", VariantType::packed_int64_array},
        {"PackedFloat32Array", VariantType::packed_float32_array},
        {"PackedFloat64Array", VariantType::packed_float64_array},
        {"PackedStringArray", VariantType::packed_string_array},
        {"PackedVector2Array", VariantType::packed_vector2_array},
        {"PackedVector3Array", VariantType::packed_vector3_array},
        {"PackedColorArray", VariantType::packed_color_array},
        {"PackedVector4Array", VariantType::packed_vector4_array},
    };
    const auto found = builtin_types.find(type.name);
    return found == builtin_types.end() ? std::nullopt
                                        : std::optional<VariantType>{found->second};
}

std::string_view variant_type_name(const VariantType type) noexcept {
    switch (type) {
    case VariantType::nil:
        return "Nil";
    case VariantType::boolean:
        return "bool";
    case VariantType::integer:
        return "int";
    case VariantType::floating:
        return "float";
    case VariantType::string:
        return "String";
    case VariantType::vector2:
        return "Vector2";
    case VariantType::vector2i:
        return "Vector2i";
    case VariantType::rect2:
        return "Rect2";
    case VariantType::rect2i:
        return "Rect2i";
    case VariantType::vector3:
        return "Vector3";
    case VariantType::vector3i:
        return "Vector3i";
    case VariantType::transform2d:
        return "Transform2D";
    case VariantType::vector4:
        return "Vector4";
    case VariantType::vector4i:
        return "Vector4i";
    case VariantType::plane:
        return "Plane";
    case VariantType::quaternion:
        return "Quaternion";
    case VariantType::aabb:
        return "AABB";
    case VariantType::basis:
        return "Basis";
    case VariantType::transform3d:
        return "Transform3D";
    case VariantType::projection:
        return "Projection";
    case VariantType::color:
        return "Color";
    case VariantType::string_name:
        return "StringName";
    case VariantType::node_path:
        return "NodePath";
    case VariantType::rid:
        return "RID";
    case VariantType::object:
        return "Object";
    case VariantType::callable:
        return "Callable";
    case VariantType::signal:
        return "Signal";
    case VariantType::dictionary:
        return "Dictionary";
    case VariantType::array:
        return "Array";
    case VariantType::packed_byte_array:
        return "PackedByteArray";
    case VariantType::packed_int32_array:
        return "PackedInt32Array";
    case VariantType::packed_int64_array:
        return "PackedInt64Array";
    case VariantType::packed_float32_array:
        return "PackedFloat32Array";
    case VariantType::packed_float64_array:
        return "PackedFloat64Array";
    case VariantType::packed_string_array:
        return "PackedStringArray";
    case VariantType::packed_vector2_array:
        return "PackedVector2Array";
    case VariantType::packed_vector3_array:
        return "PackedVector3Array";
    case VariantType::packed_color_array:
        return "PackedColorArray";
    case VariantType::packed_vector4_array:
        return "PackedVector4Array";
    }
    return "Unknown";
}

Type packed_array_element_type(const Type& packed_array) {
    static const std::unordered_map<std::string, Type> elements{
        {"PackedByteArray", {TypeKind::integer, "int"}},
        {"PackedInt32Array", {TypeKind::integer, "int"}},
        {"PackedInt64Array", {TypeKind::integer, "int"}},
        {"PackedFloat32Array", {TypeKind::floating, "float"}},
        {"PackedFloat64Array", {TypeKind::floating, "float"}},
        {"PackedStringArray", {TypeKind::string, "String"}},
        {"PackedVector2Array", {TypeKind::builtin, "Vector2"}},
        {"PackedVector3Array", {TypeKind::builtin, "Vector3"}},
        {"PackedColorArray", {TypeKind::builtin, "Color"}},
        {"PackedVector4Array", {TypeKind::builtin, "Vector4"}},
    };
    const auto found = elements.find(packed_array.name);
    return found == elements.end() ? Type{TypeKind::variant, "Variant"} : found->second;
}

std::optional<std::vector<std::string>>
generic_type_arguments(const std::string_view type_name, const std::string_view container_name,
                       const std::size_t expected_arguments) {
    if (type_name.size() <= container_name.size() + 2U ||
        type_name.compare(0, container_name.size(), container_name) != 0 ||
        type_name[container_name.size()] != '[' || type_name.back() != ']') {
        return std::nullopt;
    }
    const auto trim = [](std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
            value.remove_suffix(1);
        }
        return value;
    };
    auto content =
        type_name.substr(container_name.size() + 1U, type_name.size() - container_name.size() - 2U);
    std::vector<std::string> arguments;
    std::size_t begin = 0;
    std::size_t depth = 0;
    for (std::size_t index = 0; index <= content.size(); ++index) {
        const bool at_end = index == content.size();
        const auto character = at_end ? '\0' : content[index];
        if (!at_end && character == '[') {
            ++depth;
            continue;
        }
        if (!at_end && character == ']') {
            if (depth == 0)
                return std::nullopt;
            --depth;
            continue;
        }
        if (!at_end && (character != ',' || depth != 0))
            continue;
        const auto argument = trim(content.substr(begin, index - begin));
        if (argument.empty())
            return std::nullopt;
        arguments.emplace_back(argument);
        begin = index + 1U;
    }
    if (depth != 0 || arguments.size() != expected_arguments)
        return std::nullopt;
    return arguments;
}

bool ContainerTypeDescriptor::has_runtime_constraint() const noexcept {
    return std::any_of(arguments.begin(), arguments.end(),
                       [](const std::string& argument) { return argument != "Variant"; });
}

std::optional<ContainerTypeDescriptor> describe_container_type(const Type& type) {
    if (type.kind == TypeKind::array) {
        const auto arguments = generic_type_arguments(type.name, "Array", 1);
        if (arguments)
            return ContainerTypeDescriptor{ContainerTypeKind::array, *arguments};
    }
    if (type.kind == TypeKind::dictionary) {
        const auto arguments = generic_type_arguments(type.name, "Dictionary", 2);
        if (arguments)
            return ContainerTypeDescriptor{ContainerTypeKind::dictionary, *arguments};
    }
    return std::nullopt;
}

bool is_explicitly_typed_container(const Type& type) {
    return describe_container_type(type).has_value();
}

bool is_assignable(const Type& target, const Type& source) noexcept {
    return is_implicitly_convertible(target, source);
}

} // namespace gdpp
