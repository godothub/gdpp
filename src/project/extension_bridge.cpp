#include "gdpp/project/extension_bridge.hpp"

#include "gdpp/core/path_utf8.hpp"
#include "gdpp/support/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

namespace gdpp {
namespace {

struct Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::map<std::string, Json>;
struct Json {
    std::variant<std::nullptr_t, bool, std::int64_t, std::string, JsonArray, JsonObject> value;
};

class JsonParser final {
  public:
    explicit JsonParser(std::string_view source) : source_(source) {}

    std::optional<Json> parse() {
        auto value = parse_value();
        skip_space();
        if (!value || offset_ != source_.size())
            return std::nullopt;
        return value;
    }

  private:
    static std::optional<std::uint32_t> hex_digit(const char value) {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint32_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint32_t>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F')
            return static_cast<std::uint32_t>(value - 'A' + 10);
        return std::nullopt;
    }

    std::optional<std::uint32_t> parse_unicode_code_unit() {
        if (offset_ + 4 > source_.size())
            return std::nullopt;
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto digit = hex_digit(source_[offset_++]);
            if (!digit)
                return std::nullopt;
            value = (value << 4U) | *digit;
        }
        return value;
    }

    static bool append_utf8(std::string& result, const std::uint32_t code_point) {
        if (code_point <= 0x7FU) {
            result.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            if (code_point >= 0xD800U && code_point <= 0xDFFFU)
                return false;
            result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0x10FFFFU) {
            result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            return false;
        }
        return true;
    }

    void skip_space() {
        while (offset_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[offset_])) != 0)
            ++offset_;
    }

    bool consume(char expected) {
        skip_space();
        if (offset_ >= source_.size() || source_[offset_] != expected)
            return false;
        ++offset_;
        return true;
    }

    std::optional<std::string> parse_string() {
        if (!consume('"'))
            return std::nullopt;
        std::string result;
        while (offset_ < source_.size()) {
            const char character = source_[offset_++];
            if (character == '"')
                return result;
            if (character == '\\') {
                if (offset_ >= source_.size())
                    return std::nullopt;
                const char escape = source_[offset_++];
                if (escape == '"' || escape == '\\' || escape == '/')
                    result.push_back(escape);
                else if (escape == 'b')
                    result.push_back('\b');
                else if (escape == 'f')
                    result.push_back('\f');
                else if (escape == 'n')
                    result.push_back('\n');
                else if (escape == 'r')
                    result.push_back('\r');
                else if (escape == 't')
                    result.push_back('\t');
                else if (escape == 'u') {
                    const auto first = parse_unicode_code_unit();
                    if (!first)
                        return std::nullopt;
                    std::uint32_t code_point = *first;
                    if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                        if (offset_ + 2 > source_.size() || source_[offset_] != '\\' ||
                            source_[offset_ + 1] != 'u') {
                            return std::nullopt;
                        }
                        offset_ += 2;
                        const auto second = parse_unicode_code_unit();
                        if (!second || *second < 0xDC00U || *second > 0xDFFFU)
                            return std::nullopt;
                        code_point =
                            0x10000U + ((code_point - 0xD800U) << 10U) + (*second - 0xDC00U);
                    } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                        return std::nullopt;
                    }
                    if (!append_utf8(result, code_point))
                        return std::nullopt;
                } else
                    return std::nullopt;
            } else {
                if (static_cast<unsigned char>(character) < 0x20U)
                    return std::nullopt;
                result.push_back(character);
            }
        }
        return std::nullopt;
    }

    std::optional<Json> parse_value() {
        skip_space();
        if (offset_ >= source_.size())
            return std::nullopt;
        if (source_[offset_] == '"') {
            auto string = parse_string();
            return string ? std::optional<Json>{Json{std::move(*string)}} : std::nullopt;
        }
        if (source_[offset_] == '{')
            return parse_object();
        if (source_[offset_] == '[')
            return parse_array();
        if (source_.substr(offset_, 4) == "true") {
            offset_ += 4;
            return Json{true};
        }
        if (source_.substr(offset_, 5) == "false") {
            offset_ += 5;
            return Json{false};
        }
        if (source_.substr(offset_, 4) == "null") {
            offset_ += 4;
            return Json{nullptr};
        }
        const auto begin = offset_;
        if (source_[offset_] == '-')
            ++offset_;
        while (offset_ < source_.size() &&
               std::isdigit(static_cast<unsigned char>(source_[offset_])) != 0)
            ++offset_;
        if (begin == offset_ || (offset_ == begin + 1 && source_[begin] == '-'))
            return std::nullopt;
        try {
            return Json{std::stoll(std::string{source_.substr(begin, offset_ - begin)})};
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<Json> parse_object() {
        if (!consume('{'))
            return std::nullopt;
        JsonObject object;
        skip_space();
        if (consume('}'))
            return Json{std::move(object)};
        while (true) {
            auto key = parse_string();
            if (!key || !consume(':'))
                return std::nullopt;
            auto value = parse_value();
            if (!value || !object.emplace(std::move(*key), std::move(*value)).second)
                return std::nullopt;
            skip_space();
            if (consume('}'))
                return Json{std::move(object)};
            if (!consume(','))
                return std::nullopt;
        }
    }

    std::optional<Json> parse_array() {
        if (!consume('['))
            return std::nullopt;
        JsonArray array;
        skip_space();
        if (consume(']'))
            return Json{std::move(array)};
        while (true) {
            auto value = parse_value();
            if (!value)
                return std::nullopt;
            array.push_back(std::move(*value));
            skip_space();
            if (consume(']'))
                return Json{std::move(array)};
            if (!consume(','))
                return std::nullopt;
        }
    }

    std::string_view source_;
    std::size_t offset_{0};
};

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

const Json* field(const JsonObject& object, const std::string& key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

const std::string* string_field(const JsonObject& object, const std::string& key) {
    const auto* item = field(object, key);
    return item ? std::get_if<std::string>(&item->value) : nullptr;
}

const bool* bool_field(const JsonObject& object, const std::string& key) {
    const auto* item = field(object, key);
    return item ? std::get_if<bool>(&item->value) : nullptr;
}

const std::int64_t* integer_field(const JsonObject& object, const std::string& key) {
    const auto* item = field(object, key);
    return item ? std::get_if<std::int64_t>(&item->value) : nullptr;
}

const JsonArray* array_field(const JsonObject& object, const std::string& key) {
    const auto* item = field(object, key);
    return item ? std::get_if<JsonArray>(&item->value) : nullptr;
}

bool is_safe_relative(const std::filesystem::path& path) {
    return !path.empty() && !path.is_absolute() && *path.begin() != "..";
}

std::optional<std::filesystem::path> resolve_project_path(const std::filesystem::path& root,
                                                          const std::filesystem::path& owner,
                                                          const std::string& value) {
    constexpr std::string_view resource_prefix{"res://"};
    std::filesystem::path relative = value.rfind(resource_prefix, 0) == 0
                                         ? path_from_utf8(value.substr(resource_prefix.size()))
                                         : owner / path_from_utf8(value);
    relative = relative.lexically_normal();
    if (!is_safe_relative(relative))
        return std::nullopt;
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error)
        return std::nullopt;
    const auto absolute = std::filesystem::weakly_canonical(root / relative, error);
    if (error)
        return std::nullopt;
    const auto contained = absolute.lexically_relative(canonical_root);
    if (!contained.empty() && *contained.begin() == "..")
        return std::nullopt;
    return absolute;
}

bool valid_identifier(std::string_view value, bool qualified) {
    if (value.empty())
        return false;
    bool segment_start = true;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (qualified && character == ':' && index + 1 < value.size() && value[index + 1] == ':') {
            if (segment_start)
                return false;
            segment_start = true;
            ++index;
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        if (segment_start) {
            if (std::isalpha(byte) == 0 && character != '_')
                return false;
            segment_start = false;
        } else if (std::isalnum(byte) == 0 && character != '_') {
            return false;
        }
    }
    return !segment_start;
}

bool valid_type_name(std::string_view value) {
    if (value.empty())
        return false;
    int bracket_depth = 0;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '_' || character == '.')
            continue;
        if (character == '[') {
            ++bracket_depth;
            continue;
        }
        if (character == ']' && bracket_depth > 0) {
            --bracket_depth;
            continue;
        }
        if (character == ',' && bracket_depth > 0)
            continue;
        return false;
    }
    return bracket_depth == 0;
}

bool parse_bridge_parameters(const JsonObject& member, const std::string& member_name,
                             const std::filesystem::path& manifest,
                             std::vector<ExtensionBridgeParameter>& output,
                             std::vector<std::string>& diagnostics) {
    const auto* value = field(member, "parameters");
    if (!value)
        return true;
    const auto* parameters = std::get_if<JsonArray>(&value->value);
    if (!parameters) {
        diagnostics.push_back(path_to_utf8(manifest) + ": bridge member '" + member_name +
                              "' field 'parameters' must be an array");
        return false;
    }
    bool valid = true;
    bool saw_default = false;
    std::set<std::string> names;
    for (const auto& parameter_value : *parameters) {
        const auto* parameter = std::get_if<JsonObject>(&parameter_value.value);
        const auto* name = parameter ? string_field(*parameter, "name") : nullptr;
        const auto* type = parameter ? string_field(*parameter, "type") : nullptr;
        if (!name || !valid_identifier(*name, false) || (type && !valid_type_name(*type)) ||
            !names.emplace(name ? *name : std::string{}).second) {
            diagnostics.push_back(path_to_utf8(manifest) + ": bridge member '" + member_name +
                                  "' has an invalid or duplicate parameter");
            valid = false;
            continue;
        }
        bool has_default = false;
        if (field(*parameter, "has_default")) {
            const auto* flag = bool_field(*parameter, "has_default");
            if (!flag) {
                diagnostics.push_back(path_to_utf8(manifest) + ": bridge member '" + member_name +
                                      "' parameter '" + *name +
                                      "' field 'has_default' must be boolean");
                valid = false;
                continue;
            }
            has_default = *flag;
        }
        if (saw_default && !has_default) {
            diagnostics.push_back(path_to_utf8(manifest) + ": bridge member '" + member_name +
                                  "' has a required parameter after an optional parameter");
            valid = false;
            continue;
        }
        saw_default = saw_default || has_default;
        output.push_back({*name, type ? *type : "Variant", has_default});
    }
    return valid;
}

bool parse_bridge_member_field(const JsonObject& object, const std::string& class_name,
                               const std::string& field_name, ExtensionBridgeMemberKind kind,
                               const std::filesystem::path& manifest, std::set<std::string>& names,
                               std::vector<ExtensionBridgeMember>& output,
                               std::vector<std::string>& diagnostics) {
    const auto* value = field(object, field_name);
    if (!value)
        return true;
    const auto* members = std::get_if<JsonArray>(&value->value);
    if (!members) {
        diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" + class_name +
                              "' field '" + field_name + "' must be an array");
        return false;
    }
    bool valid = true;
    for (const auto& member_value : *members) {
        const auto* member = std::get_if<JsonObject>(&member_value.value);
        const auto* name = member ? string_field(*member, "name") : nullptr;
        const char* type_key = kind == ExtensionBridgeMemberKind::method ? "return_type" : "type";
        const auto* type = member ? string_field(*member, type_key) : nullptr;
        if (!name || !valid_identifier(*name, false) || (type && !valid_type_name(*type)) ||
            !names.emplace(name ? *name : std::string{}).second) {
            diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" + class_name +
                                  "' has an invalid or duplicate " + field_name + " entry");
            valid = false;
            continue;
        }
        ExtensionBridgeMember bridge_member;
        bridge_member.kind = kind;
        bridge_member.name = *name;
        bridge_member.type = type                                        ? *type
                             : kind == ExtensionBridgeMemberKind::signal ? "Signal"
                                                                         : "Variant";
        if (kind != ExtensionBridgeMemberKind::property &&
            !parse_bridge_parameters(*member, *name, manifest, bridge_member.parameters,
                                     diagnostics)) {
            valid = false;
            continue;
        }
        const char* flag_name =
            kind == ExtensionBridgeMemberKind::property ? "read_only" : "vararg";
        if (kind != ExtensionBridgeMemberKind::signal && field(*member, flag_name)) {
            const auto* flag = bool_field(*member, flag_name);
            if (!flag) {
                diagnostics.push_back(path_to_utf8(manifest) + ": bridge member '" + *name +
                                      "' field '" + flag_name + "' must be boolean");
                valid = false;
                continue;
            }
            if (kind == ExtensionBridgeMemberKind::property)
                bridge_member.read_only = *flag;
            else
                bridge_member.vararg = *flag;
        }
        if (kind == ExtensionBridgeMemberKind::method && field(*member, "static")) {
            const auto* flag = bool_field(*member, "static");
            if (!flag) {
                diagnostics.push_back(path_to_utf8(manifest) + ": bridge member '" + *name +
                                      "' field 'static' must be boolean");
                valid = false;
                continue;
            }
            bridge_member.is_static = *flag;
        }
        if (kind == ExtensionBridgeMemberKind::method && field(*member, "hash")) {
            const auto* hash = integer_field(*member, "hash");
            if (!hash || *hash < 0 ||
                static_cast<std::uint64_t>(*hash) >
                    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                diagnostics.push_back(path_to_utf8(manifest) + ": bridge method '" + *name +
                                      "' field 'hash' must be an unsigned 32-bit integer");
                valid = false;
                continue;
            }
            bridge_member.method_hash = static_cast<std::uint32_t>(*hash);
            bridge_member.has_method_hash = true;
        }
        output.push_back(std::move(bridge_member));
    }
    return valid;
}

bool parse_bridge_members(const JsonObject& object, ExtensionBridgeClass& bridge_class,
                          const std::filesystem::path& manifest,
                          std::vector<std::string>& diagnostics) {
    bool valid = true;
    if (field(object, "editor_only")) {
        const auto* editor_only = bool_field(object, "editor_only");
        if (!editor_only) {
            diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" +
                                  bridge_class.gdscript_name +
                                  "' field 'editor_only' must be boolean");
            valid = false;
        } else {
            bridge_class.editor_only = *editor_only;
        }
    }
    if (field(object, "members_complete")) {
        const auto* complete = bool_field(object, "members_complete");
        if (!complete) {
            diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" +
                                  bridge_class.gdscript_name +
                                  "' field 'members_complete' must be boolean");
            valid = false;
        } else {
            bridge_class.members_complete = *complete;
        }
    }
    std::set<std::string> names;
    valid = parse_bridge_member_field(object, bridge_class.gdscript_name, "properties",
                                      ExtensionBridgeMemberKind::property, manifest, names,
                                      bridge_class.members, diagnostics) &&
            valid;
    valid = parse_bridge_member_field(object, bridge_class.gdscript_name, "methods",
                                      ExtensionBridgeMemberKind::method, manifest, names,
                                      bridge_class.members, diagnostics) &&
            valid;
    valid = parse_bridge_member_field(object, bridge_class.gdscript_name, "signals",
                                      ExtensionBridgeMemberKind::signal, manifest, names,
                                      bridge_class.members, diagnostics) &&
            valid;
    return valid;
}

bool parse_bridge_enums(const JsonObject& object, ExtensionBridgeClass& bridge_class,
                        const std::filesystem::path& manifest,
                        std::vector<std::string>& diagnostics) {
    const auto* value = field(object, "enums");
    if (!value)
        return true;
    const auto* enums = std::get_if<JsonArray>(&value->value);
    if (!enums) {
        diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" +
                              bridge_class.gdscript_name + "' field 'enums' must be an array");
        return false;
    }

    std::set<std::string> class_names;
    for (const auto& member : bridge_class.members)
        class_names.insert(member.name);
    bool valid = true;
    for (const auto& enum_value : *enums) {
        const auto* enumeration = std::get_if<JsonObject>(&enum_value.value);
        const auto* name = enumeration ? string_field(*enumeration, "name") : nullptr;
        const auto* entries = enumeration ? array_field(*enumeration, "values") : nullptr;
        if (!name || !valid_identifier(*name, false) || !entries || entries->empty() ||
            !class_names.insert(name ? *name : std::string{}).second) {
            diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" +
                                  bridge_class.gdscript_name +
                                  "' has an invalid, empty or duplicate enum");
            valid = false;
            continue;
        }
        ExtensionBridgeEnum bridge_enum;
        bridge_enum.name = *name;
        if (field(*enumeration, "bitfield")) {
            const auto* bitfield = bool_field(*enumeration, "bitfield");
            if (!bitfield) {
                diagnostics.push_back(path_to_utf8(manifest) + ": bridge enum '" + *name +
                                      "' field 'bitfield' must be boolean");
                valid = false;
                continue;
            }
            bridge_enum.is_bitfield = *bitfield;
        }
        std::set<std::string> entry_names;
        bool enum_valid = true;
        for (const auto& entry_value : *entries) {
            const auto* entry = std::get_if<JsonObject>(&entry_value.value);
            const auto* entry_name = entry ? string_field(*entry, "name") : nullptr;
            const auto* entry_number = entry ? integer_field(*entry, "value") : nullptr;
            if (!entry_name || !valid_identifier(*entry_name, false) || !entry_number ||
                !entry_names.insert(entry_name ? *entry_name : std::string{}).second ||
                !class_names.insert(entry_name ? *entry_name : std::string{}).second) {
                diagnostics.push_back(path_to_utf8(manifest) + ": bridge enum '" + *name +
                                      "' has an invalid or duplicate value");
                valid = false;
                enum_valid = false;
                continue;
            }
            bridge_enum.entries.push_back({*entry_name, *entry_number});
        }
        if (enum_valid && bridge_enum.entries.size() == entries->size())
            bridge_class.enums.push_back(std::move(bridge_enum));
    }
    return valid;
}

} // namespace

ExtensionBridgeLoadResult
load_extension_bridges(const std::filesystem::path& project_root,
                       const std::vector<std::filesystem::path>& manifests,
                       GodotVersion target_version) {
    ExtensionBridgeLoadResult result;
    std::map<std::string, std::filesystem::path> class_owners;
    for (const auto& manifest : manifests) {
        const auto source = read_file(manifest);
        const auto parsed = source ? JsonParser{*source}.parse() : std::nullopt;
        const auto* object = parsed ? std::get_if<JsonObject>(&parsed->value) : nullptr;
        if (!object) {
            result.diagnostics.push_back(path_to_utf8(manifest) + ": invalid JSON bridge manifest");
            continue;
        }
        const auto* schema_value = field(*object, "schema");
        const auto* schema =
            schema_value ? std::get_if<std::int64_t>(&schema_value->value) : nullptr;
        const auto* provider = string_field(*object, "provider");
        const auto* abi = string_field(*object, "abi");
        const auto* minimum = string_field(*object, "godot_minimum");
        const auto* classes = array_field(*object, "classes");
        if (!schema || *schema != 1 || !provider || provider->empty() || !abi || abi->empty() ||
            !minimum || !classes || classes->empty()) {
            result.diagnostics.push_back(
                path_to_utf8(manifest) +
                ": bridge requires schema=1, provider, abi, godot_minimum and classes");
            continue;
        }
        const auto parsed_minimum = parse_godot_version(*minimum);
        if (!parsed_minimum || *parsed_minimum > target_version) {
            result.diagnostics.push_back(path_to_utf8(manifest) + ": bridge requires Godot " +
                                         *minimum + " but the project target is older");
            continue;
        }
        const auto owner = manifest.parent_path().lexically_relative(project_root);
        const auto provider_path = resolve_project_path(project_root, owner, *provider);
        if (!provider_path || provider_path->extension() != ".gdextension" ||
            !std::filesystem::is_regular_file(*provider_path)) {
            result.diagnostics.push_back(path_to_utf8(manifest) +
                                         ": provider must resolve to an existing .gdextension");
            continue;
        }
        ExtensionBridge bridge;
        bridge.manifest_path = manifest;
        bridge.provider_descriptor = *provider_path;
        bridge.provider = *provider;
        bridge.abi = *abi;
        bridge.contract_hash = sha256(*source);
        bridge.minimum_godot_version = *parsed_minimum;
        for (const auto& item : *classes) {
            const auto* class_object = std::get_if<JsonObject>(&item.value);
            const auto* gdscript_name =
                class_object ? string_field(*class_object, "gdscript_name") : nullptr;
            const auto* mode = class_object ? string_field(*class_object, "mode") : nullptr;
            const auto* godot_base =
                class_object ? string_field(*class_object, "godot_base") : nullptr;
            // Both legacy modes are consumed as runtime ClassDB contracts. Direct native
            // inheritance across GDExtension libraries is unsupported by Godot, while the
            // attached ScriptInstance backend needs neither provider headers nor link inputs.
            if (mode && *mode != "runtime" && *mode != "native") {
                result.diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" +
                                             (gdscript_name ? *gdscript_name : "<unknown>") +
                                             "' has unsupported mode '" + *mode + "'");
                continue;
            }
            if (!gdscript_name || !godot_base || !valid_identifier(*gdscript_name, false) ||
                !valid_identifier(*godot_base, false)) {
                result.diagnostics.push_back(path_to_utf8(manifest) +
                                             ": every bridge class needs valid GDScript and Godot "
                                             "base names");
                continue;
            }
            ExtensionBridgeClass bridge_class{*gdscript_name, {},    {}, *godot_base, true,
                                              false,          false, {}, {}};
            if (!parse_bridge_members(*class_object, bridge_class, manifest, result.diagnostics))
                continue;
            if (!parse_bridge_enums(*class_object, bridge_class, manifest, result.diagnostics))
                continue;
            const auto [existing, unique] = class_owners.emplace(*gdscript_name, manifest);
            if (!unique) {
                result.diagnostics.push_back(path_to_utf8(manifest) + ": bridge class '" +
                                             *gdscript_name + "' is already provided by " +
                                             path_to_utf8(existing->second));
                continue;
            }
            bridge.classes.push_back(std::move(bridge_class));
        }
        if (!bridge.classes.empty())
            result.bridges.push_back(std::move(bridge));
    }
    if (!result.diagnostics.empty())
        result.bridges.clear();
    return result;
}

std::optional<ExtensionBridgeTarget> select_extension_bridge_target(const ExtensionBridge& bridge,
                                                                    std::string_view platform,
                                                                    std::string_view architecture,
                                                                    std::string_view profile) {
    const auto found = std::find_if(
        bridge.targets.begin(), bridge.targets.end(), [&](const ExtensionBridgeTarget& target) {
            return target.platform == platform && target.architecture == architecture &&
                   target.profile == profile;
        });
    return found == bridge.targets.end() ? std::nullopt : std::optional{*found};
}

std::string write_extension_bridge_lock(const std::vector<ExtensionBridge>& bridges) {
    std::ostringstream output;
    output << "GDPP_BRIDGE_LOCK 1\n";
    for (const auto& bridge : bridges) {
        output << "bridge " << std::quoted(bridge.abi) << ' '
               << std::quoted(path_to_utf8(bridge.manifest_path)) << '\n';
        const bool runtime_only =
            std::all_of(bridge.classes.begin(), bridge.classes.end(),
                        [](const ExtensionBridgeClass& type) { return type.runtime_only; });
        if (runtime_only)
            output << "runtime\n";
        for (const auto& target : bridge.targets) {
            output << "target " << std::quoted(target.platform) << ' '
                   << std::quoted(target.architecture) << ' ' << std::quoted(target.profile) << ' '
                   << target.include_directories.size();
            for (const auto& include : target.include_directories)
                output << ' ' << std::quoted(path_to_utf8(include));
            output << ' ' << target.link_libraries.size();
            for (const auto& library : target.link_libraries)
                output << ' ' << std::quoted(path_to_utf8(library));
            output << '\n';
        }
    }
    return output.str();
}

} // namespace gdpp
