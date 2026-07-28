#include "gdpp/semantic/iteration.hpp"

namespace gdpp {

IterationPlan make_iteration_plan(const Type& iterable, const Type& element_type,
                                  const bool intrinsic_range) {
    if (intrinsic_range)
        return {IterationStrategy::intrinsic_range, {TypeKind::integer, "int"}};
    if (iterable.is_dynamic())
        return {IterationStrategy::dynamic_protocol, element_type};
    if (iterable.kind == TypeKind::integer || iterable.kind == TypeKind::enumeration)
        return {IterationStrategy::integer_count, {TypeKind::integer, "int"}};
    if (iterable.kind == TypeKind::floating)
        return {IterationStrategy::floating_count, {TypeKind::floating, "float"}};
    if (iterable.kind == TypeKind::builtin && iterable.name == "Vector2")
        return {IterationStrategy::vector2_range, {TypeKind::floating, "float"}};
    if (iterable.kind == TypeKind::builtin && iterable.name == "Vector2i")
        return {IterationStrategy::vector2i_range, {TypeKind::integer, "int"}};
    if (iterable.kind == TypeKind::builtin && iterable.name == "Vector3")
        return {IterationStrategy::vector3_range, {TypeKind::floating, "float"}};
    if (iterable.kind == TypeKind::builtin && iterable.name == "Vector3i")
        return {IterationStrategy::vector3i_range, {TypeKind::integer, "int"}};
    if (iterable.kind == TypeKind::string)
        return {IterationStrategy::indexed_string, {TypeKind::string, "String"}};
    if (iterable.kind == TypeKind::array)
        return {IterationStrategy::indexed_array, element_type};
    if (iterable.is_packed_array())
        return {IterationStrategy::indexed_packed_array, element_type};
    if (iterable.kind == TypeKind::dictionary)
        return {IterationStrategy::dictionary_protocol, element_type};
    if (iterable.kind == TypeKind::object)
        return {IterationStrategy::object_protocol, element_type};
    return {};
}

std::string_view iteration_strategy_name(const IterationStrategy strategy) noexcept {
    switch (strategy) {
    case IterationStrategy::none:
        return "none";
    case IterationStrategy::dynamic_protocol:
        return "dynamic_protocol";
    case IterationStrategy::integer_count:
        return "integer_count";
    case IterationStrategy::floating_count:
        return "floating_count";
    case IterationStrategy::intrinsic_range:
        return "intrinsic_range";
    case IterationStrategy::vector2_range:
        return "vector2_range";
    case IterationStrategy::vector2i_range:
        return "vector2i_range";
    case IterationStrategy::vector3_range:
        return "vector3_range";
    case IterationStrategy::vector3i_range:
        return "vector3i_range";
    case IterationStrategy::indexed_string:
        return "indexed_string";
    case IterationStrategy::indexed_array:
        return "indexed_array";
    case IterationStrategy::indexed_packed_array:
        return "indexed_packed_array";
    case IterationStrategy::dictionary_protocol:
        return "dictionary_protocol";
    case IterationStrategy::object_protocol:
        return "object_protocol";
    }
    return "unknown";
}

} // namespace gdpp
