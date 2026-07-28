#include "gdpp/semantic/conversion.hpp"

#include <initializer_list>

namespace gdpp {
namespace {

bool is_one_of(const VariantType value, const std::initializer_list<VariantType> types) noexcept {
    for (const auto type : types) {
        if (value == type)
            return true;
    }
    return false;
}

bool strict_variant_conversion(const VariantType target, const VariantType source) noexcept {
    if (target == source)
        return true;
    if (source == VariantType::nil)
        return target == VariantType::object;

    switch (target) {
    case VariantType::boolean:
        return is_one_of(source, {VariantType::integer, VariantType::floating});
    case VariantType::integer:
        return is_one_of(source, {VariantType::boolean, VariantType::floating});
    case VariantType::floating:
        return is_one_of(source, {VariantType::boolean, VariantType::integer});
    case VariantType::string:
        return is_one_of(source, {VariantType::node_path, VariantType::string_name});
    case VariantType::vector2:
        return source == VariantType::vector2i;
    case VariantType::vector2i:
        return source == VariantType::vector2;
    case VariantType::rect2:
        return source == VariantType::rect2i;
    case VariantType::rect2i:
        return source == VariantType::rect2;
    case VariantType::vector3:
        return source == VariantType::vector3i;
    case VariantType::vector3i:
        return source == VariantType::vector3;
    case VariantType::transform2d:
        return source == VariantType::transform3d;
    case VariantType::vector4:
        return source == VariantType::vector4i;
    case VariantType::vector4i:
        return source == VariantType::vector4;
    case VariantType::quaternion:
        return source == VariantType::basis;
    case VariantType::basis:
        return source == VariantType::quaternion;
    case VariantType::transform3d:
        return is_one_of(source, {VariantType::transform2d, VariantType::quaternion,
                                  VariantType::basis, VariantType::projection});
    case VariantType::projection:
        return source == VariantType::transform3d;
    case VariantType::color:
        return is_one_of(source, {VariantType::string, VariantType::integer});
    case VariantType::string_name:
    case VariantType::node_path:
        return source == VariantType::string;
    case VariantType::rid:
        return source == VariantType::object;
    case VariantType::object:
        return false;
    case VariantType::array:
        return is_one_of(source,
                         {VariantType::packed_byte_array, VariantType::packed_int32_array,
                          VariantType::packed_int64_array, VariantType::packed_float32_array,
                          VariantType::packed_float64_array, VariantType::packed_string_array,
                          VariantType::packed_vector2_array, VariantType::packed_vector3_array,
                          VariantType::packed_color_array, VariantType::packed_vector4_array});
    case VariantType::packed_byte_array:
    case VariantType::packed_int32_array:
    case VariantType::packed_int64_array:
    case VariantType::packed_float32_array:
    case VariantType::packed_float64_array:
    case VariantType::packed_string_array:
    case VariantType::packed_vector2_array:
    case VariantType::packed_vector3_array:
    case VariantType::packed_color_array:
    case VariantType::packed_vector4_array:
        return source == VariantType::array;
    case VariantType::nil:
    case VariantType::plane:
    case VariantType::aabb:
    case VariantType::callable:
    case VariantType::signal:
    case VariantType::dictionary:
        return false;
    }
    return false;
}

bool explicit_variant_conversion(const VariantType target, const VariantType source) noexcept {
    if (strict_variant_conversion(target, source))
        return true;
    if (source == VariantType::nil)
        return false;

    switch (target) {
    case VariantType::boolean:
    case VariantType::integer:
    case VariantType::floating:
        return source == VariantType::string;
    case VariantType::string:
        return source != VariantType::object;
    case VariantType::nil:
    case VariantType::vector2:
    case VariantType::vector2i:
    case VariantType::rect2:
    case VariantType::rect2i:
    case VariantType::vector3:
    case VariantType::vector3i:
    case VariantType::transform2d:
    case VariantType::vector4:
    case VariantType::vector4i:
    case VariantType::plane:
    case VariantType::quaternion:
    case VariantType::aabb:
    case VariantType::basis:
    case VariantType::transform3d:
    case VariantType::projection:
    case VariantType::color:
    case VariantType::string_name:
    case VariantType::node_path:
    case VariantType::rid:
    case VariantType::object:
    case VariantType::callable:
    case VariantType::signal:
    case VariantType::dictionary:
    case VariantType::array:
    case VariantType::packed_byte_array:
    case VariantType::packed_int32_array:
    case VariantType::packed_int64_array:
    case VariantType::packed_float32_array:
    case VariantType::packed_float64_array:
    case VariantType::packed_string_array:
    case VariantType::packed_vector2_array:
    case VariantType::packed_vector3_array:
    case VariantType::packed_color_array:
    case VariantType::packed_vector4_array:
        return false;
    }
    return false;
}

bool compatible_container_shape(const Type& target, const Type& source) noexcept {
    if (target.kind != source.kind)
        return true;
    if (target.kind != TypeKind::array && target.kind != TypeKind::dictionary)
        return true;
    const bool target_typed = is_explicitly_typed_container(target);
    const bool source_typed = is_explicitly_typed_container(source);
    return !target_typed || !source_typed || target.name == source.name;
}

} // namespace

ConversionKind classify_conversion(const Type& target, const Type& source) noexcept {
    if (!target.is_value() || !source.is_value())
        return ConversionKind::incompatible;
    if (target == source)
        return ConversionKind::identity;
    if (target.kind == TypeKind::unknown || target.kind == TypeKind::variant)
        return ConversionKind::implicit;
    if (source.kind == TypeKind::unknown || source.kind == TypeKind::variant)
        return ConversionKind::dynamic;

    if (target.kind == TypeKind::enumeration) {
        if (source.kind == TypeKind::integer)
            return ConversionKind::implicit;
        if (source.kind == TypeKind::enumeration)
            return target.name == source.name ? ConversionKind::implicit
                                              : ConversionKind::explicit_only;
        return ConversionKind::incompatible;
    }
    if (target.kind == TypeKind::integer && source.kind == TypeKind::enumeration)
        return ConversionKind::implicit;
    if (target.kind == TypeKind::script_resource && source.kind == TypeKind::nil)
        return ConversionKind::implicit;
    // A resolved `.gd` resource is a source-level GDScript value. Binary-only exports replace its
    // concrete provider with AttachedCompiledScript, but that representational change must not
    // make valid GDScript annotations fail semantic analysis.
    if (target.kind == TypeKind::object && target.name == "GDScript" &&
        source.kind == TypeKind::script_resource) {
        return ConversionKind::implicit;
    }
    if (target.kind == TypeKind::script_resource || source.kind == TypeKind::script_resource)
        return ConversionKind::incompatible;

    const auto target_variant = variant_type_of(target);
    const auto source_variant = variant_type_of(source);
    if (!target_variant || !source_variant) {
        return ConversionKind::incompatible;
    }
    if (strict_variant_conversion(*target_variant, *source_variant)) {
        // GDScript's `as` validates only the builtin Variant kind. Typed containers therefore
        // remain explicitly castable across element signatures even though strict assignment is
        // invariant and the VM may reject the resulting value when it enters typed storage.
        return compatible_container_shape(target, source) ? ConversionKind::implicit
                                                          : ConversionKind::explicit_only;
    }
    if (explicit_variant_conversion(*target_variant, *source_variant))
        return ConversionKind::explicit_only;
    return ConversionKind::incompatible;
}

bool is_implicitly_convertible(const Type& target, const Type& source) noexcept {
    const auto conversion = classify_conversion(target, source);
    return conversion == ConversionKind::identity || conversion == ConversionKind::implicit ||
           conversion == ConversionKind::dynamic;
}

bool is_explicitly_convertible(const Type& target, const Type& source) noexcept {
    return classify_conversion(target, source) != ConversionKind::incompatible;
}

bool is_typed_storage_compatible(const Type& target, const Type& source) noexcept {
    if (!is_explicitly_typed_container(target) || source.is_dynamic())
        return true;
    return target.kind == source.kind && target.name == source.name;
}

bool is_runtime_storage_compatible(const Type& target, const Type& source) noexcept {
    if (!is_typed_storage_compatible(target, source))
        return false;
    // RID <- Object is a native binding conversion. A typed GDScript local instead retains the
    // Object Variant unchanged, which a native RID slot cannot represent without changing it.
    return !(target.kind == TypeKind::builtin && target.name == "RID" &&
             source.kind == TypeKind::object);
}

bool is_explicit_runtime_constructible(const Type& target, const Type& source) noexcept {
    if (target.is_dynamic() || source.is_dynamic())
        return true;
    const auto conversion = classify_conversion(target, source);
    if (conversion == ConversionKind::incompatible)
        return false;
    // Variant::can_convert advertises general stringification, which GDScript's `as` analyzer
    // accepts. The VM then invokes String's one-argument constructors, whose only source types are
    // String, StringName and NodePath. Every additional advertised source fails deterministically.
    return target.kind != TypeKind::string || conversion != ConversionKind::explicit_only;
}

} // namespace gdpp
