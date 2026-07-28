#include "gdpp/semantic/intrinsics.hpp"

#include <algorithm>
#include <array>

namespace gdpp {
namespace {

constexpr auto features = std::array{
    IntrinsicFeature{IntrinsicKind::color8,
                     "Color8",
                     3,
                     4,
                     {IntrinsicArgumentRule::integer, IntrinsicArgumentRule::integer,
                      IntrinsicArgumentRule::integer, IntrinsicArgumentRule::integer},
                     IntrinsicResultRule::color,
                     "gdpp::runtime::color8",
                     true},
    IntrinsicFeature{IntrinsicKind::character,
                     "char",
                     1,
                     1,
                     {IntrinsicArgumentRule::integer},
                     IntrinsicResultRule::string,
                     "gdpp::runtime::character",
                     true},
    IntrinsicFeature{IntrinsicKind::convert,
                     "convert",
                     2,
                     2,
                     {IntrinsicArgumentRule::any, IntrinsicArgumentRule::integer},
                     IntrinsicResultRule::dynamic,
                     "gdpp::runtime::convert_value",
                     true},
    IntrinsicFeature{IntrinsicKind::dictionary_to_instance,
                     "dict_to_inst",
                     1,
                     1,
                     {IntrinsicArgumentRule::dictionary},
                     IntrinsicResultRule::object,
                     "gdpp::runtime::dictionary_to_instance",
                     false},
    IntrinsicFeature{IntrinsicKind::get_stack,
                     "get_stack",
                     0,
                     0,
                     {},
                     IntrinsicResultRule::array,
                     "gdpp::runtime::get_stack",
                     false},
    IntrinsicFeature{IntrinsicKind::instance_to_dictionary,
                     "inst_to_dict",
                     1,
                     1,
                     {IntrinsicArgumentRule::object},
                     IntrinsicResultRule::dictionary,
                     "gdpp::runtime::instance_to_dictionary",
                     false},
    IntrinsicFeature{IntrinsicKind::is_instance_of,
                     "is_instance_of",
                     2,
                     2,
                     {IntrinsicArgumentRule::any, IntrinsicArgumentRule::type_descriptor},
                     IntrinsicResultRule::boolean,
                     "gdpp::runtime::is_instance_of",
                     true},
    IntrinsicFeature{IntrinsicKind::length,
                     "len",
                     1,
                     1,
                     {IntrinsicArgumentRule::any},
                     IntrinsicResultRule::integer,
                     "gdpp::runtime::length",
                     true},
    IntrinsicFeature{IntrinsicKind::load,
                     "load",
                     1,
                     1,
                     {IntrinsicArgumentRule::resource_path},
                     IntrinsicResultRule::resource,
                     "gdpp::runtime::load_resource",
                     false},
    IntrinsicFeature{IntrinsicKind::ordinal,
                     "ord",
                     1,
                     1,
                     {IntrinsicArgumentRule::string},
                     IntrinsicResultRule::integer,
                     "gdpp::runtime::ordinal",
                     true},
    IntrinsicFeature{IntrinsicKind::preload,
                     "preload",
                     1,
                     1,
                     {IntrinsicArgumentRule::resource_path},
                     IntrinsicResultRule::resource,
                     "gdpp::runtime::load_resource",
                     true},
    IntrinsicFeature{IntrinsicKind::print_debug,
                     "print_debug",
                     0,
                     0,
                     {},
                     IntrinsicResultRule::void_type,
                     "gdpp::runtime::print_debug",
                     false},
    IntrinsicFeature{IntrinsicKind::print_stack,
                     "print_stack",
                     0,
                     0,
                     {},
                     IntrinsicResultRule::void_type,
                     "gdpp::runtime::print_stack",
                     false},
    IntrinsicFeature{IntrinsicKind::range,
                     "range",
                     0,
                     3,
                     {IntrinsicArgumentRule::integer, IntrinsicArgumentRule::integer,
                      IntrinsicArgumentRule::integer},
                     IntrinsicResultRule::integer_array,
                     "gdpp::runtime::make_range_checked",
                     false},
    IntrinsicFeature{IntrinsicKind::type_exists,
                     "type_exists",
                     1,
                     1,
                     {IntrinsicArgumentRule::string_name},
                     IntrinsicResultRule::boolean,
                     "gdpp::runtime::type_exists",
                     true},
};

static_assert(
    [] {
        for (std::size_t index = 1; index < features.size(); ++index) {
            if (features[index - 1].name >= features[index].name)
                return false;
        }
        return true;
    }(),
    "intrinsic registry must remain sorted by name");

} // namespace

bool intrinsic_is_vararg(const IntrinsicKind kind) noexcept {
    return kind == IntrinsicKind::print_debug || kind == IntrinsicKind::range;
}

const IntrinsicRegistry& IntrinsicRegistry::latest() noexcept {
    static const IntrinsicRegistry registry;
    return registry;
}

const IntrinsicFeature* IntrinsicRegistry::find(const std::string_view name) const noexcept {
    const auto found =
        std::lower_bound(features.begin(), features.end(), name,
                         [](const IntrinsicFeature& feature, const std::string_view candidate) {
                             return feature.name < candidate;
                         });
    return found != features.end() && found->name == name ? &*found : nullptr;
}

const IntrinsicFeature* IntrinsicRegistry::find(const IntrinsicKind kind) const noexcept {
    const auto found = std::find_if(features.begin(), features.end(),
                                    [kind](const auto& feature) { return feature.kind == kind; });
    return found == features.end() ? nullptr : &*found;
}

} // namespace gdpp
