#pragma once

#include "gdpp/core/godot_version.hpp"
#include "gdpp/semantic/type.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gdpp {

struct GodotClassRecord {
    const char* name;
    const char* inherits;
    bool builtin;
    bool editor_only;
};

struct GodotClassConstantRecord {
    const char* owner;
    const char* enum_name;
    const char* name;
    std::int64_t value;
    bool is_bitfield;
};

struct GodotSingletonRecord {
    const char* name;
    const char* type;
};

struct GodotMethodRecord {
    const char* owner;
    const char* name;
    const char* return_type;
    const char* return_meta;
    std::uint16_t required_arguments;
    std::uint16_t maximum_arguments;
    std::uint32_t first_argument;
    bool is_static;
    bool is_vararg;
    bool is_const;
    bool is_virtual;
};

struct GodotArgumentRecord {
    const char* name;
    const char* type;
    const char* meta;
    bool has_default;
};

struct GodotConstructorRecord {
    const char* owner;
    std::uint32_t first_argument;
    std::uint16_t argument_count;
};

struct GodotPropertyRecord {
    const char* owner;
    const char* name;
    const char* type;
    const char* getter;
    const char* setter;
    bool direct;
    std::int64_t index;
};

struct GodotSignalRecord {
    const char* owner;
    const char* name;
    std::uint32_t first_argument;
    std::uint16_t argument_count;
};

struct GodotUtilityFunctionRecord {
    const char* name;
    const char* return_type;
    std::uint16_t required_arguments;
    std::uint16_t maximum_arguments;
    std::uint32_t first_argument;
    bool is_vararg;
    bool is_constant;
};

struct GodotGlobalConstantRecord {
    const char* name;
    std::int64_t value;
};

struct GodotGlobalEnumValueRecord {
    const char* owner;
    const char* name;
    std::int64_t value;
    bool is_bitfield;
};

struct GodotBuiltinOperatorRecord {
    const char* left_type;
    const char* name;
    const char* right_type;
    const char* return_type;
};

struct GodotBuiltinConstantRecord {
    const char* owner;
    const char* name;
    const char* type;
    const char* value;
};

// extension_api.json carries a second, native representation axis for scalar values and
// non-null object references. Keep that vocabulary explicit and fail-closed so a new Godot meta
// value cannot silently fall back to GDScript's wider int/float ABI.
enum class GodotNativeMeta {
    none,
    real,
    float32,
    float64,
    int8,
    int16,
    int32,
    int64,
    uint8,
    uint16,
    uint32,
    uint64,
    char16,
    char32,
    required_object,
    unknown,
};

class GodotApi final {
  public:
    [[nodiscard]] static const GodotApi& instance() noexcept;
    [[nodiscard]] static const GodotApi& for_version(GodotVersion version) noexcept;

    [[nodiscard]] std::string_view version() const noexcept;
    [[nodiscard]] std::size_t class_count() const noexcept;
    [[nodiscard]] std::size_t class_constant_count() const noexcept;
    [[nodiscard]] std::size_t method_count() const noexcept;
    [[nodiscard]] std::size_t property_count() const noexcept;
    [[nodiscard]] std::size_t signal_count() const noexcept;
    [[nodiscard]] std::size_t argument_count() const noexcept;
    [[nodiscard]] std::size_t constructor_count() const noexcept;
    [[nodiscard]] std::size_t singleton_count() const noexcept;
    [[nodiscard]] std::size_t utility_function_count() const noexcept;
    [[nodiscard]] std::size_t global_constant_count() const noexcept;
    [[nodiscard]] std::size_t global_enum_value_count() const noexcept;
    [[nodiscard]] std::size_t builtin_operator_count() const noexcept;
    [[nodiscard]] std::size_t builtin_constant_count() const noexcept;
    [[nodiscard]] bool validate_metadata() const noexcept;
    [[nodiscard]] const GodotClassRecord* find_class(std::string_view name) const noexcept;
    [[nodiscard]] bool is_editor_class(std::string_view name) const noexcept;
    [[nodiscard]] const GodotClassConstantRecord*
    find_class_constant(std::string_view owner, std::string_view name,
                        bool include_inherited = true) const noexcept;
    [[nodiscard]] const GodotClassConstantRecord*
    find_class_enum_value(std::string_view owner, std::string_view enum_name, std::string_view name,
                          bool include_inherited = true) const noexcept;
    [[nodiscard]] const GodotClassRecord*
    find_class_enum_owner(std::string_view owner, std::string_view enum_name,
                          bool include_inherited = true) const noexcept;
    [[nodiscard]] bool has_class_enum(std::string_view owner, std::string_view enum_name,
                                      bool include_inherited = true) const noexcept;
    [[nodiscard]] const GodotSingletonRecord* find_singleton(std::string_view name) const noexcept;
    [[nodiscard]] const GodotMethodRecord*
    find_method(std::string_view owner, std::string_view name,
                bool include_inherited = true) const noexcept;
    [[nodiscard]] const GodotPropertyRecord*
    find_property(std::string_view owner, std::string_view name,
                  bool include_inherited = true) const noexcept;
    [[nodiscard]] const GodotPropertyRecord* property(std::size_t index) const noexcept;
    // Property metadata may list several inspector-compatible concrete resources
    // (for example CanvasItemMaterial,ShaderMaterial) even though the generated
    // getter/setter ABI uses their common Material base. Reads and writes are
    // resolved independently so code generation never assumes that both accessors
    // have the same contract merely because current engine metadata usually does.
    [[nodiscard]] Type property_getter_type(const GodotPropertyRecord& property) const noexcept;
    [[nodiscard]] Type property_setter_type(const GodotPropertyRecord& property) const noexcept;
    [[nodiscard]] const GodotSignalRecord*
    find_signal(std::string_view owner, std::string_view name,
                bool include_inherited = true) const noexcept;
    [[nodiscard]] const GodotArgumentRecord* argument(const GodotSignalRecord& signal,
                                                      std::size_t index) const noexcept;
    [[nodiscard]] bool inherits(std::string_view type, std::string_view base) const noexcept;
    [[nodiscard]] const GodotArgumentRecord* argument(const GodotMethodRecord& method,
                                                      std::size_t index) const noexcept;
    [[nodiscard]] const GodotConstructorRecord*
    find_constructor(std::string_view owner, std::size_t argument_count,
                     std::size_t occurrence = 0) const noexcept;
    [[nodiscard]] const GodotArgumentRecord* argument(const GodotConstructorRecord& constructor,
                                                      std::size_t index) const noexcept;
    [[nodiscard]] const GodotUtilityFunctionRecord*
    find_utility_function(std::string_view name) const noexcept;
    [[nodiscard]] const GodotArgumentRecord* argument(const GodotUtilityFunctionRecord& function,
                                                      std::size_t index) const noexcept;
    [[nodiscard]] const GodotGlobalConstantRecord*
    find_global_constant(std::string_view name) const noexcept;
    [[nodiscard]] const GodotGlobalEnumValueRecord*
    find_global_enum_value(std::string_view owner, std::string_view name) const noexcept;
    [[nodiscard]] const GodotGlobalEnumValueRecord*
    find_global_enum_value(std::string_view name) const noexcept;
    [[nodiscard]] bool has_global_enum(std::string_view owner) const noexcept;
    [[nodiscard]] const GodotBuiltinOperatorRecord*
    find_builtin_operator(std::string_view left_type, std::string_view name,
                          std::string_view right_type = {}) const noexcept;
    [[nodiscard]] const GodotBuiltinConstantRecord*
    find_builtin_constant(std::string_view owner, std::string_view name) const noexcept;

  private:
    GodotApi(std::string_view version, const GodotClassRecord* classes, std::size_t class_count,
             const GodotClassConstantRecord* class_constants, std::size_t class_constant_count,
             const GodotSingletonRecord* singletons, std::size_t singleton_count,
             const GodotMethodRecord* methods, std::size_t method_count,
             const GodotConstructorRecord* constructors, std::size_t constructor_count,
             const GodotArgumentRecord* arguments, std::size_t argument_count,
             const GodotPropertyRecord* properties, std::size_t property_count,
             const GodotSignalRecord* signals, std::size_t signal_count,
             const GodotUtilityFunctionRecord* utility_functions,
             std::size_t utility_function_count, const GodotGlobalConstantRecord* global_constants,
             std::size_t global_constant_count,
             const GodotGlobalEnumValueRecord* global_enum_values,
             std::size_t global_enum_value_count,
             const GodotBuiltinOperatorRecord* builtin_operators,
             std::size_t builtin_operator_count,
             const GodotBuiltinConstantRecord* builtin_constants,
             std::size_t builtin_constant_count) noexcept;

    std::string_view version_;
    const GodotClassRecord* classes_;
    std::size_t class_count_;
    const GodotClassConstantRecord* class_constants_;
    std::size_t class_constant_count_;
    const GodotSingletonRecord* singletons_;
    std::size_t singleton_count_;
    const GodotMethodRecord* methods_;
    std::size_t method_count_;
    const GodotConstructorRecord* constructors_;
    std::size_t constructor_count_;
    const GodotArgumentRecord* arguments_;
    std::size_t argument_count_;
    const GodotPropertyRecord* properties_;
    std::size_t property_count_;
    const GodotSignalRecord* signals_;
    std::size_t signal_count_;
    const GodotUtilityFunctionRecord* utility_functions_;
    std::size_t utility_function_count_;
    const GodotGlobalConstantRecord* global_constants_;
    std::size_t global_constant_count_;
    const GodotGlobalEnumValueRecord* global_enum_values_;
    std::size_t global_enum_value_count_;
    const GodotBuiltinOperatorRecord* builtin_operators_;
    std::size_t builtin_operator_count_;
    const GodotBuiltinConstantRecord* builtin_constants_;
    std::size_t builtin_constant_count_;
};

[[nodiscard]] Type type_from_godot_api(std::string_view type);
[[nodiscard]] GodotNativeMeta godot_native_meta(std::string_view meta) noexcept;
[[nodiscard]] std::string_view godot_native_scalar_cpp_type(GodotNativeMeta meta) noexcept;
[[nodiscard]] bool godot_api_meta_matches(std::string_view type, std::string_view meta) noexcept;
[[nodiscard]] bool godot_api_type_is_native_pointer(std::string_view type) noexcept;

} // namespace gdpp
