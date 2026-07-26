#include "gdpp/semantic/godot_api.hpp"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>

#include "gdpp/godot_api_data_4_4.inc"
#include "gdpp/godot_api_data_4_5.inc"
#include "gdpp/godot_api_data_4_6.inc"
#include "gdpp/godot_api_data_4_7.inc"

namespace gdpp {
namespace {

template <typename Record>
const Record* find_owned_record(const Record* records, std::size_t size, std::string_view owner,
                                std::string_view name) noexcept {
    const auto key = std::pair{owner, name};
    const auto found =
        std::lower_bound(records, records + size, key, [](const Record& record, const auto& value) {
            return std::pair{std::string_view{record.owner}, std::string_view{record.name}} < value;
        });
    if (found == records + size || std::string_view{found->owner} != owner ||
        std::string_view{found->name} != name)
        return nullptr;
    return found;
}

std::string_view first_allowed_type(std::string_view type) {
    const auto comma = type.find(',');
    if (comma != std::string_view::npos)
        type = type.substr(0, comma);
    while (!type.empty() && type.front() == '-')
        type.remove_prefix(1);
    return type;
}

std::string collection_element_type(std::string_view type) {
    const auto colon = type.find(':');
    if (colon == std::string_view::npos)
        return std::string{type};
    const auto slash = type.find('/');
    const auto type_end = slash == std::string_view::npos || slash > colon ? colon : slash;
    std::uint32_t raw_type = 0;
    const auto parsed = std::from_chars(type.data(), type.data() + type_end, raw_type);
    if (parsed.ec != std::errc{} || parsed.ptr != type.data() + type_end ||
        raw_type > static_cast<std::uint32_t>(VariantType::packed_vector4_array)) {
        return std::string{type};
    }
    const auto detail = type.substr(colon + 1);
    if (raw_type == static_cast<std::uint32_t>(VariantType::nil))
        return "Variant";
    if (raw_type == static_cast<std::uint32_t>(VariantType::object) && !detail.empty())
        return std::string{detail};
    return std::string{variant_type_name(static_cast<VariantType>(raw_type))};
}

} // namespace

GodotApi::GodotApi(
    std::string_view version, const GodotClassRecord* classes, std::size_t class_count,
    const GodotClassConstantRecord* class_constants, std::size_t class_constant_count,
    const GodotSingletonRecord* singletons, std::size_t singleton_count,
    const GodotMethodRecord* methods, std::size_t method_count,
    const GodotConstructorRecord* constructors, std::size_t constructor_count,
    const GodotArgumentRecord* arguments, std::size_t argument_count,
    const GodotPropertyRecord* properties, std::size_t property_count,
    const GodotSignalRecord* signals, std::size_t signal_count,
    const GodotUtilityFunctionRecord* utility_functions, std::size_t utility_function_count,
    const GodotGlobalConstantRecord* global_constants, std::size_t global_constant_count,
    const GodotGlobalEnumValueRecord* global_enum_values, std::size_t global_enum_value_count,
    const GodotBuiltinOperatorRecord* builtin_operators, std::size_t builtin_operator_count,
    const GodotBuiltinConstantRecord* builtin_constants,
    std::size_t builtin_constant_count) noexcept
    : version_(version), classes_(classes), class_count_(class_count),
      class_constants_(class_constants), class_constant_count_(class_constant_count),
      singletons_(singletons), singleton_count_(singleton_count), methods_(methods),
      method_count_(method_count), constructors_(constructors),
      constructor_count_(constructor_count), arguments_(arguments), argument_count_(argument_count),
      properties_(properties), property_count_(property_count), signals_(signals),
      signal_count_(signal_count), utility_functions_(utility_functions),
      utility_function_count_(utility_function_count), global_constants_(global_constants),
      global_constant_count_(global_constant_count), global_enum_values_(global_enum_values),
      global_enum_value_count_(global_enum_value_count), builtin_operators_(builtin_operators),
      builtin_operator_count_(builtin_operator_count), builtin_constants_(builtin_constants),
      builtin_constant_count_(builtin_constant_count) {}

const GodotApi& GodotApi::instance() noexcept { return for_version(minimum_godot_version); }

const GodotApi& GodotApi::for_version(GodotVersion version) noexcept {
    static const GodotApi api_4_4{generated_api_4_4::version,
                                  generated_api_4_4::classes,
                                  std::size(generated_api_4_4::classes),
                                  generated_api_4_4::class_constants,
                                  generated_api_4_4::class_constant_count,
                                  generated_api_4_4::singletons,
                                  std::size(generated_api_4_4::singletons),
                                  generated_api_4_4::methods,
                                  std::size(generated_api_4_4::methods),
                                  generated_api_4_4::constructors,
                                  std::size(generated_api_4_4::constructors),
                                  generated_api_4_4::arguments,
                                  std::size(generated_api_4_4::arguments),
                                  generated_api_4_4::properties,
                                  std::size(generated_api_4_4::properties),
                                  generated_api_4_4::signals,
                                  std::size(generated_api_4_4::signals),
                                  generated_api_4_4::utility_functions,
                                  generated_api_4_4::utility_function_count,
                                  generated_api_4_4::global_constants,
                                  generated_api_4_4::global_constant_count,
                                  generated_api_4_4::global_enum_values,
                                  generated_api_4_4::global_enum_value_count,
                                  generated_api_4_4::builtin_operators,
                                  generated_api_4_4::builtin_operator_count,
                                  generated_api_4_4::builtin_constants,
                                  generated_api_4_4::builtin_constant_count};
    static const GodotApi api_4_5{generated_api_4_5::version,
                                  generated_api_4_5::classes,
                                  std::size(generated_api_4_5::classes),
                                  generated_api_4_5::class_constants,
                                  generated_api_4_5::class_constant_count,
                                  generated_api_4_5::singletons,
                                  std::size(generated_api_4_5::singletons),
                                  generated_api_4_5::methods,
                                  std::size(generated_api_4_5::methods),
                                  generated_api_4_5::constructors,
                                  std::size(generated_api_4_5::constructors),
                                  generated_api_4_5::arguments,
                                  std::size(generated_api_4_5::arguments),
                                  generated_api_4_5::properties,
                                  std::size(generated_api_4_5::properties),
                                  generated_api_4_5::signals,
                                  std::size(generated_api_4_5::signals),
                                  generated_api_4_5::utility_functions,
                                  generated_api_4_5::utility_function_count,
                                  generated_api_4_5::global_constants,
                                  generated_api_4_5::global_constant_count,
                                  generated_api_4_5::global_enum_values,
                                  generated_api_4_5::global_enum_value_count,
                                  generated_api_4_5::builtin_operators,
                                  generated_api_4_5::builtin_operator_count,
                                  generated_api_4_5::builtin_constants,
                                  generated_api_4_5::builtin_constant_count};
    static const GodotApi api_4_6{generated_api_4_6::version,
                                  generated_api_4_6::classes,
                                  std::size(generated_api_4_6::classes),
                                  generated_api_4_6::class_constants,
                                  generated_api_4_6::class_constant_count,
                                  generated_api_4_6::singletons,
                                  std::size(generated_api_4_6::singletons),
                                  generated_api_4_6::methods,
                                  std::size(generated_api_4_6::methods),
                                  generated_api_4_6::constructors,
                                  std::size(generated_api_4_6::constructors),
                                  generated_api_4_6::arguments,
                                  std::size(generated_api_4_6::arguments),
                                  generated_api_4_6::properties,
                                  std::size(generated_api_4_6::properties),
                                  generated_api_4_6::signals,
                                  std::size(generated_api_4_6::signals),
                                  generated_api_4_6::utility_functions,
                                  generated_api_4_6::utility_function_count,
                                  generated_api_4_6::global_constants,
                                  generated_api_4_6::global_constant_count,
                                  generated_api_4_6::global_enum_values,
                                  generated_api_4_6::global_enum_value_count,
                                  generated_api_4_6::builtin_operators,
                                  generated_api_4_6::builtin_operator_count,
                                  generated_api_4_6::builtin_constants,
                                  generated_api_4_6::builtin_constant_count};
    static const GodotApi api_4_7{generated_api_4_7::version,
                                  generated_api_4_7::classes,
                                  std::size(generated_api_4_7::classes),
                                  generated_api_4_7::class_constants,
                                  generated_api_4_7::class_constant_count,
                                  generated_api_4_7::singletons,
                                  std::size(generated_api_4_7::singletons),
                                  generated_api_4_7::methods,
                                  std::size(generated_api_4_7::methods),
                                  generated_api_4_7::constructors,
                                  std::size(generated_api_4_7::constructors),
                                  generated_api_4_7::arguments,
                                  std::size(generated_api_4_7::arguments),
                                  generated_api_4_7::properties,
                                  std::size(generated_api_4_7::properties),
                                  generated_api_4_7::signals,
                                  std::size(generated_api_4_7::signals),
                                  generated_api_4_7::utility_functions,
                                  generated_api_4_7::utility_function_count,
                                  generated_api_4_7::global_constants,
                                  generated_api_4_7::global_constant_count,
                                  generated_api_4_7::global_enum_values,
                                  generated_api_4_7::global_enum_value_count,
                                  generated_api_4_7::builtin_operators,
                                  generated_api_4_7::builtin_operator_count,
                                  generated_api_4_7::builtin_constants,
                                  generated_api_4_7::builtin_constant_count};
    switch (version) {
    case GodotVersion::v4_4:
        return api_4_4;
    case GodotVersion::v4_5:
        return api_4_5;
    case GodotVersion::v4_6:
        return api_4_6;
    case GodotVersion::v4_7:
        return api_4_7;
    }
    return api_4_4;
}

std::string_view GodotApi::version() const noexcept { return version_; }
std::size_t GodotApi::class_count() const noexcept { return class_count_; }
std::size_t GodotApi::class_constant_count() const noexcept { return class_constant_count_; }
std::size_t GodotApi::method_count() const noexcept { return method_count_; }
std::size_t GodotApi::property_count() const noexcept { return property_count_; }
std::size_t GodotApi::signal_count() const noexcept { return signal_count_; }
std::size_t GodotApi::argument_count() const noexcept { return argument_count_; }
std::size_t GodotApi::constructor_count() const noexcept { return constructor_count_; }
std::size_t GodotApi::singleton_count() const noexcept { return singleton_count_; }
std::size_t GodotApi::utility_function_count() const noexcept { return utility_function_count_; }
std::size_t GodotApi::global_constant_count() const noexcept { return global_constant_count_; }
std::size_t GodotApi::global_enum_value_count() const noexcept { return global_enum_value_count_; }
std::size_t GodotApi::builtin_operator_count() const noexcept { return builtin_operator_count_; }
std::size_t GodotApi::builtin_constant_count() const noexcept { return builtin_constant_count_; }

bool GodotApi::validate_metadata() const noexcept {
    const auto arguments_valid = [&](const std::size_t first, const std::size_t count) {
        return first <= argument_count_ && count <= argument_count_ - first;
    };
    if (!std::is_sorted(classes_, classes_ + class_count_, [](const auto& left, const auto& right) {
            return std::string_view{left.name} < std::string_view{right.name};
        })) {
        return false;
    }
    for (std::size_t index = 0; index < class_count_; ++index) {
        const auto& type = classes_[index];
        if (type.name[0] == '\0' ||
            (index != 0 &&
             std::string_view{classes_[index - 1].name} == std::string_view{type.name}) ||
            (type.inherits[0] != '\0' && !find_class(type.inherits))) {
            return false;
        }
    }
    for (std::size_t index = 0; index < method_count_; ++index) {
        const auto& method = methods_[index];
        if (!find_class(method.owner) || method.name[0] == '\0' ||
            method.required_arguments > method.maximum_arguments ||
            !arguments_valid(method.first_argument, method.maximum_arguments)) {
            return false;
        }
        for (std::size_t argument_index = 0; argument_index < method.maximum_arguments;
             ++argument_index) {
            const auto* value = argument(method, argument_index);
            if (!value || value->type[0] == '\0' ||
                (argument_index < method.required_arguments && value->has_default) ||
                (argument_index >= method.required_arguments && !value->has_default)) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < constructor_count_; ++index) {
        const auto& constructor = constructors_[index];
        const auto* owner = find_class(constructor.owner);
        if (!owner || !owner->builtin ||
            !arguments_valid(constructor.first_argument, constructor.argument_count)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < signal_count_; ++index) {
        const auto& signal = signals_[index];
        if (!find_class(signal.owner) || signal.name[0] == '\0' ||
            !arguments_valid(signal.first_argument, signal.argument_count)) {
            return false;
        }
        for (std::size_t argument_index = 0; argument_index < signal.argument_count;
             ++argument_index) {
            const auto* value = argument(signal, argument_index);
            if (!value || value->type[0] == '\0' || value->has_default)
                return false;
        }
    }
    for (std::size_t index = 0; index < utility_function_count_; ++index) {
        const auto& function = utility_functions_[index];
        if (function.name[0] == '\0' ||
            function.required_arguments > function.maximum_arguments ||
            !arguments_valid(function.first_argument, function.maximum_arguments)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < property_count_; ++index) {
        if (!find_class(properties_[index].owner) || properties_[index].name[0] == '\0' ||
            properties_[index].type[0] == '\0') {
            return false;
        }
    }
    for (std::size_t index = 0; index < singleton_count_; ++index) {
        if (singletons_[index].name[0] == '\0' || !find_class(singletons_[index].type))
            return false;
    }
    return true;
}

const GodotClassRecord* GodotApi::find_class(std::string_view name) const noexcept {
    const auto found = std::lower_bound(classes_, classes_ + class_count_, name,
                                        [](const GodotClassRecord& record, std::string_view value) {
                                            return std::string_view{record.name} < value;
                                        });
    return found == classes_ + class_count_ || std::string_view{found->name} != name ? nullptr
                                                                                     : found;
}

bool GodotApi::is_editor_class(std::string_view name) const noexcept {
    for (std::size_t depth = 0; !name.empty() && depth < class_count_; ++depth) {
        const auto* type = find_class(name);
        if (!type)
            return false;
        if (type->editor_only)
            return true;
        name = type->inherits;
    }
    return false;
}

const GodotClassConstantRecord*
GodotApi::find_class_constant(std::string_view owner, std::string_view name,
                              bool include_inherited) const noexcept {
    for (std::size_t depth = 0; !owner.empty() && depth < class_count(); ++depth) {
        const auto first =
            std::lower_bound(class_constants_, class_constants_ + class_constant_count_, owner,
                             [](const GodotClassConstantRecord& record, std::string_view value) {
                                 return std::string_view{record.owner} < value;
                             });
        for (auto current = first; current != class_constants_ + class_constant_count_ &&
                                   std::string_view{current->owner} == owner;
             ++current) {
            if (std::string_view{current->name} == name)
                return current;
        }
        if (!include_inherited)
            break;
        const auto* type = find_class(owner);
        if (!type || type->inherits[0] == '\0')
            break;
        owner = type->inherits;
    }
    return nullptr;
}

const GodotClassConstantRecord*
GodotApi::find_class_enum_value(std::string_view owner, std::string_view enum_name,
                                std::string_view name, bool include_inherited) const noexcept {
    for (std::size_t depth = 0; !owner.empty() && depth < class_count(); ++depth) {
        const auto first =
            std::lower_bound(class_constants_, class_constants_ + class_constant_count_, owner,
                             [](const GodotClassConstantRecord& record, std::string_view value) {
                                 return std::string_view{record.owner} < value;
                             });
        for (auto current = first; current != class_constants_ + class_constant_count_ &&
                                   std::string_view{current->owner} == owner;
             ++current) {
            if (std::string_view{current->enum_name} == enum_name &&
                std::string_view{current->name} == name) {
                return current;
            }
        }
        if (!include_inherited)
            break;
        const auto* type = find_class(owner);
        if (!type || type->inherits[0] == '\0')
            break;
        owner = type->inherits;
    }
    return nullptr;
}

bool GodotApi::has_class_enum(std::string_view owner, std::string_view enum_name,
                              bool include_inherited) const noexcept {
    for (std::size_t depth = 0; !owner.empty() && depth < class_count(); ++depth) {
        const auto first =
            std::lower_bound(class_constants_, class_constants_ + class_constant_count_, owner,
                             [](const GodotClassConstantRecord& record, std::string_view value) {
                                 return std::string_view{record.owner} < value;
                             });
        for (auto current = first; current != class_constants_ + class_constant_count_ &&
                                   std::string_view{current->owner} == owner;
             ++current) {
            if (std::string_view{current->enum_name} == enum_name)
                return true;
        }
        if (!include_inherited)
            break;
        const auto* type = find_class(owner);
        if (!type || type->inherits[0] == '\0')
            break;
        owner = type->inherits;
    }
    return false;
}

const GodotSingletonRecord* GodotApi::find_singleton(std::string_view name) const noexcept {
    const auto found =
        std::lower_bound(singletons_, singletons_ + singleton_count_, name,
                         [](const GodotSingletonRecord& record, std::string_view value) {
                             return std::string_view{record.name} < value;
                         });
    return found == singletons_ + singleton_count_ || std::string_view{found->name} != name
               ? nullptr
               : found;
}

const GodotMethodRecord* GodotApi::find_method(std::string_view owner, std::string_view name,
                                               bool include_inherited) const noexcept {
    for (std::size_t depth = 0; !owner.empty() && depth < class_count(); ++depth) {
        if (const auto* method = find_owned_record(methods_, method_count_, owner, name))
            return method;
        if (!include_inherited)
            break;
        const auto* type = find_class(owner);
        if (!type || type->inherits[0] == '\0')
            break;
        owner = type->inherits;
    }
    return nullptr;
}

const GodotPropertyRecord* GodotApi::find_property(std::string_view owner, std::string_view name,
                                                   bool include_inherited) const noexcept {
    for (std::size_t depth = 0; !owner.empty() && depth < class_count(); ++depth) {
        if (const auto* property = find_owned_record(properties_, property_count_, owner, name))
            return property;
        if (!include_inherited)
            break;
        const auto* type = find_class(owner);
        if (!type || type->inherits[0] == '\0')
            break;
        owner = type->inherits;
    }
    return nullptr;
}

const GodotPropertyRecord* GodotApi::property(const std::size_t index) const noexcept {
    return index < property_count_ ? properties_ + index : nullptr;
}

Type GodotApi::property_getter_type(const GodotPropertyRecord& property) const noexcept {
    if (!property.direct && property.getter[0] != '\0') {
        if (const auto* getter = find_method(property.owner, property.getter);
            getter && getter->return_type[0] != '\0' &&
            std::string_view{getter->return_type} != "void") {
            return type_from_godot_api(getter->return_type);
        }
    }
    return type_from_godot_api(property.type);
}

Type GodotApi::property_setter_type(const GodotPropertyRecord& property) const noexcept {
    if (!property.direct && property.setter[0] != '\0') {
        if (const auto* setter = find_method(property.owner, property.setter)) {
            const auto value_index = property.index >= 0 ? std::size_t{1} : std::size_t{0};
            if (const auto* value = argument(*setter, value_index))
                return type_from_godot_api(value->type);
        }
    }
    return type_from_godot_api(property.type);
}

const GodotSignalRecord* GodotApi::find_signal(std::string_view owner, std::string_view name,
                                               bool include_inherited) const noexcept {
    for (std::size_t depth = 0; !owner.empty() && depth < class_count(); ++depth) {
        if (const auto* signal = find_owned_record(signals_, signal_count_, owner, name))
            return signal;
        if (!include_inherited)
            break;
        const auto* type = find_class(owner);
        if (!type || type->inherits[0] == '\0')
            break;
        owner = type->inherits;
    }
    return nullptr;
}

const GodotArgumentRecord* GodotApi::argument(const GodotSignalRecord& signal,
                                              std::size_t index) const noexcept {
    if (index >= signal.argument_count)
        return nullptr;
    const auto offset = static_cast<std::size_t>(signal.first_argument) + index;
    return offset < argument_count_ ? &arguments_[offset] : nullptr;
}

bool GodotApi::inherits(std::string_view type, std::string_view base) const noexcept {
    if (type == base)
        return true;
    for (std::size_t depth = 0; !type.empty() && depth < class_count(); ++depth) {
        const auto* record = find_class(type);
        if (!record || record->inherits[0] == '\0')
            return false;
        type = record->inherits;
        if (type == base)
            return true;
    }
    return false;
}

const GodotArgumentRecord* GodotApi::argument(const GodotMethodRecord& method,
                                              std::size_t index) const noexcept {
    if (index >= method.maximum_arguments)
        return nullptr;
    const auto offset = static_cast<std::size_t>(method.first_argument) + index;
    return offset < argument_count_ ? &arguments_[offset] : nullptr;
}

const GodotConstructorRecord* GodotApi::find_constructor(std::string_view owner,
                                                         std::size_t argument_count_value,
                                                         std::size_t occurrence) const noexcept {
    const auto first =
        std::lower_bound(constructors_, constructors_ + constructor_count_, owner,
                         [](const GodotConstructorRecord& record, std::string_view value) {
                             return std::string_view{record.owner} < value;
                         });
    for (auto current = first;
         current != constructors_ + constructor_count_ && std::string_view{current->owner} == owner;
         ++current) {
        if (current->argument_count != argument_count_value)
            continue;
        if (occurrence == 0)
            return current;
        --occurrence;
    }
    return nullptr;
}

const GodotArgumentRecord* GodotApi::argument(const GodotConstructorRecord& constructor,
                                              std::size_t index) const noexcept {
    if (index >= constructor.argument_count)
        return nullptr;
    const auto offset = static_cast<std::size_t>(constructor.first_argument) + index;
    return offset < argument_count_ ? &arguments_[offset] : nullptr;
}

const GodotUtilityFunctionRecord*
GodotApi::find_utility_function(std::string_view name) const noexcept {
    const auto found =
        std::lower_bound(utility_functions_, utility_functions_ + utility_function_count_, name,
                         [](const GodotUtilityFunctionRecord& record, std::string_view value) {
                             return std::string_view{record.name} < value;
                         });
    return found == utility_functions_ + utility_function_count_ ||
                   std::string_view{found->name} != name
               ? nullptr
               : found;
}

const GodotArgumentRecord* GodotApi::argument(const GodotUtilityFunctionRecord& function,
                                              std::size_t index) const noexcept {
    if (index >= function.maximum_arguments)
        return nullptr;
    const auto offset = static_cast<std::size_t>(function.first_argument) + index;
    return offset < argument_count_ ? &arguments_[offset] : nullptr;
}

const GodotGlobalConstantRecord*
GodotApi::find_global_constant(std::string_view name) const noexcept {
    const auto found =
        std::lower_bound(global_constants_, global_constants_ + global_constant_count_, name,
                         [](const GodotGlobalConstantRecord& record, std::string_view value) {
                             return std::string_view{record.name} < value;
                         });
    return found == global_constants_ + global_constant_count_ ||
                   std::string_view{found->name} != name
               ? nullptr
               : found;
}

const GodotGlobalEnumValueRecord*
GodotApi::find_global_enum_value(std::string_view owner, std::string_view name) const noexcept {
    const auto key = std::pair{owner, name};
    const auto found = std::lower_bound(
        global_enum_values_, global_enum_values_ + global_enum_value_count_, key,
        [](const GodotGlobalEnumValueRecord& record, const auto& value) {
            return std::pair{std::string_view{record.owner}, std::string_view{record.name}} < value;
        });
    return found == global_enum_values_ + global_enum_value_count_ ||
                   std::string_view{found->owner} != owner || std::string_view{found->name} != name
               ? nullptr
               : found;
}

const GodotGlobalEnumValueRecord*
GodotApi::find_global_enum_value(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < global_enum_value_count_; ++index) {
        if (std::string_view{global_enum_values_[index].name} == name)
            return &global_enum_values_[index];
    }
    return nullptr;
}

bool GodotApi::has_global_enum(std::string_view owner) const noexcept {
    const auto found =
        std::lower_bound(global_enum_values_, global_enum_values_ + global_enum_value_count_, owner,
                         [](const GodotGlobalEnumValueRecord& record, std::string_view value) {
                             return std::string_view{record.owner} < value;
                         });
    return found != global_enum_values_ + global_enum_value_count_ &&
           std::string_view{found->owner} == owner;
}

const GodotBuiltinOperatorRecord*
GodotApi::find_builtin_operator(std::string_view left_type, std::string_view name,
                                std::string_view right_type) const noexcept {
    const auto key = std::tuple{left_type, name, right_type};
    const auto found = std::lower_bound(
        builtin_operators_, builtin_operators_ + builtin_operator_count_, key,
        [](const GodotBuiltinOperatorRecord& record, const auto& value) {
            return std::tuple{std::string_view{record.left_type}, std::string_view{record.name},
                              std::string_view{record.right_type}} < value;
        });
    return found == builtin_operators_ + builtin_operator_count_ ||
                   std::string_view{found->left_type} != left_type ||
                   std::string_view{found->name} != name ||
                   std::string_view{found->right_type} != right_type
               ? nullptr
               : found;
}

const GodotBuiltinConstantRecord*
GodotApi::find_builtin_constant(std::string_view owner, std::string_view name) const noexcept {
    return find_owned_record(builtin_constants_, builtin_constant_count_, owner, name);
}

Type type_from_godot_api(std::string_view raw_type) {
    auto type = first_allowed_type(raw_type);
    if (type.empty() || type == "void")
        return {TypeKind::void_type, "void"};
    if (type == "Nil")
        return {TypeKind::nil, "null"};
    if (type.rfind("enum::", 0) == 0 || type.rfind("bitfield::", 0) == 0)
        return {TypeKind::integer, "int"};
    if (type.rfind("typedarray::", 0) == 0) {
        type.remove_prefix(std::string_view{"typedarray::"}.size());
        return {TypeKind::array,
                type.empty() ? "Array" : "Array[" + collection_element_type(type) + "]"};
    }
    if (type.rfind("typeddictionary::", 0) == 0) {
        type.remove_prefix(std::string_view{"typeddictionary::"}.size());
        const auto separator = type.find(';');
        if (separator == std::string_view::npos || separator == 0 || separator + 1 >= type.size())
            return {TypeKind::dictionary, "Dictionary"};
        return {TypeKind::dictionary,
                "Dictionary[" + collection_element_type(type.substr(0, separator)) + ", " +
                    collection_element_type(type.substr(separator + 1)) + "]"};
    }
    if (type.back() == '*')
        return {TypeKind::variant, "Variant"};
    return type_from_annotation(std::string{type});
}

} // namespace gdpp
