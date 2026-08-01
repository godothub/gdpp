#include "gdpp/project/project_compiler.hpp"

#include "gdpp/core/compiler_stack.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/frontend/constant_evaluator.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/parser.hpp"
#include "gdpp/project/extension_bridge.hpp"
#include "gdpp/project/native_builder.hpp"
#include "gdpp/project/native_contract.hpp"
#include "gdpp/semantic/analyzer.hpp"
#include "gdpp/semantic/godot_api.hpp"
#include "gdpp/core/path_utf8.hpp"
#include "gdpp/support/sha256.hpp"
#include "gdpp/version.hpp"

#include "project_file_selector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace gdpp {
namespace {

struct ManifestEntry {
    std::string header;
    std::string source;
    std::string symbols;
};

using Manifest = std::map<std::string, ManifestEntry>;

struct EmbeddedScriptSource {
    std::string id;
    std::string source;
};

struct EmbeddedScriptScan {
    std::vector<EmbeddedScriptSource> scripts;
    std::vector<std::string> unresolved_ids;
};

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::optional<std::string> quoted_attribute(std::string_view line, std::string_view attribute) {
    const auto key = std::string{attribute} + "=\"";
    std::size_t search_from = 0;
    while (true) {
        const auto begin = line.find(key, search_from);
        if (begin == std::string_view::npos)
            return std::nullopt;
        const bool complete_name = begin == 0 ||
                                   std::isspace(static_cast<unsigned char>(line[begin - 1])) != 0 ||
                                   line[begin - 1] == '[';
        if (complete_name) {
            const auto value_begin = begin + key.size();
            const auto end = line.find('"', value_begin);
            if (end == std::string_view::npos)
                return std::nullopt;
            return std::string{line.substr(value_begin, end - value_begin)};
        }
        search_from = begin + 1;
    }
}

std::optional<std::string> text_resource_uid(std::string_view source) {
    const auto line_end = source.find_first_of("\r\n");
    const auto first_line = source.substr(0, line_end);
    auto uid = quoted_attribute(first_line, "uid");
    if (!uid || uid->rfind("uid://", 0) != 0)
        return std::nullopt;
    return uid;
}

std::optional<std::string> uid_sidecar_value(std::string_view source) {
    const auto first = source.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return std::nullopt;
    const auto last = source.find_last_not_of(" \t\r\n");
    std::string uid{source.substr(first, last - first + 1)};
    if (uid.rfind("uid://", 0) != 0)
        return std::nullopt;
    return uid;
}

std::optional<std::string> imported_resource_uid(std::string_view source) {
    std::size_t offset = 0;
    while (offset < source.size()) {
        const auto line_end = source.find_first_of("\r\n", offset);
        auto line =
            source.substr(offset, line_end == std::string_view::npos ? source.size() - offset
                                                                     : line_end - offset);
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            line.remove_prefix(first);
            if (line.rfind("uid=", 0) == 0) {
                auto uid = quoted_attribute(line, "uid");
                if (uid && uid->rfind("uid://", 0) == 0)
                    return uid;
            }
        }
        if (line_end == std::string_view::npos)
            break;
        offset = line_end + 1;
        if (offset < source.size() && source[line_end] == '\r' && source[offset] == '\n')
            ++offset;
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> text_resource_references(std::string_view source) {
    std::vector<std::pair<std::string, std::string>> references;
    std::size_t offset = 0;
    while (offset < source.size()) {
        const auto line_end = source.find_first_of("\r\n", offset);
        auto line =
            source.substr(offset, line_end == std::string_view::npos ? source.size() - offset
                                                                     : line_end - offset);
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            line.remove_prefix(first);
            if (line.rfind("[ext_resource", 0) == 0) {
                const auto uid = quoted_attribute(line, "uid");
                const auto path = quoted_attribute(line, "path");
                if (uid && path && uid->rfind("uid://", 0) == 0 && path->rfind("res://", 0) == 0) {
                    references.emplace_back(*uid, path->substr(6));
                }
            }
        }
        if (line_end == std::string_view::npos)
            break;
        offset = line_end + 1;
        if (offset < source.size() && source[line_end] == '\r' && source[offset] == '\n')
            ++offset;
    }
    return references;
}

std::optional<std::string> godot_string_literal(std::string_view value) {
    const auto quote = value.find('"');
    if (quote == std::string_view::npos)
        return std::nullopt;
    std::string result;
    for (std::size_t index = quote + 1; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '"')
            return result;
        if (character != '\\') {
            result.push_back(character);
            continue;
        }
        if (++index >= value.size())
            return std::nullopt;
        switch (value[index]) {
        case 'n':
            result.push_back('\n');
            break;
        case 'r':
            result.push_back('\r');
            break;
        case 't':
            result.push_back('\t');
            break;
        case '"':
            result.push_back('"');
            break;
        case '\\':
            result.push_back('\\');
            break;
        default:
            // Preserve less common Godot escapes for the lexer instead of silently
            // changing the embedded source text.
            result.push_back('\\');
            result.push_back(value[index]);
            break;
        }
    }
    return std::nullopt;
}

struct TextResourceSection {
    std::string_view header;
    std::string_view body;
};

std::vector<TextResourceSection> text_resource_sections(std::string_view resource) {
    std::vector<TextResourceSection> sections;
    std::size_t section_begin = 0;
    while (section_begin < resource.size()) {
        const auto line_end = resource.find('\n', section_begin);
        const auto header_end = line_end == std::string_view::npos ? resource.size() : line_end;
        const auto header = resource.substr(section_begin, header_end - section_begin);
        const auto body_begin = line_end == std::string_view::npos ? resource.size() : line_end + 1;
        auto section_end = resource.find("\n[", body_begin);
        if (section_end == std::string_view::npos)
            section_end = resource.size();
        if (!header.empty() && header.front() == '[')
            sections.push_back({header, resource.substr(body_begin, section_end - body_begin)});
        section_begin = section_end == resource.size() ? resource.size() : section_end + 1;
    }
    return sections;
}

bool section_references_script(const TextResourceSection& section, std::string_view id) {
    std::istringstream lines{std::string{section.body}};
    std::string line;
    const auto reference = "SubResource(\"" + std::string{id} + "\")";
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            continue;
        const std::string_view trimmed{line.data() + first, line.size() - first};
        if (trimmed.rfind("script", 0) == 0 && trimmed.find(reference) != std::string_view::npos)
            return true;
    }
    return false;
}

std::optional<std::string> section_native_type(const TextResourceSection& section,
                                               const std::optional<std::string>& root_type) {
    if (section.header.rfind("[resource", 0) == 0)
        return root_type;
    if (section.header.rfind("[node", 0) == 0 || section.header.rfind("[sub_resource", 0) == 0)
        return quoted_attribute(section.header, "type");
    return std::nullopt;
}

EmbeddedScriptScan embedded_gdscript_sources(std::string_view resource) {
    EmbeddedScriptScan result;
    const auto sections = text_resource_sections(resource);
    std::optional<std::string> root_type;
    if (!sections.empty() && sections.front().header.rfind("[gd_resource", 0) == 0)
        root_type = quoted_attribute(sections.front().header, "type");

    for (const auto& section : sections) {
        const auto header = section.header;
        if (header.rfind("[sub_resource", 0) != 0 ||
            header.find("type=\"GDScript\"") == std::string_view::npos)
            continue;
        const auto id = quoted_attribute(header, "id");
        if (!id)
            continue;
        const auto property = section.body.find("script/source");
        if (property != std::string_view::npos) {
            const auto equal =
                section.body.find('=', property + std::string_view{"script/source"}.size());
            if (equal != std::string_view::npos) {
                if (auto source = godot_string_literal(section.body.substr(equal + 1))) {
                    result.scripts.push_back({*id, std::move(*source)});
                    continue;
                }
            }
        }

        // Godot can serialize an empty built-in GDScript without script/source. A native
        // replacement still needs the concrete owner type: using GDScript's implicit RefCounted
        // base would lose Resource/Node identity during export conversion.
        std::optional<std::string> owner_type;
        for (const auto& owner : sections) {
            if (!section_references_script(owner, *id))
                continue;
            const auto candidate = section_native_type(owner, root_type);
            if (!candidate)
                continue;
            if (owner_type && *owner_type != *candidate) {
                owner_type.reset();
                break;
            }
            owner_type = candidate;
        }
        if (owner_type)
            result.scripts.push_back({*id, "extends " + *owner_type + "\n"});
        else
            result.unresolved_ids.push_back(*id);
    }
    return result;
}

std::optional<std::string> scene_root_script(const std::filesystem::path& project_root,
                                             const std::string& scene_relative) {
    const auto scene_path = path_from_utf8(scene_relative);
    if (scene_path.extension() != ".tscn")
        return std::nullopt;
    const auto content = read_file(project_root / scene_path);
    if (!content)
        return std::nullopt;

    std::unordered_map<std::string, std::string> script_resources;
    std::istringstream stream{*content};
    std::string line;
    bool in_root_node = false;
    while (std::getline(stream, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == ';' || line[first] == '#')
            continue;
        const std::string_view trimmed{line.data() + first, line.size() - first};
        if (trimmed.rfind("[ext_resource", 0) == 0 &&
            trimmed.find("type=\"Script\"") != std::string_view::npos) {
            const auto id = quoted_attribute(trimmed, "id");
            auto path = quoted_attribute(trimmed, "path");
            if (!id || !path)
                continue;
            constexpr std::string_view resource_prefix{"res://"};
            if (path->rfind(resource_prefix, 0) == 0)
                path->erase(0, resource_prefix.size());
            script_resources.emplace(
                *id, generic_path_to_utf8(path_from_utf8(*path).lexically_normal()));
            continue;
        }
        if (trimmed.rfind("[node", 0) == 0) {
            if (in_root_node)
                break;
            in_root_node = true;
            continue;
        }
        if (!in_root_node || trimmed.rfind("script", 0) != 0)
            continue;
        const auto ext_resource = trimmed.find("ExtResource(\"");
        if (ext_resource == std::string_view::npos)
            continue;
        const auto id_begin = ext_resource + std::string_view{"ExtResource(\""}.size();
        const auto id_end = trimmed.find('"', id_begin);
        if (id_end == std::string_view::npos)
            continue;
        const auto resource =
            script_resources.find(std::string{trimmed.substr(id_begin, id_end - id_begin)});
        if (resource != script_resources.end())
            return resource->second;
    }
    return std::nullopt;
}

struct ProjectAutoloadScan {
    std::unordered_map<std::string, std::string> scripts;
    std::vector<std::pair<std::string, std::string>> unresolved_uids;
};

ProjectAutoloadScan
read_project_autoloads(const std::filesystem::path& project_file,
                       const std::map<std::string, std::string>& resource_aliases) {
    ProjectAutoloadScan result;
    const auto content = read_file(project_file);
    if (!content)
        return result;
    std::istringstream stream{*content};
    std::string line;
    bool in_autoload_section = false;
    while (std::getline(stream, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == ';' || line[first] == '#')
            continue;
        if (line[first] == '[') {
            in_autoload_section = line.compare(first, 10, "[autoload]") == 0;
            continue;
        }
        if (!in_autoload_section)
            continue;
        const auto equal = line.find('=', first);
        if (equal == std::string::npos)
            continue;
        auto name = line.substr(first, equal - first);
        const auto name_end = name.find_last_not_of(" \t");
        if (name_end == std::string::npos)
            continue;
        name.resize(name_end + 1);
        const auto quote = line.find('"', equal + 1);
        const auto last_quote = line.find_last_of('"');
        if (quote == std::string::npos || last_quote <= quote)
            continue;
        auto path = line.substr(quote + 1, last_quote - quote - 1);
        if (!path.empty() && path.front() == '*')
            path.erase(path.begin());
        if (path.rfind("uid://", 0) == 0) {
            const auto resolved = resource_aliases.find(path);
            if (resolved == resource_aliases.end()) {
                result.unresolved_uids.emplace_back(std::move(name), std::move(path));
                continue;
            }
            path = resolved->second;
        }
        constexpr std::string_view resource_prefix{"res://"};
        if (path.rfind(resource_prefix, 0) == 0)
            path.erase(0, resource_prefix.size());
        if (path.empty())
            continue;
        path = generic_path_to_utf8(path_from_utf8(path).lexically_normal());
        if (const auto script = scene_root_script(project_file.parent_path(), path))
            path = *script;
        if (path_from_utf8(path).extension() == ".gd")
            result.scripts.emplace(std::move(path), std::move(name));
    }
    return result;
}

bool write_file_if_changed(const std::filesystem::path& path, const std::string& content) {
    if (const auto existing = read_file(path); existing && *existing == content)
        return true;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output.good())
            return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (!error)
        return true;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool inside_root(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

std::string to_pascal_case(std::string_view value) {
    std::string result;
    bool uppercase = true;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) == 0) {
            uppercase = true;
            continue;
        }
        result.push_back(uppercase ? static_cast<char>(std::toupper(byte)) : character);
        uppercase = false;
    }
    return result.empty() ? "GeneratedScript" : result;
}

std::string to_snake_case(std::string_view value) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (std::isupper(byte) != 0) {
            const bool previous_is_lower =
                index > 0 && std::islower(static_cast<unsigned char>(value[index - 1])) != 0;
            const bool next_starts_word =
                index > 0 && index + 1 < value.size() &&
                std::islower(static_cast<unsigned char>(value[index + 1])) != 0;
            if (!result.empty() && result.back() != '_' && (previous_is_lower || next_starts_word))
                result.push_back('_');
            result.push_back(static_cast<char>(std::tolower(byte)));
        } else if (std::isalnum(byte) != 0 || value[index] == '_') {
            result.push_back(value[index]);
        } else {
            result.push_back('_');
        }
    }
    return result.empty() ? "generated_script" : result;
}

std::string native_class_stem(const std::string& relative, const std::string& script_name,
                              const bool globally_named) {
    if (globally_named)
        return script_name;
    auto path = path_from_utf8(relative);
    path.replace_extension();
    // Unnamed scripts are resources, not global types. Deriving their C++ identity from the
    // complete resource path prevents common file names such as player.gd/opponent.gd from
    // colliding while the digest makes separator/case normalization collisions deterministic.
    return "Path_" + to_pascal_case(generic_path_to_utf8(path)) + "_" +
           sha256(relative).substr(0, 12);
}

std::string normalized_script_reference(const std::string& owner, const std::string& reference) {
    constexpr std::string_view resource_prefix{"res://"};
    std::filesystem::path path;
    if (reference.rfind(resource_prefix, 0) == 0) {
        path = path_from_utf8(reference.substr(resource_prefix.size()));
    } else {
        path = path_from_utf8(owner).parent_path() / path_from_utf8(reference);
    }
    return generic_path_to_utf8(path.lexically_normal());
}

std::optional<std::string> direct_preload_path(const ast::Expression* expression) {
    if (!expression)
        return std::nullopt;
    const auto* call = expression->get_if<ast::CallExpression>();
    if (!call || !call->callee || call->callee->kind() != ast::ExpressionKind::identifier ||
        call->callee->value() != "preload" || call->arguments.size() != 1 ||
        !call->arguments.front() ||
        call->arguments.front()->literal_kind() != ast::LiteralKind::string) {
        return std::nullopt;
    }
    return call->arguments.front()->value();
}

Type signature_expression_type(const ast::Expression& expression, const GodotApi& api) {
    const Type dynamic{TypeKind::variant, "Variant"};
    if (expression.get_if<ast::ArrayExpression>())
        return {TypeKind::array, "Array"};
    if (expression.get_if<ast::DictionaryExpression>())
        return {TypeKind::dictionary, "Dictionary"};
    if (const auto* literal = expression.get_if<ast::LiteralExpression>()) {
        switch (literal->kind) {
        case ast::LiteralKind::nil:
            return {TypeKind::nil, "null"};
        case ast::LiteralKind::boolean:
            return {TypeKind::boolean, "bool"};
        case ast::LiteralKind::integer:
            return {TypeKind::integer, "int"};
        case ast::LiteralKind::floating:
            return {TypeKind::floating, "float"};
        case ast::LiteralKind::string:
            return {TypeKind::string, "String"};
        case ast::LiteralKind::string_name:
            return {TypeKind::string_name, "StringName"};
        case ast::LiteralKind::node_path:
            return {TypeKind::builtin, "NodePath"};
        case ast::LiteralKind::none:
            return dynamic;
        }
    }
    if (const auto* identifier = expression.get_if<ast::IdentifierExpression>();
        identifier && (identifier->name == "PI" || identifier->name == "TAU" ||
                       identifier->name == "INF" || identifier->name == "NAN")) {
        return {TypeKind::floating, "float"};
    }
    if (const auto* unary = expression.get_if<ast::UnaryExpression>()) {
        if (unary->operation == "not")
            return {TypeKind::boolean, "bool"};
        if (unary->operation == "~")
            return {TypeKind::integer, "int"};
        return signature_expression_type(*unary->operand, api);
    }
    if (const auto* binary = expression.get_if<ast::BinaryExpression>()) {
        const auto left = signature_expression_type(*binary->left, api);
        const auto right = signature_expression_type(*binary->right, api);
        static const std::set<std::string_view> comparisons{
            "==", "!=", "<", ">", "<=", ">=", "in", "not in", "is", "is not", "and", "or"};
        if (comparisons.find(binary->operation) != comparisons.end())
            return {TypeKind::boolean, "bool"};
        static const std::set<std::string_view> integer_operations{"<<", ">>", "&", "|", "^", "%"};
        if (integer_operations.find(binary->operation) != integer_operations.end() &&
            left.kind == TypeKind::integer && right.kind == TypeKind::integer) {
            return {TypeKind::integer, "int"};
        }
        if (binary->operation == "+" && left.kind == TypeKind::string && right == left)
            return left;
        if (left.is_numeric() && right.is_numeric()) {
            if (left.kind == TypeKind::floating || right.kind == TypeKind::floating) {
                return {TypeKind::floating, "float"};
            }
            return {TypeKind::integer, "int"};
        }
        return dynamic;
    }
    if (const auto* conditional = expression.get_if<ast::ConditionalExpression>()) {
        const auto when_true = signature_expression_type(*conditional->when_true, api);
        const auto when_false = signature_expression_type(*conditional->when_false, api);
        if (when_true == when_false)
            return when_true;
        if (when_true.is_numeric() && when_false.is_numeric())
            return {TypeKind::floating, "float"};
    }
    if (const auto* call = expression.get_if<ast::CallExpression>()) {
        const auto* callee = call->callee->get_if<ast::IdentifierExpression>();
        if (!callee)
            return dynamic;
        const auto constructed = type_from_annotation(callee->name);
        if (constructed.kind == TypeKind::builtin || api.find_class(callee->name)) {
            return constructed;
        }
    }
    return dynamic;
}

Type signature_type(const std::optional<std::string>& annotation,
                    const ast::Expression* initializer, const GodotApi& api) {
    if (annotation) {
        if (api.has_global_enum(*annotation))
            return {TypeKind::enumeration, *annotation};
        if (const auto separator = annotation->rfind('.');
            separator != std::string::npos &&
            api.has_class_enum(annotation->substr(0, separator),
                               annotation->substr(separator + 1))) {
            return {TypeKind::enumeration, *annotation};
        }
        return type_from_annotation(*annotation);
    }
    if (initializer)
        return signature_expression_type(*initializer, api);
    return {TypeKind::variant, "Variant"};
}

bool expression_contains_await(const ast::Expression& expression) {
    if (expression.kind() == ast::ExpressionKind::await_expression)
        return true;
    for (std::size_t index = 0; index < expression.operand_count(); ++index) {
        if (expression_contains_await(*expression.operand(index)))
            return true;
    }
    return false;
}

bool statements_contain_await(const std::vector<ast::Statement>& statements) {
    for (const auto& statement : statements) {
        if ((statement.expression() && expression_contains_await(*statement.expression())) ||
            (statement.condition() && expression_contains_await(*statement.condition())) ||
            statements_contain_await(statement.body()) ||
            statements_contain_await(statement.else_body())) {
            return true;
        }
        for (const auto& branch : statement.match_branches()) {
            if ((branch.guard && expression_contains_await(*branch.guard)) ||
                statements_contain_await(branch.body)) {
                return true;
            }
        }
    }
    return false;
}

bool function_contains_await(const ast::FunctionDeclaration& function) {
    for (const auto& parameter : function.parameters) {
        if (parameter.default_value && expression_contains_await(*parameter.default_value))
            return true;
    }
    if (function.rest_parameter && function.rest_parameter->default_value &&
        expression_contains_await(*function.rest_parameter->default_value)) {
        return true;
    }
    return statements_contain_await(function.body);
}

bool accessor_contains_await(const std::optional<ast::PropertyAccessor>& accessor,
                             const std::vector<ast::FunctionDeclaration>& functions) {
    if (!accessor)
        return false;
    if (accessor->method.empty())
        return statements_contain_await(accessor->body);
    const auto method = std::find_if(functions.begin(), functions.end(), [&](const auto& function) {
        return function.name == accessor->method;
    });
    return method != functions.end() && method->name != "_init" && method->name != "_static_init" &&
           function_contains_await(*method);
}

ScriptInnerClassSymbol inner_class_symbol(const ast::ClassDeclaration& declaration,
                                          const GodotApi& api) {
    ScriptInnerClassSymbol symbol;
    symbol.name = declaration.name;
    symbol.godot_base_type = declaration.base_type.value_or("RefCounted");
    symbol.base_class_name = declaration.base_type.value_or("");
    symbol.is_abstract = std::any_of(
        declaration.annotations.begin(), declaration.annotations.end(),
        [](const ast::Annotation& annotation) { return annotation.name == "abstract"; });
    for (const auto& variable : declaration.variables) {
        ScriptMemberSymbol member;
        member.kind = variable.is_constant ? ScriptMemberKind::constant : ScriptMemberKind::field;
        member.name = variable.name;
        member.type = signature_type(
            variable.type,
            variable.infer_type || variable.is_constant ? variable.initializer.get() : nullptr,
            api);
        member.is_static = variable.is_constant || variable.is_static;
        member.has_accessor = variable.getter.has_value() || variable.setter.has_value();
        if (variable.getter)
            member.getter_method = variable.getter->method;
        if (variable.setter)
            member.setter_method = variable.setter->method;
        member.getter_is_coroutine =
            accessor_contains_await(variable.getter, declaration.functions);
        member.setter_is_coroutine =
            accessor_contains_await(variable.setter, declaration.functions);
        symbol.members.push_back(std::move(member));
    }
    for (const auto& function : declaration.functions) {
        ScriptMemberSymbol member;
        member.kind = ScriptMemberKind::function;
        member.name = function.name;
        member.type = function.name == "_init" ? Type{TypeKind::void_type, "void"}
                                               : signature_type(function.return_type, nullptr, api);
        member.is_static = function.is_static;
        member.is_vararg = function.rest_parameter.has_value();
        member.is_coroutine = function.name != "_static_init" && function_contains_await(function);
        member.is_abstract = function.is_abstract;
        member.has_explicit_type = function.name == "_init" || function.return_type.has_value();
        for (const auto& parameter : function.parameters) {
            member.parameters.push_back(signature_type(
                parameter.type, parameter.infer_type ? parameter.default_value.get() : nullptr,
                api));
            member.explicit_parameter_types.push_back(parameter.type.has_value() ||
                                                      parameter.infer_type);
            member.default_parameters.push_back(parameter.default_value != nullptr);
            if (!parameter.default_value)
                ++member.required_arguments;
        }
        symbol.members.push_back(std::move(member));
    }
    for (const auto& signal : declaration.signals) {
        ScriptMemberSymbol member;
        member.kind = ScriptMemberKind::signal;
        member.name = signal.name;
        member.type = {TypeKind::builtin, "Signal"};
        for (const auto& parameter : signal.parameters) {
            member.parameters.push_back(signature_type(parameter.type, nullptr, api));
            ++member.required_arguments;
        }
        symbol.members.push_back(std::move(member));
    }
    for (const auto& declaration_enum : declaration.enums) {
        ScriptEnumSymbol enumeration;
        enumeration.name = declaration_enum.name.value_or("");
        std::unordered_map<std::string, std::int64_t> previous;
        std::int64_t next_value = 0;
        for (const auto& entry : declaration_enum.entries) {
            const auto value = entry.value ? evaluate_integer_constant(*entry.value, previous)
                                           : std::optional<std::int64_t>{next_value};
            const auto resolved = value.value_or(0);
            previous.emplace(entry.name, resolved);
            enumeration.entries.push_back({entry.name, resolved});
            if (!declaration_enum.name) {
                ScriptMemberSymbol member;
                member.kind = ScriptMemberKind::enum_value;
                member.name = entry.name;
                member.type = {TypeKind::integer, "int"};
                member.is_static = true;
                symbol.members.push_back(std::move(member));
            }
            if (resolved != std::numeric_limits<std::int64_t>::max())
                next_value = resolved + 1;
        }
        if (declaration_enum.name)
            symbol.enums.push_back(std::move(enumeration));
    }
    return symbol;
}

std::string native_inner_suffix(std::string_view qualified_name) {
    std::string result;
    result.reserve(qualified_name.size() + 8);
    for (const char character : qualified_name) {
        if (character == '.')
            result += "__";
        else
            result.push_back(character);
    }
    return result;
}

bool managed_translation_unit_name(std::string_view name) {
    if (name.empty() || name.find_first_of("/\\") != std::string_view::npos)
        return false;
    return (name.size() > 7U && name.substr(name.size() - 7U) == ".gd.hpp") ||
           (name.size() > 7U && name.substr(name.size() - 7U) == ".gd.cpp");
}

bool managed_symbol_map_name(std::string_view name) {
    constexpr std::string_view suffix{".gd.symbols"};
    return name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix &&
           name.find_first_of("/\\") == std::string_view::npos;
}

struct IconPathResolution {
    std::optional<std::string> resource_path;
    std::string error;
};

std::optional<std::string> script_icon_literal(const ast::Script& script) {
    const auto icon =
        std::find_if(script.annotations.begin(), script.annotations.end(),
                     [](const ast::Annotation& annotation) { return annotation.name == "icon"; });
    if (icon == script.annotations.end() || icon->arguments.size() != 1 ||
        icon->arguments.front()->literal_kind() != ast::LiteralKind::string) {
        return std::nullopt;
    }
    return icon->arguments.front()->value();
}

bool path_escapes_project(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return true;
    return std::any_of(path.begin(), path.end(),
                       [](const auto& component) { return component == ".."; });
}

IconPathResolution resolve_script_icon_path(const std::string_view source_path,
                                            const std::string_view icon_path) {
    if (icon_path.empty())
        return {{}, "@icon path cannot be empty"};
    if (std::any_of(icon_path.begin(), icon_path.end(), [](const char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 0x20U || character == '\\';
        })) {
        return {{}, "@icon path must use a single-line Godot resource path with '/' separators"};
    }
    if (icon_path.rfind("uid://", 0) == 0)
        return {std::string{icon_path}, {}};

    std::filesystem::path relative_icon;
    if (icon_path.rfind("res://", 0) == 0) {
        relative_icon = path_from_utf8(icon_path.substr(6));
    } else {
        if (icon_path.find("://") != std::string_view::npos || icon_path.front() == '/') {
            return {{}, "@icon path must be relative, res://, or uid://"};
        }
        auto owner = std::string{source_path};
        if (const auto embedded = owner.find("::"); embedded != std::string::npos)
            owner.erase(embedded);
        relative_icon = path_from_utf8(owner).parent_path() / path_from_utf8(icon_path);
    }
    relative_icon = relative_icon.lexically_normal();
    if (path_escapes_project(relative_icon))
        return {{}, "@icon path escapes the project resource root"};
    return {"res://" + generic_path_to_utf8(relative_icon), {}};
}

Manifest read_manifest(const std::filesystem::path& path) {
    Manifest manifest;
    std::ifstream input{path};
    std::string magic;
    std::string version;
    if (!(input >> magic >> version) || magic != "GDPP_OUTPUT_MANIFEST" || version != "1")
        return manifest;
    std::string source_path;
    ManifestEntry entry;
    while (input >> std::quoted(source_path) >> std::quoted(entry.header) >>
           std::quoted(entry.source) >> std::quoted(entry.symbols)) {
        if (!managed_translation_unit_name(entry.header) ||
            !managed_translation_unit_name(entry.source) ||
            !managed_symbol_map_name(entry.symbols)) {
            return {};
        }
        manifest.emplace(source_path, entry);
    }
    return manifest;
}

std::string write_manifest(const Manifest& manifest) {
    std::ostringstream output;
    output << "GDPP_OUTPUT_MANIFEST 1\n";
    for (const auto& [path, entry] : manifest) {
        output << std::quoted(path) << ' ' << std::quoted(entry.header) << ' '
               << std::quoted(entry.source) << ' ' << std::quoted(entry.symbols) << '\n';
    }
    return output.str();
}

std::optional<std::filesystem::path>
containing_build_directory(const std::filesystem::path& root,
                           const std::filesystem::path& relative_output) {
    auto current = root;
    for (const auto& component : relative_output) {
        current /= component;
        if (component == "build")
            return current;
    }
    return std::nullopt;
}

std::string generated_registration(const std::vector<CompiledProjectScript>& scripts) {
    std::ostringstream output;
    const bool has_attached_scripts = std::any_of(
        scripts.begin(), scripts.end(), [](const auto& script) { return script.is_attached; });
    const bool has_editor_only_classes =
        std::any_of(scripts.begin(), scripts.end(), [](const auto& script) {
            return script.is_editor_only || !script.editor_only_inner_class_names.empty();
        });
    const auto emit_registration = [&](const std::string& class_name, const bool is_abstract,
                                       const bool is_editor_only) {
        output << (is_editor_only ? "    if (gdpp_editor_environment) {\n        " : "    ");
        output << (is_abstract ? "GDREGISTER_ABSTRACT_CLASS(" : "GDREGISTER_CLASS(") << class_name
               << ");\n";
        if (is_editor_only)
            output << "    }\n";
    };
    output << "// Generated by GDPP. Do not edit.\n";
    for (const auto& script : scripts)
        output << "#include \"" << script.header_file_name << "\"\n";
    output << "\n#include <gdextension_interface.h>\n"
           << "#include <godot_cpp/classes/engine.hpp>\n"
           << "#include <godot_cpp/core/class_db.hpp>\n"
           << "#include <godot_cpp/core/defs.hpp>\n"
           << "#include <godot_cpp/core/error_macros.hpp>\n"
           << "#include <godot_cpp/godot.hpp>\n\n"
           << "#include <gdpp/runtime/variant_ops.hpp>\n"
           << (has_attached_scripts ? "#include <gdpp/runtime/attached_script.hpp>\n" : "") << "\n"
           << "namespace {\n"
           << "void initialize_gdpp_project(godot::ModuleInitializationLevel level) {\n"
           << "    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) return;\n"
           << "    GDREGISTER_CLASS(gdpp::runtime::CoroutineFunctionState);\n"
           << "    gdpp::runtime::initialize_coroutine_runtime();\n";
    if (has_editor_only_classes) {
        output << "    auto* gdpp_engine = godot::Engine::get_singleton();\n"
               << "    ERR_FAIL_NULL_MSG(gdpp_engine, "
                  "\"Godot Engine singleton is unavailable during GDPP registration\");\n"
               << "    const bool gdpp_editor_environment = gdpp_engine->is_editor_hint();\n";
    }
    if (has_attached_scripts) {
        // Export runs in the editor and must invoke ScriptExtension virtual callbacks even for
        // non-tool customer scripts. Keep the bridge infrastructure editor-visible; generated
        // behavior classes retain their own tool/runtime registration policy below.
        output << "    GDREGISTER_CLASS(gdpp::runtime::AttachedScriptBehavior);\n"
               << "    GDREGISTER_CLASS(gdpp::runtime::AttachedCompiledLanguage);\n"
               << "    GDREGISTER_CLASS(gdpp::runtime::AttachedCompiledScript);\n"
               << "    GDREGISTER_CLASS(gdpp::runtime::AttachedScriptResourceLoader);\n";
    }
    for (const auto& script : scripts) {
        for (const auto& inner_class_name : script.inner_class_names) {
            const bool is_abstract =
                std::find(script.abstract_inner_class_names.begin(),
                          script.abstract_inner_class_names.end(),
                          inner_class_name) != script.abstract_inner_class_names.end();
            const bool editor_only =
                std::find(script.editor_only_inner_class_names.begin(),
                          script.editor_only_inner_class_names.end(),
                          inner_class_name) != script.editor_only_inner_class_names.end();
            emit_registration(inner_class_name, is_abstract, editor_only);
        }
        emit_registration(script.class_name, script.is_abstract, script.is_editor_only);
    }
    if (has_attached_scripts) {
        for (const auto& script : scripts) {
            if (!script.is_attached)
                continue;
            const std::string indent = script.is_editor_only ? "        " : "    ";
            if (script.is_editor_only)
                output << "    if (gdpp_editor_environment) {\n";
            for (const auto& inner_class_name : script.inner_class_names) {
                const bool inner_editor_only =
                    std::find(script.editor_only_inner_class_names.begin(),
                              script.editor_only_inner_class_names.end(),
                              inner_class_name) != script.editor_only_inner_class_names.end();
                if (inner_editor_only)
                    output << indent << "if (gdpp_editor_environment) {\n";
                const auto inner_indent = inner_editor_only ? indent + "    " : indent;
                output << inner_indent << "{\n"
                       << inner_indent << "    godot::String error;\n"
                       << inner_indent
                       << "    ERR_FAIL_COND_MSG(!gdpp::runtime::register_attached_script("
                       << inner_class_name << "::_gdpp_descriptor(), &error), error);\n"
                       << inner_indent << "}\n";
                if (inner_editor_only)
                    output << indent << "}\n";
            }
            output << indent << "{\n"
                   << indent << "    godot::String error;\n"
                   << indent << "    ERR_FAIL_COND_MSG(!gdpp::runtime::register_attached_script("
                   << script.class_name << "::_gdpp_descriptor(), &error), error);\n"
                   << indent << "}\n";
            if (script.is_editor_only)
                output << "    }\n";
        }
        output << "    {\n"
               << "        godot::String error;\n"
               << "        ERR_FAIL_COND_MSG(!gdpp::runtime::AttachedCompiledLanguage::"
                  "register_singleton(&error), error);\n"
               << "        ERR_FAIL_COND_MSG(!gdpp::runtime::"
                  "register_attached_script_resource_loader(&error), error);\n"
               << "    }\n";
    }
    output << "}\n"
           << "void uninitialize_gdpp_project(godot::ModuleInitializationLevel level) {\n"
           << "    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) return;\n"
           << "    gdpp::runtime::shutdown_coroutine_runtime();\n";
    // Generated preload caches and script static fields can own Godot resources
    // for the lifetime of the project extension. Release them while the
    // scene-level servers are still alive; C++ static destructors run too late
    // during shutdown.
    for (auto script = scripts.rbegin(); script != scripts.rend(); ++script) {
        output << "    " << script->class_name << "::_gdpp_release_preloaded_resources();\n";
        for (auto inner = script->inner_class_names.rbegin();
             inner != script->inner_class_names.rend(); ++inner) {
            output << "    " << *inner << "::_gdpp_release_preloaded_resources();\n";
        }
    }
    if (has_attached_scripts) {
        output << "    gdpp::runtime::detach_all_attached_script_instances();\n"
               << "    gdpp::runtime::unregister_attached_script_resource_loader();\n"
               << "    gdpp::runtime::AttachedCompiledLanguage::unregister_singleton();\n"
               << "    gdpp::runtime::unregister_all_attached_scripts();\n";
    }
    output << "    gdpp::runtime::shutdown_coroutine_runtime();\n"
           << "    gdpp::runtime::release_engine_lifetime_statics();\n"
           << "}\n"
           << "} // namespace\n\n"
           << "extern \"C\" GDExtensionBool GDE_EXPORT\n"
           << project_library_entry_symbol << "(GDExtensionInterfaceGetProcAddress address,\n"
           << "                            GDExtensionClassLibraryPtr library,\n"
           << "                            GDExtensionInitialization* initialization) {\n"
           << "    godot::GDExtensionBinding::InitObject init{address, library, initialization};\n"
           << "    init.register_initializer(initialize_gdpp_project);\n"
           << "    init.register_terminator(uninitialize_gdpp_project);\n"
           << "    init.set_minimum_library_initialization_level(\n"
           << "        godot::MODULE_INITIALIZATION_LEVEL_SCENE);\n"
           << "    return init.init();\n"
           << "}\n";
    return output.str();
}

Diagnostic project_error(std::string code, std::string message) {
    return {DiagnosticSeverity::error, std::move(code), std::move(message), {}};
}

bool has_project_errors(const std::vector<ProjectDiagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.diagnostic.severity == DiagnosticSeverity::error;
    });
}

void report_project_progress(const ProjectCompileOptions& options, const ProjectCompilePhase phase,
                             const std::size_t completed, const std::size_t total) {
    if (options.progress_callback)
        options.progress_callback(phase, completed, total);
}

} // namespace

ProjectCompileResult ProjectCompiler::compile(const ProjectCompileOptions& options) const {
    ProjectCompileResult result;
    run_on_compiler_stack([&]() { result = compile_impl(options); });
    return result;
}

ProjectCompileResult ProjectCompiler::compile_impl(const ProjectCompileOptions& options) const {
    ProjectCompileResult result;
    std::error_code error;
    const auto root = std::filesystem::absolute(options.project_root, error).lexically_normal();
    if (error || !std::filesystem::is_directory(root)) {
        result.diagnostics.push_back(
            {options.project_root, project_error("PRJ0001", "project root is not a directory")});
        return result;
    }
    const auto output =
        std::filesystem::absolute(options.output_directory, error).lexically_normal();
    if (error || !inside_root(root, output)) {
        result.diagnostics.push_back(
            {options.output_directory,
             project_error("PRJ0002", "project output must be inside the project root")});
        return result;
    }
    const auto native_library_directory =
        options.native_library_directory.empty()
            ? (root / "addons/gdpp/binary")
            : std::filesystem::absolute(options.native_library_directory, error).lexically_normal();
    if (error || !inside_root(root, native_library_directory)) {
        result.diagnostics.push_back(
            {options.native_library_directory,
             project_error("PRJ0010", "native library output must be inside the project root")});
        return result;
    }

    const auto generated = output / "generated";
    const auto manifest_path = output / "manifest.txt";
    const auto old_manifest = read_manifest(manifest_path);
    report_project_progress(options, ProjectCompilePhase::scan, 0, 1);
    std::vector<std::filesystem::path> source_paths;
    std::vector<std::filesystem::path> text_resource_paths;
    std::vector<std::filesystem::path> uid_sidecar_paths;
    std::vector<std::filesystem::path> import_sidecar_paths;
    std::vector<std::filesystem::path> external_extension_descriptors;
    std::vector<std::filesystem::path> extension_bridge_manifests;
    const project::ProjectFileSelector file_selector{root, output};
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::skip_permission_denied, error};
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto relative = iterator->path().lexically_relative(root);
        if (iterator->is_directory() && !file_selector.should_descend(relative)) {
            iterator.disable_recursion_pending();
        } else if (iterator->is_regular_file() && iterator->path().extension() == ".gd" &&
                   file_selector.should_compile(relative)) {
            source_paths.push_back(iterator->path());
        } else if (iterator->is_regular_file() &&
                   (iterator->path().extension() == ".tscn" ||
                    iterator->path().extension() == ".tres") &&
                   file_selector.should_compile(relative)) {
            text_resource_paths.push_back(iterator->path());
        } else if (iterator->is_regular_file() && iterator->path().extension() == ".gdextension" &&
                   file_selector.should_compile(relative)) {
            external_extension_descriptors.push_back(iterator->path());
        } else if (iterator->is_regular_file() &&
                   iterator->path().filename() == "gdpp_bridge.json" &&
                   file_selector.should_compile(relative)) {
            extension_bridge_manifests.push_back(iterator->path());
        } else if (iterator->is_regular_file() && iterator->path().extension() == ".uid" &&
                   file_selector.should_compile(relative)) {
            uid_sidecar_paths.push_back(iterator->path());
        } else if (iterator->is_regular_file() && iterator->path().extension() == ".import" &&
                   file_selector.should_compile(relative)) {
            import_sidecar_paths.push_back(iterator->path());
        }
        iterator.increment(error);
    }
    if (error) {
        result.diagnostics.push_back(
            {root, project_error("PRJ0003", "failed while scanning project scripts")});
        return result;
    }
    std::sort(source_paths.begin(), source_paths.end());
    std::sort(text_resource_paths.begin(), text_resource_paths.end());
    std::sort(uid_sidecar_paths.begin(), uid_sidecar_paths.end());
    std::sort(import_sidecar_paths.begin(), import_sidecar_paths.end());
    std::sort(external_extension_descriptors.begin(), external_extension_descriptors.end());
    std::sort(extension_bridge_manifests.begin(), extension_bridge_manifests.end());

    auto bridge_load =
        load_extension_bridges(root, extension_bridge_manifests, options.compiler.target_version);
    for (const auto& diagnostic : bridge_load.diagnostics) {
        result.diagnostics.push_back(
            {root, project_error("PRJ0020", "invalid third-party bridge: " + diagnostic)});
    }
    if (has_project_errors(result.diagnostics))
        return result;
    // A checked-in contract is an explicit, reviewable override for offline/cross-machine builds.
    // Live ClassDB reflection fills only classes that are not already described by one of those
    // manifests. Keeping each reflected class in its own bridge also gives the generated Ninja
    // graph class-granular invalidation when a third-party API changes.
    std::set<std::string> declared_bridge_classes;
    for (const auto& bridge : bridge_load.bridges) {
        for (const auto& type : bridge.classes)
            declared_bridge_classes.insert(type.gdscript_name);
    }
    for (const auto& reflected : options.reflected_extension_bridges) {
        ExtensionBridge filtered = reflected;
        filtered.classes.erase(std::remove_if(filtered.classes.begin(), filtered.classes.end(),
                                              [&](const auto& type) {
                                                  return declared_bridge_classes.find(
                                                             type.gdscript_name) !=
                                                         declared_bridge_classes.end();
                                              }),
                               filtered.classes.end());
        for (const auto& type : filtered.classes)
            declared_bridge_classes.insert(type.gdscript_name);
        if (!filtered.classes.empty())
            bridge_load.bridges.push_back(std::move(filtered));
    }
    struct BridgeClassReference {
        const ExtensionBridge* bridge{nullptr};
        const ExtensionBridgeClass* type{nullptr};
    };
    std::map<std::string, BridgeClassReference> bridge_classes;
    for (const auto& bridge : bridge_load.bridges) {
        for (const auto& type : bridge.classes)
            bridge_classes.emplace(type.gdscript_name, BridgeClassReference{&bridge, &type});
    }
    const auto find_bridge_enum =
        [&](const std::string& qualified_name) -> const ExtensionBridgeEnum* {
        const auto separator = qualified_name.find('.');
        if (separator == std::string::npos ||
            qualified_name.find('.', separator + 1) != std::string::npos) {
            return nullptr;
        }
        const auto owner = bridge_classes.find(qualified_name.substr(0, separator));
        if (owner == bridge_classes.end())
            return nullptr;
        const auto enum_name = qualified_name.substr(separator + 1);
        const auto found =
            std::find_if(owner->second.type->enums.begin(), owner->second.type->enums.end(),
                         [&](const auto& enumeration) { return enumeration.name == enum_name; });
        return found == owner->second.type->enums.end() ? nullptr : &*found;
    };
    const auto bridge_contract_identity = [](const ExtensionBridge& bridge) {
        return bridge.abi + ":" + bridge.contract_hash;
    };

    struct SourceInput {
        std::filesystem::path path;
        std::string relative;
        std::string source;
        std::string source_hash;
        std::string public_abi_hash;
        std::string implementation_hash;
        ast::Script script;
        std::vector<std::string> dependencies;
        std::vector<std::string> extension_abis;
        std::string script_class_name;
        std::string native_class_stem;
        std::string base_reference{"RefCounted"};
        std::string semantic_base_type{"RefCounted"};
        std::optional<std::size_t> script_base;
        std::optional<std::size_t> local_inner_base;
        BridgeClassReference extension_base;
        std::string external_base_name;
        std::string attached_native_base{"RefCounted"};
        bool attached{false};
        bool globally_named{false};
        std::string autoload_name;
        std::vector<ScriptMemberSymbol> members;
        std::vector<ScriptEnumSymbol> enums;
        std::vector<ScriptInnerClassSymbol> inner_classes;
        std::optional<std::string> icon_path;
        bool is_abstract{false};
        bool static_unload{false};
    };
    std::vector<SourceInput> inputs;
    inputs.reserve(source_paths.size());
    std::map<std::string, std::string> resource_aliases;
    const auto add_resource_alias = [&](const std::filesystem::path& source_path, std::string uid,
                                        std::string project_path) {
        const auto [existing, inserted] = resource_aliases.emplace(uid, project_path);
        if (!inserted && existing->second != project_path) {
            result.diagnostics.push_back(
                {source_path,
                 project_error("PRJ0019", "resource UID '" + existing->first +
                                              "' resolves to both '" + existing->second +
                                              "' and '" + project_path + "'")});
        }
    };
    const auto& target_api = GodotApi::for_version(options.compiler.target_version);
    const auto append_source = [&](const std::filesystem::path& source_path, std::string relative,
                                   std::string source) {
        auto source_hash =
            sha256(std::string{GDPP_VERSION_STRING} + ":codegen:" + GDPP_CODEGEN_FINGERPRINT +
                   ":api:" + std::string{target_api.version()} +
                   (options.compiler.optimize ? ":opt:" : ":no-opt:") + source);
        SourceInput input;
        input.path = source_path;
        input.relative = std::move(relative);
        input.source = std::move(source);
        input.source_hash = std::move(source_hash);
        inputs.push_back(std::move(input));
    };
    for (const auto& source_path : source_paths) {
        const auto source = read_file(source_path);
        if (!source) {
            result.diagnostics.push_back(
                {source_path, project_error("PRJ0004", "cannot read script")});
            continue;
        }
        append_source(source_path, generic_path_to_utf8(source_path.lexically_relative(root)),
                      *source);
    }
    for (const auto& sidecar_path : uid_sidecar_paths) {
        const auto sidecar = read_file(sidecar_path);
        if (!sidecar)
            continue;
        const auto uid = uid_sidecar_value(*sidecar);
        if (!uid)
            continue;
        auto resource_path = sidecar_path;
        resource_path.replace_extension();
        add_resource_alias(sidecar_path, *uid,
                           generic_path_to_utf8(resource_path.lexically_relative(root)));
    }
    for (const auto& sidecar_path : import_sidecar_paths) {
        const auto sidecar = read_file(sidecar_path);
        if (!sidecar)
            continue;
        const auto uid = imported_resource_uid(*sidecar);
        if (!uid)
            continue;
        auto resource_path = sidecar_path;
        resource_path.replace_extension();
        add_resource_alias(sidecar_path, *uid,
                           generic_path_to_utf8(resource_path.lexically_relative(root)));
    }
    for (const auto& resource_path : text_resource_paths) {
        const auto resource = read_file(resource_path);
        if (!resource) {
            result.diagnostics.push_back(
                {resource_path,
                 project_error("PRJ0016", "cannot read text resource for embedded scripts")});
            continue;
        }
        const auto relative_resource = generic_path_to_utf8(resource_path.lexically_relative(root));
        if (const auto uid = text_resource_uid(*resource))
            add_resource_alias(resource_path, *uid, relative_resource);
        for (auto& [uid, path] : text_resource_references(*resource))
            add_resource_alias(resource_path, std::move(uid), std::move(path));
        auto embedded_scan = embedded_gdscript_sources(*resource);
        for (const auto& unresolved : embedded_scan.unresolved_ids) {
            result.diagnostics.push_back(
                {resource_path,
                 project_error("PRJ0017",
                               "cannot infer the native owner type for source-less embedded "
                               "GDScript '" +
                                   unresolved + "'")});
        }
        for (auto& embedded : embedded_scan.scripts) {
            append_source(resource_path, relative_resource + "::" + embedded.id,
                          std::move(embedded.source));
        }
    }
    const auto autoload_scan = read_project_autoloads(root / "project.godot", resource_aliases);
    for (const auto& [name, uid] : autoload_scan.unresolved_uids) {
        result.diagnostics.push_back(
            {root / "project.godot",
             project_error("PRJ0030", "autoload '" + name +
                                          "' references unresolved resource UID '" + uid + "'")});
    }
    if (has_project_errors(result.diagnostics))
        return result;
    const auto& project_autoloads = autoload_scan.scripts;
    std::sort(inputs.begin(), inputs.end(), [](const SourceInput& left, const SourceInput& right) {
        return left.relative < right.relative;
    });
    report_project_progress(options, ProjectCompilePhase::scan, 1, 1);

    // Parse every script before code generation so project-wide class names and inheritance form
    // one deterministic graph. The regular compiler reparses each unit later and remains the
    // authority for full language diagnostics.
    std::unordered_map<std::string, std::size_t> script_classes;
    std::unordered_map<std::string, std::size_t> autoload_classes;
    std::unordered_map<std::string, std::size_t> native_class_names;
    std::unordered_map<std::string, std::size_t> script_paths;
    const auto script_progress_total = std::max<std::size_t>(inputs.size(), 1);
    report_project_progress(options, ProjectCompilePhase::parse, 0, script_progress_total);
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        auto& input = inputs[index];
        const SourceFile source{input.relative, input.source};
        DiagnosticBag diagnostics{options.compiler.frontend_limits.max_diagnostics};
        Lexer lexer{source, diagnostics, options.compiler.frontend_limits};
        const auto tokens = lexer.scan();
        Parser parser{tokens, diagnostics, options.compiler.frontend_limits};
        input.script = parser.parse_script();
        const auto& script = input.script;
        for (const auto& diagnostic : diagnostics.items())
            result.diagnostics.push_back({input.path, diagnostic});
        input.script_class_name =
            script.class_name.value_or(to_pascal_case(path_to_utf8(input.path.stem())));
        input.globally_named = script.class_name.has_value();
        input.is_abstract = std::any_of(
            script.annotations.begin(), script.annotations.end(),
            [](const ast::Annotation& annotation) { return annotation.name == "abstract"; });
        input.static_unload = std::any_of(
            script.annotations.begin(), script.annotations.end(),
            [](const ast::Annotation& annotation) { return annotation.name == "static_unload"; });
        if (const auto icon = script_icon_literal(script)) {
            auto resolved = resolve_script_icon_path(input.relative, *icon);
            if (!resolved.resource_path) {
                result.diagnostics.push_back(
                    {input.path, project_error("PRJ0027", std::move(resolved.error))});
            } else {
                input.icon_path = std::move(resolved.resource_path);
            }
        }
        input.native_class_stem =
            native_class_stem(input.relative, input.script_class_name, input.globally_named);
        if (const auto autoload = project_autoloads.find(input.relative);
            autoload != project_autoloads.end()) {
            input.autoload_name = autoload->second;
        }
        input.base_reference = script.base_type.value_or("RefCounted");
        for (const auto& variable : script.variables) {
            ScriptMemberSymbol member;
            member.kind =
                variable.is_constant ? ScriptMemberKind::constant : ScriptMemberKind::field;
            member.name = variable.name;
            member.type = signature_type(
                variable.type,
                variable.infer_type || variable.is_constant ? variable.initializer.get() : nullptr,
                target_api);
            member.is_static = variable.is_constant || variable.is_static;
            member.has_accessor = variable.getter.has_value() || variable.setter.has_value();
            if (variable.getter)
                member.getter_method = variable.getter->method;
            if (variable.setter)
                member.setter_method = variable.setter->method;
            member.getter_is_coroutine = accessor_contains_await(variable.getter, script.functions);
            member.setter_is_coroutine = accessor_contains_await(variable.setter, script.functions);
            for (const auto& annotation : variable.annotations) {
                if (annotation.name == "export_storage") {
                    member.property_storage = true;
                } else if (annotation.name == "export_tool_button") {
                    member.property_editor = true;
                } else if (annotation.name.rfind("export", 0) == 0) {
                    member.property_storage = true;
                    member.property_editor = true;
                }
            }
            input.members.push_back(std::move(member));
        }
        for (const auto& function : script.functions) {
            ScriptMemberSymbol member;
            member.kind = ScriptMemberKind::function;
            member.name = function.name;
            member.type = function.name == "_init"
                              ? Type{TypeKind::void_type, "void"}
                              : signature_type(function.return_type, nullptr, target_api);
            member.is_static = function.is_static;
            member.is_vararg = function.rest_parameter.has_value();
            member.is_coroutine =
                function.name != "_static_init" && function_contains_await(function);
            member.is_abstract = function.is_abstract;
            member.has_explicit_type = function.name == "_init" || function.return_type.has_value();
            for (const auto& parameter : function.parameters) {
                member.parameter_names.push_back(parameter.name);
                member.parameters.push_back(signature_type(
                    parameter.type, parameter.infer_type ? parameter.default_value.get() : nullptr,
                    target_api));
                member.explicit_parameter_types.push_back(parameter.type.has_value() ||
                                                          parameter.infer_type);
                member.default_parameters.push_back(parameter.default_value != nullptr);
                if (!parameter.default_value)
                    ++member.required_arguments;
            }
            input.members.push_back(std::move(member));
        }
        for (const auto& signal : script.signals) {
            ScriptMemberSymbol member;
            member.kind = ScriptMemberKind::signal;
            member.name = signal.name;
            member.type = {TypeKind::builtin, "Signal"};
            for (const auto& parameter : signal.parameters) {
                member.parameter_names.push_back(parameter.name);
                member.parameters.push_back(signature_type(parameter.type, nullptr, target_api));
                ++member.required_arguments;
            }
            input.members.push_back(std::move(member));
        }
        for (const auto& declaration : script.enums) {
            ScriptEnumSymbol enumeration;
            enumeration.name = declaration.name.value_or("");
            std::unordered_map<std::string, std::int64_t> previous;
            std::int64_t next_value = 0;
            for (const auto& entry : declaration.entries) {
                const auto value = entry.value ? evaluate_integer_constant(*entry.value, previous)
                                               : std::optional<std::int64_t>{next_value};
                const auto resolved = value.value_or(0);
                previous.emplace(entry.name, resolved);
                enumeration.entries.push_back({entry.name, resolved});
                if (!declaration.name) {
                    ScriptMemberSymbol member;
                    member.kind = ScriptMemberKind::enum_value;
                    member.name = entry.name;
                    member.type = {TypeKind::integer, "int"};
                    member.is_static = true;
                    input.members.push_back(std::move(member));
                }
                if (resolved != std::numeric_limits<std::int64_t>::max())
                    next_value = resolved + 1;
            }
            if (declaration.name)
                input.enums.push_back(std::move(enumeration));
        }
        const auto collect_inner_classes = [&](auto&& self,
                                               const std::vector<ast::ClassDeclaration>& classes,
                                               const std::string& parent_name) -> void {
            for (const auto& declaration : classes) {
                auto symbol = inner_class_symbol(declaration, target_api);
                symbol.name =
                    parent_name.empty() ? declaration.name : parent_name + "." + declaration.name;
                input.inner_classes.push_back(std::move(symbol));
                self(self, declaration.classes,
                     parent_name.empty() ? declaration.name : parent_name + "." + declaration.name);
            }
        };
        collect_inner_classes(collect_inner_classes, script.classes, {});
        for (auto& inner : input.inner_classes) {
            if (target_api.find_class(inner.base_class_name)) {
                inner.godot_base_type = inner.base_class_name;
                inner.attached_native_base = inner.base_class_name;
                inner.base_class_name.clear();
            } else if (inner.base_class_name.empty()) {
                inner.godot_base_type = "RefCounted";
                inner.attached_native_base = "RefCounted";
            }
        }
        const auto resolve_inner_reference =
            [&](const std::string& owner_name,
                const std::string& reference) -> const ScriptInnerClassSymbol* {
            if (reference.empty())
                return nullptr;
            const auto exact =
                std::find_if(input.inner_classes.begin(), input.inner_classes.end(),
                             [&](const auto& candidate) { return candidate.name == reference; });
            if (reference.find('.') != std::string::npos)
                return exact == input.inner_classes.end() ? nullptr : &*exact;

            auto separator = owner_name.rfind('.');
            while (separator != std::string::npos) {
                const auto candidate_name = owner_name.substr(0, separator + 1) + reference;
                const auto candidate =
                    std::find_if(input.inner_classes.begin(), input.inner_classes.end(),
                                 [&](const auto& value) { return value.name == candidate_name; });
                if (candidate != input.inner_classes.end())
                    return &*candidate;
                if (separator == 0)
                    break;
                separator = owner_name.rfind('.', separator - 1);
            }
            if (exact != input.inner_classes.end())
                return &*exact;

            const ScriptInnerClassSymbol* unique = nullptr;
            for (const auto& candidate : input.inner_classes) {
                const auto leaf_separator = candidate.name.rfind('.');
                const auto leaf = leaf_separator == std::string::npos
                                      ? candidate.name
                                      : candidate.name.substr(leaf_separator + 1);
                if (leaf != reference)
                    continue;
                if (unique)
                    return nullptr;
                unique = &candidate;
            }
            return unique;
        };
        for (auto& inner : input.inner_classes) {
            if (inner.base_class_name.empty())
                continue;
            if (const auto* base = resolve_inner_reference(inner.name, inner.base_class_name))
                inner.base_class_name = base->name;
        }
        for (auto& inner : input.inner_classes) {
            std::unordered_set<std::string> visited{inner.name};
            auto* current = &inner;
            while (!current->base_class_name.empty()) {
                if (!visited.insert(current->base_class_name).second)
                    break;
                const auto base = std::find_if(
                    input.inner_classes.begin(), input.inner_classes.end(),
                    [&](const auto& value) { return value.name == current->base_class_name; });
                if (base == input.inner_classes.end())
                    break;
                current = &*base;
            }
            inner.godot_base_type = current->godot_base_type;
            inner.attached_native_base = current->attached_native_base;
        }
        if (input.globally_named && target_api.find_class(input.script_class_name)) {
            result.diagnostics.push_back(
                {input.path,
                 project_error("PRJ0012", "script class_name '" + input.script_class_name +
                                              "' collides with a Godot engine type")});
        }
        const auto [native_owner, unique_native_name] =
            native_class_names.emplace(input.native_class_stem, index);
        if (!unique_native_name) {
            result.diagnostics.push_back(
                {input.path,
                 project_error("PRJ0005", "native script class '" + input.native_class_stem +
                                              "' is also produced by " +
                                              inputs[native_owner->second].relative)});
        }
        if (input.globally_named) {
            const auto [owner, unique_script_name] =
                script_classes.emplace(input.script_class_name, index);
            if (!unique_script_name) {
                result.diagnostics.push_back(
                    {input.path,
                     project_error("PRJ0015", "global script class '" + input.script_class_name +
                                                  "' is also declared by " +
                                                  inputs[owner->second].relative)});
            }
        }
        if (!input.autoload_name.empty())
            autoload_classes.emplace(input.autoload_name, index);
        script_paths.emplace(input.relative, index);
        report_project_progress(options, ProjectCompilePhase::parse, index + 1,
                                script_progress_total);
    }
    if (inputs.empty())
        report_project_progress(options, ProjectCompilePhase::parse, 1, 1);

    // Resolve scalar class constants as one project graph before semantic analysis. GDScript
    // permits preload paths, enum initializers and annotation arguments to concatenate or
    // calculate constants through another globally named script. A bounded fixed point handles
    // forward references and cross-script dependency order without loading customer scripts in
    // the editor.
    std::vector<std::unordered_map<std::string, std::string>> local_string_constants(inputs.size());
    std::vector<std::unordered_map<std::string, std::int64_t>> local_integer_constants(
        inputs.size());
    std::unordered_map<std::string, std::string> project_string_constants;
    std::unordered_map<std::string, std::int64_t> project_integer_constants;
    std::size_t constant_resolution_budget = 1;
    for (const auto& input : inputs) {
        constant_resolution_budget += input.script.variables.size();
        for (const auto& enumeration : input.script.enums)
            constant_resolution_budget += enumeration.entries.size();
    }
    for (std::size_t iteration = 0; iteration < constant_resolution_budget; ++iteration) {
        bool changed = false;
        for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
            auto& input = inputs[input_index];
            auto string_environment = project_string_constants;
            auto integer_environment = project_integer_constants;
            for (const auto& [name, value] : local_string_constants[input_index])
                string_environment.insert_or_assign(name, value);
            for (const auto& [name, value] : local_integer_constants[input_index])
                integer_environment.insert_or_assign(name, value);

            const auto assign_changed = [&](auto& values, const std::string& name,
                                            const auto& value) {
                const auto existing = values.find(name);
                if (existing != values.end() && existing->second == value)
                    return false;
                values.insert_or_assign(name, value);
                return true;
            };
            const auto publish_string = [&](const std::string& name, const std::string& value) {
                changed |= assign_changed(local_string_constants[input_index], name, value);
                string_environment.insert_or_assign(name, value);
                if (input.globally_named) {
                    const auto qualified = input.script_class_name + "." + name;
                    changed |= assign_changed(project_string_constants, qualified, value);
                    string_environment.insert_or_assign(qualified, value);
                }
            };
            const auto publish_integer = [&](const std::string& name, const std::int64_t value) {
                changed |= assign_changed(local_integer_constants[input_index], name, value);
                integer_environment.insert_or_assign(name, value);
                if (input.globally_named) {
                    const auto qualified = input.script_class_name + "." + name;
                    changed |= assign_changed(project_integer_constants, qualified, value);
                    integer_environment.insert_or_assign(qualified, value);
                }
            };

            for (const auto& variable : input.script.variables) {
                if (!variable.is_constant || !variable.initializer)
                    continue;
                const auto member = std::find_if(
                    input.members.begin(), input.members.end(), [&](const auto& value) {
                        return value.kind == ScriptMemberKind::constant &&
                               value.name == variable.name;
                    });
                if (const auto value =
                        evaluate_string_constant(*variable.initializer, string_environment)) {
                    publish_string(variable.name, *value);
                    if (member != input.members.end())
                        member->folded_string_value = *value;
                }
                if (const auto value =
                        evaluate_integer_constant(*variable.initializer, integer_environment)) {
                    publish_integer(variable.name, *value);
                    if (member != input.members.end())
                        member->folded_integer_value = *value;
                }
            }

            for (const auto& declaration : input.script.enums) {
                auto previous = integer_environment;
                std::optional<std::int64_t> next_value{0};
                auto enumeration = declaration.name
                                       ? std::find_if(input.enums.begin(), input.enums.end(),
                                                      [&](const auto& value) {
                                                          return value.name == *declaration.name;
                                                      })
                                       : input.enums.end();
                for (std::size_t entry_index = 0; entry_index < declaration.entries.size();
                     ++entry_index) {
                    const auto& entry = declaration.entries[entry_index];
                    const auto value = entry.value
                                           ? evaluate_integer_constant(*entry.value, previous)
                                           : next_value;
                    if (!value) {
                        next_value.reset();
                        continue;
                    }
                    previous.insert_or_assign(entry.name, *value);
                    if (declaration.name) {
                        const auto enum_member = *declaration.name + "." + entry.name;
                        publish_integer(enum_member, *value);
                        if (enumeration != input.enums.end() &&
                            entry_index < enumeration->entries.size()) {
                            enumeration->entries[entry_index].value = *value;
                        }
                    } else {
                        publish_integer(entry.name, *value);
                        const auto member = std::find_if(
                            input.members.begin(), input.members.end(), [&](const auto& candidate) {
                                return candidate.kind == ScriptMemberKind::enum_value &&
                                       candidate.name == entry.name;
                            });
                        if (member != input.members.end()) {
                            member->constant_value = *value;
                            member->folded_integer_value = *value;
                        }
                    }
                    next_value = *value == std::numeric_limits<std::int64_t>::max()
                                     ? std::optional<std::int64_t>{}
                                     : std::optional<std::int64_t>{*value + 1};
                }
            }
        }
        if (!changed)
            break;
    }

    report_project_progress(options, ProjectCompilePhase::analyze, 0, script_progress_total);
    for (const auto& [name, bridge_class] : bridge_classes) {
        if (!target_api.find_class(bridge_class.type->godot_base)) {
            result.diagnostics.push_back(
                {bridge_class.bridge->manifest_path,
                 project_error("PRJ0021", "bridge class '" + name +
                                              "' declares unknown Godot base '" +
                                              bridge_class.type->godot_base + "'")});
        }
        if (target_api.find_class(name)) {
            result.diagnostics.push_back(
                {bridge_class.bridge->manifest_path,
                 project_error("PRJ0023",
                               "bridge class '" + name + "' collides with a Godot engine type")});
        }
        if (const auto script = script_classes.find(name); script != script_classes.end()) {
            result.diagnostics.push_back(
                {bridge_class.bridge->manifest_path,
                 project_error("PRJ0023", "bridge class '" + name +
                                              "' collides with global script class declared by " +
                                              inputs[script->second].relative)});
        }
        const auto autoload = std::find_if(
            project_autoloads.begin(), project_autoloads.end(),
            [bridge_name = name](const auto& entry) { return entry.second == bridge_name; });
        if (autoload != project_autoloads.end()) {
            result.diagnostics.push_back(
                {bridge_class.bridge->manifest_path,
                 project_error("PRJ0023", "bridge class '" + name +
                                              "' collides with project autoload declared for " +
                                              autoload->first)});
        }
        const auto bridge_manifest_path = bridge_class.bridge->manifest_path;
        const auto validate_contract_type = [&](const std::string& type_name,
                                                const bool allow_void) {
            const auto type = type_from_annotation(type_name);
            const bool known_object = type.kind != TypeKind::object ||
                                      target_api.find_class(type_name) ||
                                      bridge_classes.find(type_name) != bridge_classes.end() ||
                                      script_classes.find(type_name) != script_classes.end() ||
                                      find_bridge_enum(type_name);
            if ((!allow_void && type.kind == TypeKind::void_type) || !known_object) {
                result.diagnostics.push_back(
                    {bridge_manifest_path,
                     project_error("PRJ0024", "bridge member type '" + type_name +
                                                  "' is not available to the target project")});
            }
        };
        for (const auto& member : bridge_class.type->members) {
            validate_contract_type(member.type, member.kind == ExtensionBridgeMemberKind::method);
            for (const auto& parameter : member.parameters)
                validate_contract_type(parameter.type, false);
        }
    }
    if (has_project_errors(result.diagnostics))
        return result;

    const auto resolve_script_input = [&](const SourceInput& owner,
                                          std::string reference) -> std::optional<std::size_t> {
        if (const auto alias = resource_aliases.find(reference); alias != resource_aliases.end())
            reference = alias->second;
        if (const auto exact = script_paths.find(reference); exact != script_paths.end())
            return exact->second;
        const auto normalized = normalized_script_reference(owner.relative, reference);
        if (const auto path = script_paths.find(normalized); path != script_paths.end())
            return path->second;
        return std::nullopt;
    };
    // An internal class may inherit a globally named script, an autoload, a script resource held
    // by a lexical preload constant, or an internal class owned by any of those scripts. Resolve
    // that source-language identity before semantic analysis so the frontend and generated C++
    // share one explicit native inheritance graph.
    for (auto& input : inputs) {
        for (auto& inner : input.inner_classes) {
            if (inner.base_class_name.empty())
                continue;
            const auto local_base = std::find_if(
                input.inner_classes.begin(), input.inner_classes.end(),
                [&](const auto& candidate) { return candidate.name == inner.base_class_name; });
            if (local_base != input.inner_classes.end())
                continue;

            if (const auto external = bridge_classes.find(inner.base_class_name);
                external != bridge_classes.end()) {
                inner.external_base_name = inner.base_class_name;
                inner.godot_base_type = external->second.type->godot_base;
                inner.attached_native_base = inner.external_base_name;
                inner.base_class_name.clear();
                continue;
            }

            std::optional<std::size_t> script_base;
            std::string inner_base_name;
            const auto qualified_separator = inner.base_class_name.find('.');
            if (qualified_separator != std::string::npos) {
                const auto owner_name = inner.base_class_name.substr(0, qualified_separator);
                const auto member_name = inner.base_class_name.substr(qualified_separator + 1);
                if (const auto global = script_classes.find(owner_name);
                    global != script_classes.end()) {
                    script_base = global->second;
                } else if (const auto autoload = autoload_classes.find(owner_name);
                           autoload != autoload_classes.end()) {
                    script_base = autoload->second;
                }
                if (script_base) {
                    const auto& owner = inputs[*script_base];
                    const auto base =
                        std::find_if(owner.inner_classes.begin(), owner.inner_classes.end(),
                                     [&](const ScriptInnerClassSymbol& candidate) {
                                         return candidate.name == member_name;
                                     });
                    if (base == owner.inner_classes.end())
                        script_base.reset();
                    else
                        inner_base_name = base->name;
                }
            }
            const auto alias = std::find_if(
                input.script.variables.begin(), input.script.variables.end(),
                [&](const ast::VariableDeclaration& variable) {
                    return variable.is_constant && variable.name == inner.base_class_name;
                });
            if (!script_base && alias != input.script.variables.end()) {
                if (const auto path = direct_preload_path(alias->initializer.get()))
                    script_base = resolve_script_input(input, *path);
            }
            if (!script_base) {
                if (const auto global = script_classes.find(inner.base_class_name);
                    global != script_classes.end()) {
                    script_base = global->second;
                }
            }
            if (!script_base)
                continue;
            inner.base_script_path = inputs[*script_base].relative;
            inner.base_class_name = std::move(inner_base_name);
            if (!inner.base_class_name.empty()) {
                const auto& base_owner = inputs[*script_base];
                const auto base =
                    std::find_if(base_owner.inner_classes.begin(), base_owner.inner_classes.end(),
                                 [&](const ScriptInnerClassSymbol& candidate) {
                                     return candidate.name == inner.base_class_name;
                                 });
                if (base != base_owner.inner_classes.end()) {
                    inner.godot_base_type = base->godot_base_type;
                    inner.attached_native_base = base->attached_native_base;
                    inner.external_base_name = base->external_base_name;
                }
            }
        }
    }

    for (auto& input : inputs) {
        const auto resolve_enum_type = [&](Type& type) {
            if (type.kind != TypeKind::object)
                return;
            const auto local = std::find_if(
                input.enums.begin(), input.enums.end(),
                [&](const ScriptEnumSymbol& enumeration) { return enumeration.name == type.name; });
            if (local != input.enums.end()) {
                type = {TypeKind::enumeration, input.script_class_name + "." + local->name};
                return;
            }
            const auto inner = std::find_if(
                input.inner_classes.begin(), input.inner_classes.end(), [&](const auto& owner) {
                    return std::any_of(owner.enums.begin(), owner.enums.end(),
                                       [&](const auto& enumeration) {
                                           return owner.name + "." + enumeration.name == type.name;
                                       });
                });
            if (inner != input.inner_classes.end()) {
                type.kind = TypeKind::enumeration;
                return;
            }
            const auto separator = type.name.find('.');
            if (separator == std::string::npos)
                return;
            const auto owner_name = type.name.substr(0, separator);
            const auto global = script_classes.find(owner_name);
            const auto autoload = autoload_classes.find(owner_name);
            if (global == script_classes.end() && autoload == autoload_classes.end())
                return;
            const auto owner_index =
                global != script_classes.end() ? global->second : autoload->second;
            const auto& enumerations = inputs[owner_index].enums;
            const auto found = std::find_if(
                enumerations.begin(), enumerations.end(), [&](const ScriptEnumSymbol& enumeration) {
                    return enumeration.name == type.name.substr(separator + 1);
                });
            if (found != enumerations.end())
                type.kind = TypeKind::enumeration;
        };
        for (auto& member : input.members) {
            resolve_enum_type(member.type);
            for (auto& parameter : member.parameters)
                resolve_enum_type(parameter);
        }
        for (auto& inner : input.inner_classes) {
            for (auto& member : inner.members) {
                if ((member.type.kind == TypeKind::object ||
                     member.type.kind == TypeKind::enumeration) &&
                    member.type.name.find('.') == std::string::npos) {
                    const auto enumeration = std::find_if(
                        inner.enums.begin(), inner.enums.end(),
                        [&](const auto& candidate) { return candidate.name == member.type.name; });
                    if (enumeration != inner.enums.end()) {
                        member.type = {TypeKind::enumeration, inner.name + "." + enumeration->name};
                    }
                }
                resolve_enum_type(member.type);
                for (auto& parameter : member.parameters) {
                    if ((parameter.kind == TypeKind::object ||
                         parameter.kind == TypeKind::enumeration) &&
                        parameter.name.find('.') == std::string::npos) {
                        const auto enumeration = std::find_if(
                            inner.enums.begin(), inner.enums.end(), [&](const auto& candidate) {
                                return candidate.name == parameter.name;
                            });
                        if (enumeration != inner.enums.end()) {
                            parameter = {TypeKind::enumeration,
                                         inner.name + "." + enumeration->name};
                        }
                    }
                    resolve_enum_type(parameter);
                }
            }
        }
    }

    for (std::size_t index = 0; index < inputs.size(); ++index) {
        auto& input = inputs[index];
        if (target_api.find_class(input.base_reference)) {
            input.semantic_base_type = input.base_reference;
            input.attached_native_base = input.base_reference;
            input.attached = true;
            continue;
        }
        const auto by_class = script_classes.find(input.base_reference);
        if (by_class != script_classes.end()) {
            input.script_base = by_class->second;
            continue;
        }
        const auto path_reference =
            normalized_script_reference(input.relative, input.base_reference);
        const auto by_path = script_paths.find(path_reference);
        if (by_path != script_paths.end()) {
            input.script_base = by_path->second;
            continue;
        }
        const auto local_inner =
            std::find_if(input.inner_classes.begin(), input.inner_classes.end(),
                         [&](const auto& inner) { return inner.name == input.base_reference; });
        if (local_inner != input.inner_classes.end()) {
            auto terminal = local_inner;
            std::unordered_set<std::string> visited;
            while (!terminal->base_class_name.empty() && visited.insert(terminal->name).second) {
                const auto base =
                    std::find_if(input.inner_classes.begin(), input.inner_classes.end(),
                                 [&](const auto& candidate) {
                                     return candidate.name == terminal->base_class_name;
                                 });
                if (base == input.inner_classes.end())
                    break;
                terminal = base;
            }
            input.local_inner_base =
                static_cast<std::size_t>(std::distance(input.inner_classes.begin(), local_inner));
            input.semantic_base_type = terminal->godot_base_type;
            input.attached_native_base = terminal->attached_native_base;
            input.external_base_name = terminal->external_base_name;
            input.attached = true;
            continue;
        }
        const auto bridged = bridge_classes.find(input.base_reference);
        if (bridged != bridge_classes.end()) {
            if (!target_api.find_class(bridged->second.type->godot_base)) {
                result.diagnostics.push_back(
                    {input.path,
                     project_error("PRJ0021", "bridge base '" + input.base_reference +
                                                  "' declares unknown Godot base '" +
                                                  bridged->second.type->godot_base + "'")});
                continue;
            }
            input.extension_base = bridged->second;
            input.external_base_name = input.base_reference;
            input.attached_native_base = input.base_reference;
            input.attached = true;
            input.semantic_base_type = bridged->second.type->godot_base;
            continue;
        }
        if (external_extension_descriptors.empty()) {
            result.diagnostics.push_back(
                {input.path,
                 project_error("PRJ0013", "base script or Godot type '" + input.base_reference +
                                              "' was not found in the project")});
            continue;
        }
        std::ostringstream providers;
        constexpr std::size_t displayed_provider_limit = 3;
        for (std::size_t provider_index = 0;
             provider_index < external_extension_descriptors.size() &&
             provider_index < displayed_provider_limit;
             ++provider_index) {
            if (provider_index > 0)
                providers << ", ";
            providers << generic_path_to_utf8(
                external_extension_descriptors[provider_index].lexically_relative(root));
        }
        if (external_extension_descriptors.size() > displayed_provider_limit)
            providers << ", ...";
        result.diagnostics.push_back(
            {input.path,
             project_error(
                 "PRJ0018",
                 "base type '" + input.base_reference +
                     "' is not declared by Godot or project scripts; the project contains "
                     "third-party GDExtensions (" +
                     providers.str() +
                     "). Attached AOT requires the provider class and exact MethodBind hashes "
                     "from the active ClassDB snapshot or gdpp_bridge.json; load the provider "
                     "extension in the editor or supply its runtime bridge metadata. "
                     "Binary-only AOT is blocked instead of guessing an unsafe ABI")});
    }
    if (has_project_errors(result.diagnostics))
        return result;

    std::vector<std::size_t> compile_order;
    std::vector<unsigned char> visit_state(inputs.size(), 0);
    std::vector<std::size_t> inheritance_stack;
    const auto visit = [&](auto&& self, const std::size_t index) -> bool {
        if (visit_state[index] == 2)
            return true;
        if (visit_state[index] == 1) {
            std::ostringstream cycle;
            const auto begin = std::find(inheritance_stack.begin(), inheritance_stack.end(), index);
            for (auto current = begin; current != inheritance_stack.end(); ++current) {
                if (current != begin)
                    cycle << " -> ";
                cycle << inputs[*current].script_class_name;
            }
            cycle << " -> " << inputs[index].script_class_name;
            result.diagnostics.push_back(
                {inputs[index].path,
                 project_error("PRJ0014", "cyclic generated class inheritance: " + cycle.str())});
            return false;
        }
        visit_state[index] = 1;
        inheritance_stack.push_back(index);
        if (inputs[index].script_base) {
            if (!self(self, *inputs[index].script_base))
                return false;
            inputs[index].semantic_base_type =
                inputs[*inputs[index].script_base].semantic_base_type;
            inputs[index].external_base_name =
                inputs[*inputs[index].script_base].external_base_name;
            inputs[index].attached_native_base =
                inputs[*inputs[index].script_base].attached_native_base;
            inputs[index].attached = inputs[*inputs[index].script_base].attached;
        }
        for (auto& inner : inputs[index].inner_classes) {
            if (inner.base_script_path.empty())
                continue;
            const auto dependency = script_paths.find(inner.base_script_path);
            if (dependency == script_paths.end())
                continue;
            if (!self(self, dependency->second))
                return false;
            if (inner.base_class_name.empty()) {
                inner.godot_base_type = inputs[dependency->second].semantic_base_type;
                inner.attached_native_base = inputs[dependency->second].attached_native_base;
                inner.external_base_name = inputs[dependency->second].external_base_name;
            } else {
                const auto& base_owner = inputs[dependency->second];
                const auto base =
                    std::find_if(base_owner.inner_classes.begin(), base_owner.inner_classes.end(),
                                 [&](const ScriptInnerClassSymbol& candidate) {
                                     return candidate.name == inner.base_class_name;
                                 });
                if (base != base_owner.inner_classes.end()) {
                    inner.godot_base_type = base->godot_base_type;
                    inner.attached_native_base = base->attached_native_base;
                    inner.external_base_name = base->external_base_name;
                }
            }
        }
        for (std::size_t pass = 0; pass < inputs[index].inner_classes.size(); ++pass) {
            bool changed = false;
            for (auto& inner : inputs[index].inner_classes) {
                if (inner.base_class_name.empty())
                    continue;
                const auto base = std::find_if(
                    inputs[index].inner_classes.begin(), inputs[index].inner_classes.end(),
                    [&](const auto& candidate) { return candidate.name == inner.base_class_name; });
                if (base == inputs[index].inner_classes.end())
                    continue;
                if (inner.godot_base_type != base->godot_base_type ||
                    inner.attached_native_base != base->attached_native_base ||
                    inner.external_base_name != base->external_base_name) {
                    inner.godot_base_type = base->godot_base_type;
                    inner.attached_native_base = base->attached_native_base;
                    inner.external_base_name = base->external_base_name;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        inheritance_stack.pop_back();
        visit_state[index] = 2;
        compile_order.push_back(index);
        return true;
    };
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (!visit(visit, index))
            break;
    }
    if (has_project_errors(result.diagnostics))
        return result;

    // Resolve omitted override annotations before building the shared symbol table. GDScript
    // permits an overriding method to omit types that are present on its script base; emitting
    // those omissions as Variant would create an illegal C++ virtual override and would also
    // weaken every cross-script call site. The topological order guarantees that inherited
    // signatures are already stable when a derived script is processed.
    const auto inherited_member = [&](const auto& self, const std::size_t owner,
                                      const std::string& name) -> const ScriptMemberSymbol* {
        if (!inputs[owner].script_base)
            return nullptr;
        const auto base = *inputs[owner].script_base;
        const auto found = std::find_if(
            inputs[base].members.begin(), inputs[base].members.end(), [&](const auto& member) {
                return member.kind == ScriptMemberKind::function && member.name == name;
            });
        return found != inputs[base].members.end() ? &*found : self(self, base, name);
    };
    for (const auto index : compile_order) {
        auto& input = inputs[index];
        for (auto& member : input.members) {
            if (member.kind != ScriptMemberKind::function || member.is_static ||
                member.name == "_init") {
                continue;
            }
            // First normalize omitted annotations against Godot's actual virtual ABI. This is
            // also the root contract inherited through arbitrarily deep script hierarchies.
            if (const auto* method = target_api.find_method(input.semantic_base_type, member.name);
                method && method->is_virtual) {
                if (!member.has_explicit_type) {
                    member.type = std::string_view{method->return_type}.empty()
                                      ? Type{TypeKind::void_type, "void"}
                                      : type_from_godot_api(method->return_type);
                }
                for (std::size_t parameter = 0;
                     parameter < member.parameters.size() && parameter < method->maximum_arguments;
                     ++parameter) {
                    if ((parameter >= member.explicit_parameter_types.size() ||
                         !member.explicit_parameter_types[parameter])) {
                        if (const auto* argument = target_api.argument(*method, parameter))
                            member.parameters[parameter] = type_from_godot_api(argument->type);
                    }
                }
            }
            if (!input.script_base)
                continue;
            const auto* inherited = inherited_member(inherited_member, index, member.name);
            if (!inherited || inherited->is_static) {
                continue;
            }
            if (!member.has_explicit_type)
                member.type = inherited->type;
            const auto shared_parameters =
                std::min(member.parameters.size(), inherited->parameters.size());
            for (std::size_t parameter = 0; parameter < shared_parameters; ++parameter) {
                if (parameter >= member.explicit_parameter_types.size() ||
                    !member.explicit_parameter_types[parameter]) {
                    member.parameters[parameter] = inherited->parameters[parameter];
                }
            }
        }
    }

    const auto append_expression_identity = [](const auto& self, std::ostringstream& identity,
                                               const ast::Expression& expression) -> void {
        identity << '(' << static_cast<int>(expression.kind()) << ':'
                 << static_cast<int>(expression.literal_kind()) << ':' << expression.value();
        for (std::size_t index = 0; index < expression.operand_count(); ++index)
            self(self, identity, *expression.operand(index));
        if (const auto* lambda = expression.lambda()) {
            identity << ":lambda:" << lambda->parameters.size() << ':'
                     << lambda->return_type.value_or("");
            for (const auto& parameter : lambda->parameters) {
                identity << ':' << parameter.name << ':' << parameter.type.value_or("");
                if (parameter.default_value)
                    self(self, identity, *parameter.default_value);
            }
            if (lambda->rest_parameter) {
                identity << ":rest:" << lambda->rest_parameter->name << ':'
                         << lambda->rest_parameter->type.value_or("") << ':'
                         << lambda->rest_parameter->infer_type;
            }
        }
        identity << ')';
    };
    const auto append_annotation_identity = [&](std::ostringstream& identity,
                                                const std::vector<ast::Annotation>& annotations) {
        for (const auto& annotation : annotations) {
            identity << "@" << annotation.name << ':' << annotation.arguments.size();
            for (const auto& argument : annotation.arguments)
                append_expression_identity(append_expression_identity, identity, *argument);
            identity << ';';
        }
    };
    const auto append_public_abi = [&](const SourceInput& input) {
        std::ostringstream identity;
        identity << "path:" << input.relative << ":script:" << input.script_class_name
                 << ":native-stem:" << input.native_class_stem << ":base:" << input.base_reference
                 << ":api-base:" << input.semantic_base_type << ":autoload:" << input.autoload_name
                 << ":external-base:" << input.external_base_name << ":attached:" << input.attached
                 << ":attached-native-base:" << input.attached_native_base
                 << ":abstract:" << input.is_abstract << ":tool:" << input.script.tool
                 << ":static_unload:" << input.static_unload << ':';
        append_annotation_identity(identity, input.script.annotations);
        identity << '\n';
        if (input.extension_base.bridge && input.extension_base.type) {
            identity << "extension-base:" << input.extension_base.type->gdscript_name << ':'
                     << input.extension_base.type->cpp_type << ':'
                     << input.extension_base.type->header << ':'
                     << input.extension_base.type->godot_base
                     << ":abi:" << bridge_contract_identity(*input.extension_base.bridge) << '\n';
        }
        for (const auto& member : input.members) {
            identity << static_cast<int>(member.kind) << ':' << member.name << ':'
                     << static_cast<int>(member.type.kind) << ':' << member.type.name << ':'
                     << member.required_arguments << ':' << member.is_static << ':'
                     << member.has_accessor << ':' << member.getter_method << ':'
                     << member.setter_method << ':' << member.getter_is_coroutine << ':'
                     << member.setter_is_coroutine << ':' << member.has_explicit_type << ':'
                     << member.is_vararg << ':' << member.is_coroutine << ':' << member.is_abstract
                     << ':' << member.property_storage << ':' << member.property_editor;
            if (const auto bridge = bridge_classes.find(member.type.name);
                bridge != bridge_classes.end()) {
                identity << ":bridge-abi:" << bridge_contract_identity(*bridge->second.bridge);
            }
            for (std::size_t index = 0; index < member.parameters.size(); ++index) {
                const auto& parameter = member.parameters[index];
                identity << ':' << static_cast<int>(parameter.kind) << ':' << parameter.name << ':'
                         << (index < member.parameter_names.size() ? member.parameter_names[index]
                                                                   : std::string{})
                         << ':'
                         << (index < member.explicit_parameter_types.size() &&
                             member.explicit_parameter_types[index])
                         << ':'
                         << (index < member.default_parameters.size() &&
                             member.default_parameters[index]);
                if (const auto bridge = bridge_classes.find(parameter.name);
                    bridge != bridge_classes.end()) {
                    identity << ":bridge-abi:" << bridge_contract_identity(*bridge->second.bridge);
                }
            }
            identity << '\n';
        }
        for (const auto& enumeration : input.enums) {
            identity << "enum:" << enumeration.name;
            for (const auto& entry : enumeration.entries)
                identity << ':' << entry.name << ':' << entry.value;
            identity << '\n';
        }
        for (const auto& inner : input.inner_classes) {
            identity << "inner:" << inner.name << ':' << inner.godot_base_type << ':'
                     << inner.attached_native_base << ':' << inner.external_base_name << ':'
                     << inner.base_class_name << ':' << inner.base_script_path << ':'
                     << inner.is_abstract << '\n';
            for (const auto& member : inner.members) {
                identity << "inner-member:" << static_cast<int>(member.kind) << ':' << member.name
                         << ':' << static_cast<int>(member.type.kind) << ':' << member.type.name
                         << ':' << member.required_arguments << ':' << member.is_static << ':'
                         << member.has_accessor << ':' << member.getter_method << ':'
                         << member.setter_method << ':' << member.getter_is_coroutine << ':'
                         << member.setter_is_coroutine << ':' << member.is_vararg << ':'
                         << member.is_coroutine << ':' << member.is_abstract;
                if (const auto bridge = bridge_classes.find(member.type.name);
                    bridge != bridge_classes.end()) {
                    identity << ":bridge-abi:" << bridge_contract_identity(*bridge->second.bridge);
                }
                for (const auto& parameter : member.parameters) {
                    identity << ':' << static_cast<int>(parameter.kind) << ':' << parameter.name;
                    if (const auto bridge = bridge_classes.find(parameter.name);
                        bridge != bridge_classes.end()) {
                        identity << ":bridge-abi:"
                                 << bridge_contract_identity(*bridge->second.bridge);
                    }
                }
                identity << '\n';
            }
        }
        for (const auto& variable : input.script.variables) {
            identity << "field-metadata:" << variable.name << ':' << variable.is_static << ':'
                     << variable.is_constant << ':' << variable.onready << ':';
            append_annotation_identity(identity, variable.annotations);
            identity << '\n';
            if (variable.is_constant && variable.initializer) {
                identity << "constant-value:" << variable.name << ':';
                append_expression_identity(append_expression_identity, identity,
                                           *variable.initializer);
                identity << '\n';
            }
        }
        for (const auto& function : input.script.functions) {
            identity << "function-parameters:" << function.name << ':';
            append_annotation_identity(identity, function.annotations);
            for (const auto& parameter : function.parameters) {
                identity << ':' << parameter.name;
                if (parameter.default_value)
                    append_expression_identity(append_expression_identity, identity,
                                               *parameter.default_value);
            }
            if (function.rest_parameter) {
                identity << ":rest:" << function.rest_parameter->name << ':'
                         << function.rest_parameter->type.value_or("") << ':'
                         << function.rest_parameter->infer_type;
            }
            identity << '\n';
        }
        for (const auto& signal : input.script.signals) {
            identity << "signal-parameters:" << signal.name << ':';
            append_annotation_identity(identity, signal.annotations);
            for (const auto& parameter : signal.parameters)
                identity << ':' << parameter.name;
            identity << '\n';
        }
        for (const auto& enumeration : input.script.enums) {
            identity << "enum-metadata:" << enumeration.name.value_or("") << ':';
            append_annotation_identity(identity, enumeration.annotations);
            identity << '\n';
        }
        const auto append_inner_metadata = [&](const auto& self,
                                               const ast::ClassDeclaration& declaration) -> void {
            identity << "inner-metadata:" << declaration.name << ':';
            append_annotation_identity(identity, declaration.annotations);
            identity << '\n';
            for (const auto& variable : declaration.variables) {
                identity << "inner-field-metadata:" << declaration.name << ':' << variable.name
                         << ':';
                append_annotation_identity(identity, variable.annotations);
                identity << '\n';
            }
            for (const auto& function : declaration.functions) {
                identity << "inner-function-metadata:" << declaration.name << ':' << function.name
                         << ':';
                append_annotation_identity(identity, function.annotations);
                if (function.rest_parameter) {
                    identity << ":rest:" << function.rest_parameter->name << ':'
                             << function.rest_parameter->type.value_or("") << ':'
                             << function.rest_parameter->infer_type;
                }
                identity << '\n';
            }
            for (const auto& nested : declaration.classes)
                self(self, nested);
        };
        for (const auto& inner : input.script.classes)
            append_inner_metadata(append_inner_metadata, inner);
        return sha256(identity.str());
    };
    for (auto& input : inputs)
        input.public_abi_hash = append_public_abi(input);

    std::vector<std::string> native_script_names;
    native_script_names.reserve(inputs.size());
    for (const auto& input : inputs) {
        native_script_names.push_back("GDPPNative_" + input.native_class_stem + "_" +
                                      input.public_abi_hash.substr(0, 16));
    }
    // Signature extraction runs before the full semantic analyzer, so preload aliases initially
    // retain their source spelling (for example `msg.Player` or `msg.State`). Canonicalize those
    // public contracts after stable native identities are known. This keeps every caller and
    // callee on one path-stable object/enum identity without making source alias names part of
    // the generated C++ ABI.
    for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
        auto& input = inputs[input_index];
        std::unordered_map<std::string, std::size_t> aliases;
        for (const auto& variable : input.script.variables) {
            if (!variable.is_constant)
                continue;
            if (const auto path = direct_preload_path(variable.initializer.get())) {
                if (const auto target = resolve_script_input(input, *path))
                    aliases.insert_or_assign(variable.name, *target);
            }
        }
        const auto canonicalize_type = [&](const auto& self, Type& type) -> void {
            if (const auto container = describe_container_type(type)) {
                std::vector<Type> arguments;
                arguments.reserve(container->arguments.size());
                for (const auto& argument : container->arguments) {
                    auto argument_type = type_from_annotation(argument);
                    self(self, argument_type);
                    arguments.push_back(std::move(argument_type));
                }
                std::string name =
                    container->kind == ContainerTypeKind::array ? "Array[" : "Dictionary[";
                for (std::size_t index = 0; index < arguments.size(); ++index) {
                    if (index != 0)
                        name += ", ";
                    name += arguments[index].name;
                }
                name += ']';
                type = {type.kind, std::move(name)};
                return;
            }
            if (type.kind != TypeKind::object && type.kind != TypeKind::enumeration)
                return;

            if (const auto exact_alias = aliases.find(type.name); exact_alias != aliases.end()) {
                type = {TypeKind::object, native_script_names[exact_alias->second]};
                return;
            }
            std::size_t owner_index = input_index;
            std::string member_name = type.name;
            if (const auto separator = type.name.find('.'); separator != std::string::npos) {
                const auto owner_name = type.name.substr(0, separator);
                member_name = type.name.substr(separator + 1);
                if (const auto alias = aliases.find(owner_name); alias != aliases.end()) {
                    owner_index = alias->second;
                } else if (const auto global = script_classes.find(owner_name);
                           global != script_classes.end()) {
                    owner_index = global->second;
                } else if (const auto autoload = autoload_classes.find(owner_name);
                           autoload != autoload_classes.end()) {
                    owner_index = autoload->second;
                } else if (owner_name != input.script_class_name) {
                    const bool local_inner_owner =
                        std::any_of(input.inner_classes.begin(), input.inner_classes.end(),
                                    [&](const ScriptInnerClassSymbol& candidate) {
                                        return candidate.name == owner_name ||
                                               candidate.name.rfind(owner_name + ".", 0) == 0;
                                    });
                    if (!local_inner_owner)
                        return;
                    member_name = type.name;
                }
            }

            const auto& owner = inputs[owner_index];
            const auto root_enum = std::find_if(owner.enums.begin(), owner.enums.end(),
                                                [&](const ScriptEnumSymbol& enumeration) {
                                                    return enumeration.name == member_name;
                                                });
            if (root_enum != owner.enums.end()) {
                type = {TypeKind::enumeration,
                        native_script_names[owner_index] + "::" + root_enum->name};
                return;
            }
            if (const auto* inner = [&]() -> const ScriptInnerClassSymbol* {
                    const auto found =
                        std::find_if(owner.inner_classes.begin(), owner.inner_classes.end(),
                                     [&](const ScriptInnerClassSymbol& candidate) {
                                         return candidate.name == member_name;
                                     });
                    return found == owner.inner_classes.end() ? nullptr : &*found;
                }()) {
                type = {TypeKind::object,
                        native_script_names[owner_index] + "__" + native_inner_suffix(inner->name)};
                return;
            }
            const auto enum_separator = member_name.rfind('.');
            if (enum_separator == std::string::npos)
                return;
            const auto inner_name = member_name.substr(0, enum_separator);
            const auto enum_name = member_name.substr(enum_separator + 1);
            const auto inner = std::find_if(owner.inner_classes.begin(), owner.inner_classes.end(),
                                            [&](const ScriptInnerClassSymbol& candidate) {
                                                return candidate.name == inner_name;
                                            });
            if (inner == owner.inner_classes.end())
                return;
            const auto enumeration = std::find_if(
                inner->enums.begin(), inner->enums.end(),
                [&](const ScriptEnumSymbol& candidate) { return candidate.name == enum_name; });
            if (enumeration != inner->enums.end()) {
                type = {TypeKind::enumeration, native_script_names[owner_index] + "__" +
                                                   native_inner_suffix(inner->name) +
                                                   "::" + enumeration->name};
            }
        };
        for (auto& member : input.members) {
            canonicalize_type(canonicalize_type, member.type);
            for (auto& parameter : member.parameters)
                canonicalize_type(canonicalize_type, parameter);
        }
        for (auto& inner : input.inner_classes) {
            for (auto& member : inner.members) {
                canonicalize_type(canonicalize_type, member.type);
                for (auto& parameter : member.parameters)
                    canonicalize_type(canonicalize_type, parameter);
            }
        }
    }

    ScriptSymbolTable script_symbols;
    for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
        const auto& input = inputs[input_index];
        ScriptClassSymbol symbol;
        symbol.path = input.relative;
        symbol.script_name = input.script_class_name;
        symbol.native_class_name = native_script_names[input_index];
        symbol.header_file_name = to_snake_case(input.native_class_stem) + ".gd.hpp";
        symbol.godot_base_type = input.semantic_base_type;
        symbol.attached_native_base = input.attached_native_base;
        symbol.external_base_name = input.external_base_name;
        symbol.attached = input.attached;
        if (input.script_base)
            symbol.base_script_path = inputs[*input.script_base].relative;
        symbol.globally_named = input.globally_named;
        symbol.is_abstract = input.is_abstract;
        symbol.is_tool = input.script.tool;
        symbol.autoload_name = input.autoload_name;
        symbol.members = input.members;
        symbol.enums = input.enums;
        symbol.inner_classes = input.inner_classes;
        for (auto& inner : symbol.inner_classes) {
            inner.native_class_name =
                symbol.native_class_name + "__" + native_inner_suffix(inner.name);
        }
        script_symbols.add(std::move(symbol));
    }
    script_symbols.canonicalize_project_types(target_api);
    for (const auto& bridge : bridge_load.bridges) {
        for (const auto& type : bridge.classes) {
            ExternalClassSymbol external;
            external.name = type.gdscript_name;
            external.godot_base_type = type.godot_base;
            external.provider_abi = bridge_contract_identity(bridge);
            external.runtime_only = type.runtime_only;
            external.members_complete = type.members_complete;
            const auto external_member_type = [&](const std::string& name) {
                return find_bridge_enum(name) ? Type{TypeKind::enumeration, name}
                                              : type_from_annotation(name);
            };
            for (const auto& bridge_member : type.members) {
                ScriptMemberSymbol member;
                member.name = bridge_member.name;
                member.type = external_member_type(bridge_member.type);
                member.has_explicit_type = true;
                member.is_vararg = bridge_member.vararg;
                member.is_static = bridge_member.is_static;
                member.read_only = bridge_member.read_only;
                member.constant_value = bridge_member.constant_value;
                member.method_hash = bridge_member.method_hash;
                member.has_method_hash = bridge_member.has_method_hash;
                member.has_accessor = bridge_member.kind == ExtensionBridgeMemberKind::property;
                if (bridge_member.kind == ExtensionBridgeMemberKind::method)
                    member.kind = ScriptMemberKind::function;
                else if (bridge_member.kind == ExtensionBridgeMemberKind::signal)
                    member.kind = ScriptMemberKind::signal;
                else if (bridge_member.kind == ExtensionBridgeMemberKind::constant)
                    member.kind = ScriptMemberKind::constant;
                else
                    member.kind = ScriptMemberKind::field;
                for (const auto& parameter : bridge_member.parameters) {
                    member.parameters.push_back(external_member_type(parameter.type));
                    member.explicit_parameter_types.push_back(true);
                    member.default_parameters.push_back(parameter.has_default);
                    if (!parameter.has_default)
                        ++member.required_arguments;
                }
                external.members.push_back(std::move(member));
            }
            for (const auto& bridge_enum : type.enums) {
                ScriptEnumSymbol enumeration;
                enumeration.name = bridge_enum.name;
                enumeration.is_bitfield = bridge_enum.is_bitfield;
                for (const auto& entry : bridge_enum.entries)
                    enumeration.entries.push_back({entry.name, entry.value});
                external.enums.push_back(std::move(enumeration));
            }
            script_symbols.add_external(std::move(external));
        }
    }
    for (const auto& [uid, path] : resource_aliases)
        script_symbols.add_resource_alias(uid, path);

    // Coroutine status is part of the native ABI, but a purely syntactic scan is necessarily
    // conservative (`await 42` is immediate, while `await another_script.call()` depends on the
    // callee contract). Refine the provisional symbol graph to a semantic fixed point before
    // hashing public ABIs. Starting from the conservative graph guarantees that recursive call
    // groups never lose a possible suspension edge.
    std::size_t refinement_budget = 0;
    const auto count_inner_declarations =
        [&](const auto& self, const std::vector<ast::ClassDeclaration>& classes) -> void {
        for (const auto& declaration : classes) {
            refinement_budget += declaration.functions.size() + declaration.variables.size();
            self(self, declaration.classes);
        }
    };
    for (const auto& input : inputs) {
        refinement_budget += input.script.functions.size() + input.script.variables.size();
        count_inner_declarations(count_inner_declarations, input.script.classes);
    }
    for (std::size_t iteration = 0; iteration <= refinement_budget; ++iteration) {
        bool changed = false;
        for (auto& input : inputs) {
            DiagnosticBag provisional_diagnostics{options.compiler.frontend_limits.max_diagnostics};
            SemanticAnalyzer analyzer{provisional_diagnostics, target_api, input.semantic_base_type,
                                      &script_symbols, input.relative};
            const auto semantic = analyzer.analyze(input.script);
            const auto refine_members = [&](auto& members,
                                            const std::vector<ast::FunctionDeclaration>& functions,
                                            const std::string& inner_name) {
                for (const auto& function : functions) {
                    const auto member =
                        std::find_if(members.begin(), members.end(), [&](const auto& candidate) {
                            return candidate.kind == ScriptMemberKind::function &&
                                   candidate.name == function.name;
                        });
                    if (member == members.end())
                        continue;
                    for (std::size_t parameter = 0; parameter < function.parameters.size();
                         ++parameter) {
                        if (!function.parameters[parameter].infer_type ||
                            parameter >= member->parameters.size()) {
                            continue;
                        }
                        const auto inferred = semantic.type_of(function.parameters[parameter]);
                        if (inferred.kind == TypeKind::unknown ||
                            member->parameters[parameter] == inferred) {
                            continue;
                        }
                        member->parameters[parameter] = inferred;
                        script_symbols.set_parameter_type(input.relative, inner_name, function.name,
                                                          parameter, inferred);
                        changed = true;
                    }
                    const auto coroutine = semantic.is_coroutine(function);
                    if (member->is_coroutine != coroutine) {
                        member->is_coroutine = coroutine;
                        changed = true;
                    }
                    script_symbols.set_coroutine(input.relative, inner_name, function.name,
                                                 coroutine);
                }
            };
            const auto refine_variables =
                [&](auto& members, const std::vector<ast::VariableDeclaration>& variables,
                    const std::string& inner_name) {
                    for (const auto& variable : variables) {
                        const auto member = std::find_if(
                            members.begin(), members.end(), [&](const auto& candidate) {
                                return (candidate.kind == ScriptMemberKind::field ||
                                        candidate.kind == ScriptMemberKind::constant) &&
                                       candidate.name == variable.name;
                            });
                        if (member == members.end())
                            continue;
                        if (variable.infer_type ||
                            (variable.is_constant && !variable.type.has_value())) {
                            const auto inferred = semantic.type_of(variable);
                            if (inferred.kind != TypeKind::unknown && member->type != inferred) {
                                member->type = inferred;
                                script_symbols.set_variable_type(input.relative, inner_name,
                                                                 variable.name, inferred);
                                changed = true;
                            }
                        }
                        if (member->kind != ScriptMemberKind::field)
                            continue;
                        const bool getter_coroutine =
                            variable.getter && semantic.is_coroutine(*variable.getter);
                        const bool setter_coroutine =
                            variable.setter && semantic.is_coroutine(*variable.setter);
                        if (member->getter_is_coroutine != getter_coroutine ||
                            member->setter_is_coroutine != setter_coroutine) {
                            member->getter_is_coroutine = getter_coroutine;
                            member->setter_is_coroutine = setter_coroutine;
                            changed = true;
                        }
                        script_symbols.set_accessor_coroutines(input.relative, inner_name,
                                                               variable.name, getter_coroutine,
                                                               setter_coroutine);
                    }
                };
            refine_members(input.members, input.script.functions, "");
            refine_variables(input.members, input.script.variables, "");
            const auto refine_inner = [&](const auto& self,
                                          const std::vector<ast::ClassDeclaration>& classes,
                                          const std::string& parent) -> void {
                for (const auto& declaration : classes) {
                    const auto qualified =
                        parent.empty() ? declaration.name : parent + "." + declaration.name;
                    const auto symbol = std::find_if(
                        input.inner_classes.begin(), input.inner_classes.end(),
                        [&](const auto& candidate) { return candidate.name == qualified; });
                    if (symbol != input.inner_classes.end()) {
                        refine_members(symbol->members, declaration.functions, qualified);
                        refine_variables(symbol->members, declaration.variables, qualified);
                    }
                    self(self, declaration.classes, qualified);
                }
            };
            refine_inner(refine_inner, input.script.classes, "");
        }
        if (!changed)
            break;
    }
    std::vector<std::string> finalized_native_script_names;
    finalized_native_script_names.reserve(inputs.size());
    for (auto& input : inputs) {
        input.public_abi_hash = append_public_abi(input);
        finalized_native_script_names.push_back("GDPPNative_" + input.native_class_stem + "_" +
                                                input.public_abi_hash.substr(0, 16));
    }
    const auto remap_input_type = [](Type& type, const std::string& previous,
                                     const std::string& replacement) {
        std::size_t offset = 0;
        while ((offset = type.name.find(previous, offset)) != std::string::npos) {
            type.name.replace(offset, previous.size(), replacement);
            offset += replacement.size();
        }
    };
    for (std::size_t identity = 0; identity < inputs.size(); ++identity) {
        const auto& previous = native_script_names[identity];
        const auto& replacement = finalized_native_script_names[identity];
        if (previous == replacement)
            continue;
        for (auto& input : inputs) {
            for (auto& member : input.members) {
                remap_input_type(member.type, previous, replacement);
                for (auto& parameter : member.parameters)
                    remap_input_type(parameter, previous, replacement);
            }
            for (auto& inner : input.inner_classes) {
                for (auto& member : inner.members) {
                    remap_input_type(member.type, previous, replacement);
                    for (auto& parameter : member.parameters)
                        remap_input_type(parameter, previous, replacement);
                }
            }
        }
        script_symbols.update_class_identity(inputs[identity].relative, replacement,
                                             to_snake_case(inputs[identity].native_class_stem) +
                                                 ".gd.hpp");
    }

    std::size_t analyzed_inputs = 0;
    for (auto& input : inputs) {
        DiagnosticBag diagnostics{options.compiler.frontend_limits.max_diagnostics};
        SemanticAnalyzer analyzer{diagnostics, target_api, input.semantic_base_type,
                                  &script_symbols, input.relative};
        const auto semantic = analyzer.analyze(input.script);
        for (const auto& diagnostic : diagnostics.items())
            result.diagnostics.push_back({input.path, diagnostic});
        input.dependencies.assign(semantic.referenced_script_paths().begin(),
                                  semantic.referenced_script_paths().end());
        input.extension_abis.assign(semantic.referenced_extension_abis().begin(),
                                    semantic.referenced_extension_abis().end());
        std::sort(input.extension_abis.begin(), input.extension_abis.end());
        if (input.script_base)
            input.dependencies.push_back(inputs[*input.script_base].relative);
        for (const auto& inner : input.inner_classes) {
            if (!inner.base_script_path.empty())
                input.dependencies.push_back(inner.base_script_path);
        }
        std::sort(input.dependencies.begin(), input.dependencies.end());
        input.dependencies.erase(std::unique(input.dependencies.begin(), input.dependencies.end()),
                                 input.dependencies.end());
        report_project_progress(options, ProjectCompilePhase::analyze, ++analyzed_inputs,
                                script_progress_total);
    }
    if (inputs.empty())
        report_project_progress(options, ProjectCompilePhase::analyze, 1, 1);
    if (has_project_errors(result.diagnostics))
        return result;

    for (auto& input : inputs) {
        std::ostringstream dependency_identity;
        for (const auto& dependency : input.dependencies) {
            const auto found = script_paths.find(dependency);
            if (found != script_paths.end())
                dependency_identity << dependency << ':' << inputs[found->second].public_abi_hash
                                    << '\n';
        }
        input.implementation_hash =
            sha256(input.source_hash + ":public-abi:" + input.public_abi_hash +
                   ":dependencies:" + dependency_identity.str() + ":extension-abis:" + [&] {
                       std::ostringstream values;
                       for (const auto& abi : input.extension_abis)
                           values << abi << '\n';
                       return values.str();
                   }());
    }
    std::ostringstream build_identity;
    build_identity << target_api.version() << '\n';
    for (const auto& input : inputs)
        build_identity << input.relative << ':' << input.implementation_hash << '\n';
    result.build_id = sha256(build_identity.str()).substr(0, 16);

    auto compiler_options = options.compiler;
    compiler_options.script_symbols = &script_symbols;

    std::vector<bool> editor_only_inputs(inputs.size(), false);
    for (const auto input_index : compile_order) {
        const auto& input = inputs[input_index];
        editor_only_inputs[input_index] =
            target_api.is_editor_class(input.semantic_base_type) ||
            (input.extension_base.type && input.extension_base.type->editor_only) ||
            (input.script_base && editor_only_inputs[*input.script_base]);
    }

    struct PendingOutput {
        std::size_t script_index;
        std::string header;
        std::string source;
        std::string symbols;
    };
    std::vector<PendingOutput> pending;
    Manifest new_manifest;
    std::unordered_map<std::string, std::string> class_owners;
    std::set<std::string> output_names;
    const Compiler compiler;
    const auto translation_total = std::max<std::size_t>(compile_order.size(), 1);
    std::size_t translated_inputs = 0;
    report_project_progress(options, ProjectCompilePhase::translate, 0, translation_total);
    for (const auto input_index : compile_order) {
        const auto& input = inputs[input_index];
        CompiledProjectScript script;
        script.relative_path = input.relative;
        script.content_hash = input.implementation_hash;
        script.public_abi_hash = input.public_abi_hash;
        script.dependencies = input.dependencies;
        script.icon_path = input.icon_path;
        script.native_base_type = input.semantic_base_type;
        script.external_base_name = input.external_base_name;
        script.attached_native_base = input.attached_native_base;
        script.global_name = input.globally_named ? input.script_class_name : "";
        if (input.script_base)
            script.base_script_path =
                "res://" + generic_path_to_utf8(inputs[*input.script_base].relative);
        script.reflection_members = input.members;
        script.is_abstract = input.is_abstract;
        script.is_tool = input.script.tool;
        script.static_unload = input.static_unload;
        script.is_attached = input.attached;
        script.is_editor_only = editor_only_inputs[input_index];
        const auto expected_class_name =
            "GDPPNative_" + input.native_class_stem + "_" + input.public_abi_hash.substr(0, 16);
        for (const auto& inner : input.inner_classes) {
            const auto native_name = expected_class_name + "__" + native_inner_suffix(inner.name);
            script.inner_class_names.push_back(native_name);
            if (inner.is_abstract)
                script.abstract_inner_class_names.push_back(native_name);
            bool editor_only = target_api.is_editor_class(inner.godot_base_type);
            if (!inner.base_script_path.empty()) {
                const auto base = script_paths.find(inner.base_script_path);
                if (base != script_paths.end()) {
                    editor_only = editor_only || editor_only_inputs[base->second];
                }
            }
            if (editor_only)
                script.editor_only_inner_class_names.push_back(native_name);
        }
        auto script_options = compiler_options;
        script_options.native_class_suffix = "_" + input.public_abi_hash.substr(0, 16);
        script_options.current_script_path = input.relative;
        script_options.semantic_base_type = input.semantic_base_type;
        script_options.attached_script = input.attached;
        script_options.attached_native_base = input.attached_native_base;
        script_options.script_contract_hash = input.public_abi_hash;
        if (input.script_base) {
            const auto& base = inputs[*input.script_base];
            const auto* base_symbol = script_symbols.find_path(base.relative);
            script_options.attached_base_script_path =
                "res://" + generic_path_to_utf8(base.relative);
            script_options.native_base_class = base_symbol ? base_symbol->native_class_name : "";
            script_options.native_base_header = base_symbol ? base_symbol->header_file_name : "";
        } else if (input.extension_base.type && !input.attached) {
            script_options.native_base_class = input.extension_base.type->cpp_type;
            script_options.native_base_header = input.extension_base.type->header;
        } else if (input.local_inner_base) {
            script_options.attached_base_script_path =
                "res://" + generic_path_to_utf8(input.relative) +
                "::" + input.inner_classes[*input.local_inner_base].name;
            script_options.native_base_class =
                "GDPPNative_" + input.native_class_stem + "_" +
                input.public_abi_hash.substr(0, 16) + "__" +
                native_inner_suffix(input.inner_classes[*input.local_inner_base].name);
            script_options.native_base_header.clear();
        }
        auto compilation = compiler.compile(input.relative, input.source, script_options);
        if (!compilation.success) {
            for (auto& diagnostic : compilation.diagnostics)
                result.diagnostics.push_back({input.path, std::move(diagnostic)});
            report_project_progress(options, ProjectCompilePhase::translate, ++translated_inputs,
                                    translation_total);
            continue;
        }
        script.class_name = compilation.unit.class_name;
        script.header_file_name = compilation.unit.header_file_name;
        script.source_file_name = compilation.unit.source_file_name;
        script.symbol_file_name = compilation.unit.symbol_file_name;
        script.inner_class_names = compilation.unit.inner_class_names;
        script.abstract_inner_class_names = compilation.unit.abstract_inner_class_names;
        script.is_abstract = compilation.unit.is_abstract;
        script.is_tool = compilation.unit.is_tool;
        script.static_unload = compilation.unit.static_unload;
        pending.push_back({result.scripts.size(), std::move(compilation.unit.header),
                           std::move(compilation.unit.source),
                           std::move(compilation.unit.symbol_map)});
        ++result.compiled_count;
        const auto [owner, unique_class] = class_owners.emplace(script.class_name, input.relative);
        if (!unique_class) {
            result.diagnostics.push_back(
                {input.path,
                 project_error("PRJ0005", "native class '" + script.class_name +
                                              "' is also produced by " + owner->second)});
        }
        if (!output_names.insert(script.header_file_name).second ||
            !output_names.insert(script.source_file_name).second ||
            !output_names.insert(script.symbol_file_name).second) {
            result.diagnostics.push_back(
                {input.path, project_error("PRJ0006", "generated file name collision")});
        }
        new_manifest.emplace(input.relative,
                             ManifestEntry{script.header_file_name, script.source_file_name,
                                           script.symbol_file_name});
        result.scripts.push_back(std::move(script));
        report_project_progress(options, ProjectCompilePhase::translate, ++translated_inputs,
                                translation_total);
    }
    if (compile_order.empty())
        report_project_progress(options, ProjectCompilePhase::translate, 1, 1);
    if (has_project_errors(result.diagnostics))
        return result;

    report_project_progress(options, ProjectCompilePhase::generate, 0, 1);
    std::filesystem::create_directories(generated, error);
    if (error) {
        result.diagnostics.push_back(
            {output, project_error("PRJ0007", "cannot create project build directories")});
        return result;
    }
    for (const auto& item : pending) {
        const auto& script = result.scripts[item.script_index];
        if (!write_file_if_changed(generated / script.header_file_name, item.header) ||
            !write_file_if_changed(generated / script.source_file_name, item.source) ||
            !write_file_if_changed(generated / script.symbol_file_name, item.symbols)) {
            result.diagnostics.push_back(
                {generated, project_error("PRJ0008", "cannot write generated translation unit")});
            return result;
        }
    }
    for (const auto& [path, entry] : old_manifest) {
        const auto replacement = new_manifest.find(path);
        if (replacement != new_manifest.end() && replacement->second.header == entry.header &&
            replacement->second.source == entry.source &&
            replacement->second.symbols == entry.symbols) {
            continue;
        }
        // A moved globally named script can keep the same generated file names. The new
        // translation unit is written before stale manifest entries are removed, so deleting by
        // old source path alone would also delete the freshly generated output. The same guard
        // also handles class-name changes that replace the outputs of an existing source path.
        if (!output_names.count(entry.header))
            std::filesystem::remove(generated / entry.header, error);
        if (!error && !output_names.count(entry.source))
            std::filesystem::remove(generated / entry.source, error);
        if (!error && !output_names.count(entry.symbols))
            std::filesystem::remove(generated / entry.symbols, error);
        if (error) {
            result.diagnostics.push_back(
                {generated,
                 project_error("PRJ0019", "cannot remove stale generated translation unit")});
            return result;
        }
        ++result.removed_count;
    }
    // The generated directory is compiler-owned. Reconcile it against the new manifest so files
    // orphaned by an older manifest schema, interrupted upgrade, or historical cleanup bug cannot
    // leak into the next native CMake source list.
    std::vector<std::filesystem::path> orphaned_outputs;
    for (std::filesystem::directory_iterator generated_iterator{generated, error}, generated_end;
         !error && generated_iterator != generated_end; generated_iterator.increment(error)) {
        std::error_code entry_error;
        if (!generated_iterator->is_regular_file(entry_error)) {
            if (entry_error)
                error = entry_error;
            continue;
        }
        const auto name = path_to_utf8(generated_iterator->path().filename());
        if ((managed_translation_unit_name(name) || managed_symbol_map_name(name)) &&
            !output_names.count(name))
            orphaned_outputs.push_back(generated_iterator->path());
    }
    for (const auto& orphan : orphaned_outputs) {
        if (error)
            break;
        std::filesystem::remove(orphan, error);
    }
    if (error) {
        result.diagnostics.push_back(
            {generated, project_error("PRJ0019", "cannot reconcile generated translation units")});
        return result;
    }

    // These files belonged to the retired CMake-driven project build. The direct native builder
    // no longer reads or generates them, but an in-place plug-in upgrade can leave them beside the
    // current manifest. Reconcile the compiler-owned output directory so an old descriptor cannot
    // advertise a retired entry symbol and old CMake scripts cannot be mistaken for active build
    // inputs by tooling or support diagnostics.
    constexpr std::array<std::string_view, 4> retired_scaffold{
        "CMakeLists.txt",
        "gdpp_project.gdextension",
        "patch_godot_cpp_class_db.cmake",
        "prune_stale_development.cmake",
    };
    for (const auto name : retired_scaffold) {
        std::filesystem::remove(output / name, error);
        if (error) {
            result.diagnostics.push_back(
                {output / name,
                 project_error("PRJ0022", "cannot remove retired project build scaffold")});
            return result;
        }
    }

    const auto relative_output = output.lexically_relative(root);
    const auto build_directory = containing_build_directory(root, relative_output);
    std::ostringstream symbol_index;
    symbol_index << "GDPP_PROJECT_SYMBOL_MAP 1\n"
                 << "build_id " << std::quoted(result.build_id) << '\n'
                 << "units " << result.scripts.size() << '\n';
    for (const auto& script : result.scripts) {
        symbol_index << "unit " << std::quoted(generic_path_to_utf8(script.relative_path)) << ' '
                     << std::quoted(script.class_name) << ' '
                     << std::quoted("generated/" + script.source_file_name) << ' '
                     << std::quoted("generated/" + script.symbol_file_name) << '\n';
    }
    if (!write_file_if_changed(output / "register_types.cpp",
                               generated_registration(result.scripts)) ||
        !write_file_if_changed(output / "symbols.map", symbol_index.str()) ||
        !write_file_if_changed(output / "build_id.txt", result.build_id + "\n") ||
        !write_file_if_changed(output / "bridge.lock",
                               write_extension_bridge_lock(bridge_load.bridges)) ||
        (build_directory &&
         !write_file_if_changed(*build_directory / ".gdignore",
                                "# Generated by GDPP. Keep native intermediates out of Godot's "
                                "resource scan.\n")) ||
        !write_file_if_changed(manifest_path, write_manifest(new_manifest))) {
        result.diagnostics.push_back(
            {output, project_error("PRJ0009", "cannot write project build scaffold")});
        return result;
    }

    result.success = true;
    result.native_library_directory = native_library_directory;
    report_project_progress(options, ProjectCompilePhase::generate, 1, 1);
    return result;
}

} // namespace gdpp
