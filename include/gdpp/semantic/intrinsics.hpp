#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace gdpp {

// Language intrinsics are not Godot API utility functions: their typing/lowering remains stable
// across target engine API tables and may require compiler-specific treatment.
enum class IntrinsicKind : std::uint8_t {
    none,
    load,
    preload,
    range,
    length,
    get_stack,
    convert,
    dictionary_to_instance,
    type_exists,
    character,
    ordinal,
    color8,
    instance_to_dictionary,
    is_instance_of,
    print_debug,
    print_stack,
};

enum class IntrinsicArgumentRule : std::uint8_t {
    any,
    integer,
    string,
    string_name,
    type_descriptor,
    resource_path,
    dictionary,
    object,
};
enum class IntrinsicResultRule : std::uint8_t {
    void_type,
    dynamic,
    boolean,
    integer,
    string,
    color,
    array,
    dictionary,
    integer_array,
    object,
    resource,
};

struct IntrinsicFeature {
    IntrinsicKind kind{IntrinsicKind::none};
    std::string_view name;
    std::uint8_t minimum_arguments{0};
    std::uint8_t maximum_arguments{0};
    std::array<IntrinsicArgumentRule, 4> argument_rules{};
    IntrinsicResultRule result_rule{IntrinsicResultRule::dynamic};
    std::string_view runtime_symbol;
    bool is_constant{false};
};

[[nodiscard]] bool intrinsic_is_vararg(IntrinsicKind kind) noexcept;

class IntrinsicRegistry final {
  public:
    [[nodiscard]] static const IntrinsicRegistry& latest() noexcept;
    [[nodiscard]] const IntrinsicFeature* find(std::string_view name) const noexcept;
    [[nodiscard]] const IntrinsicFeature* find(IntrinsicKind kind) const noexcept;

  private:
    IntrinsicRegistry() = default;
};

} // namespace gdpp
