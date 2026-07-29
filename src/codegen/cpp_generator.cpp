#include "gdpp/codegen/cpp_generator.hpp"
#include "gdpp/semantic/conversion.hpp"
#include "gdpp/semantic/godot_api.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace gdpp {
namespace {

std::string indent(std::size_t level) { return std::string(level * 4, ' '); }

const Type& emitted_type(const typed::Expression& expression) {
    // Local identifiers convert their declared storage into the flow-refined semantic type inside
    // emit_expression(). Dynamic call results are different: their wider container storage must
    // remain observable until an actual assignment, parameter, or return boundary.
    return expression.kind == typed::ExpressionKind::call &&
                   expression.storage_type.kind != TypeKind::unknown
               ? expression.storage_type
               : expression.type;
}

std::string indent_block(const std::size_t level, std::string_view block) {
    if (block.empty())
        return {};
    const auto prefix = indent(level);
    std::string result;
    result.reserve(block.size() + prefix.size() * 4U);
    result += prefix;
    for (std::size_t index = 0; index < block.size(); ++index) {
        result.push_back(block[index]);
        if (block[index] == '\n' && index + 1U < block.size())
            result += prefix;
    }
    if (result.back() != '\n')
        result.push_back('\n');
    return result;
}

bool contains_preload(const typed::Expression& expression) {
    if (expression.kind == typed::ExpressionKind::call && !expression.operands.empty()) {
        const auto& callee = *expression.operands.front();
        if (callee.resolution == typed::ResolutionKind::intrinsic &&
            callee.intrinsic == IntrinsicKind::preload) {
            return true;
        }
    }
    return std::any_of(expression.operands.begin(), expression.operands.end(),
                       [](const auto& operand) { return contains_preload(*operand); });
}

bool cached_preload_field(const typed::Field& field) {
    // Static fields and constants already own class-lifetime transactional storage. Only instance
    // fields need a separate class-level preload cache before their per-instance assignment.
    return !field.is_constant && !field.is_static && !field.onready && field.initializer &&
           contains_preload(*field.initializer);
}

bool requires_static_initialization(const std::vector<typed::Field>& fields,
                                    const std::vector<typed::Function>& functions) {
    return std::any_of(
               fields.begin(), fields.end(),
               [](const typed::Field& field) { return !field.is_constant && field.is_static; }) ||
           std::any_of(functions.begin(), functions.end(), [](const typed::Function& function) {
               return function.name == "_static_init";
           });
}

bool managed_static_constant(const Type& type) {
    // Even scalar GDScript constants may use constant-foldable Godot utility calls such as
    // deg_to_rad(). A namespace-scope C++ initializer would execute those calls from DllMain,
    // before godot-cpp receives the GDExtension interface, and make LoadLibrary fail on Windows.
    // Keep every script constant behind function-local storage so initialization cannot run
    // before the extension entry point has completed.
    (void)type;
    return true;
}

bool editor_safe_initializer(const typed::Expression& expression) {
    const auto all_operands_are_safe = [&expression](std::size_t begin = 0) {
        return std::all_of(expression.operands.begin() + static_cast<std::ptrdiff_t>(begin),
                           expression.operands.end(),
                           [](const auto& operand) { return editor_safe_initializer(*operand); });
    };

    switch (expression.kind) {
    case typed::ExpressionKind::literal:
        return true;
    case typed::ExpressionKind::identifier:
        return expression.resolution == typed::ResolutionKind::godot_type ||
               expression.resolution == typed::ResolutionKind::global_constant ||
               expression.resolution == typed::ResolutionKind::global_enum_type ||
               expression.resolution == typed::ResolutionKind::global_enum_value ||
               expression.resolution == typed::ResolutionKind::builtin_constant ||
               expression.resolution == typed::ResolutionKind::enum_member;
    case typed::ExpressionKind::unary:
    case typed::ExpressionKind::await_expression:
    case typed::ExpressionKind::binary:
    case typed::ExpressionKind::subscript:
    case typed::ExpressionKind::conditional:
    case typed::ExpressionKind::array_literal:
    case typed::ExpressionKind::dictionary_literal:
        return all_operands_are_safe();
    case typed::ExpressionKind::member:
        return (expression.resolution == typed::ResolutionKind::global_constant ||
                expression.resolution == typed::ResolutionKind::global_enum_type ||
                expression.resolution == typed::ResolutionKind::global_enum_value ||
                expression.resolution == typed::ResolutionKind::builtin_constant ||
                expression.resolution == typed::ResolutionKind::enum_member) &&
               all_operands_are_safe();
    case typed::ExpressionKind::call:
        // Value-type constructors such as Vector2(...) and Color(...) are pure.
        // Object constructors, utility functions, methods, preload/load, and
        // customer callables may touch the SceneTree or external services.
        return !expression.operands.empty() &&
               expression.operands.front()->resolution ==
                   typed::ResolutionKind::godot_constructor &&
               expression.type.kind != TypeKind::object && all_operands_are_safe(1);
    case typed::ExpressionKind::node_reference:
    case typed::ExpressionKind::lambda:
        return false;
    }
    return false;
}

const typed::Expression* range_call(const typed::Expression& expression) {
    if (expression.kind != typed::ExpressionKind::call || expression.operands.size() < 2 ||
        expression.operands.size() > 4) {
        return nullptr;
    }
    const auto& callee = *expression.operands.front();
    return callee.resolution == typed::ResolutionKind::intrinsic &&
                   callee.intrinsic == IntrinsicKind::range
               ? &expression
               : nullptr;
}

bool requires_native_fallback(const std::vector<typed::Statement>& statements) {
    // Match currently lowers to guarded branches plus a matched sentinel. Even an
    // exhaustive catch-all is therefore not provably exhaustive to a C++ compiler.
    // Semantic analysis has already guaranteed that this fallback is unreachable.
    return !statements.empty() && statements.back().kind == typed::StatementKind::match_statement;
}

bool contains_current_loop_break(const std::vector<typed::Statement>& statements) {
    for (const auto& statement : statements) {
        if (statement.kind == typed::StatementKind::break_statement)
            return true;
        if (statement.kind == typed::StatementKind::while_statement ||
            statement.kind == typed::StatementKind::for_statement)
            continue;
        if (contains_current_loop_break(statement.body) ||
            contains_current_loop_break(statement.else_body))
            return true;
    }
    return false;
}

bool contains_current_loop_control(const typed::Statement& statement) {
    if (statement.kind == typed::StatementKind::break_statement ||
        statement.kind == typed::StatementKind::continue_statement) {
        return true;
    }
    if (statement.kind == typed::StatementKind::while_statement ||
        statement.kind == typed::StatementKind::for_statement) {
        return false;
    }
    return std::any_of(statement.body.begin(), statement.body.end(),
                       contains_current_loop_control) ||
           std::any_of(statement.else_body.begin(), statement.else_body.end(),
                       contains_current_loop_control) ||
           std::any_of(statement.guard_prefix.begin(), statement.guard_prefix.end(),
                       contains_current_loop_control) ||
           std::any_of(statement.assert_condition_prefix.begin(),
                       statement.assert_condition_prefix.end(), contains_current_loop_control) ||
           std::any_of(statement.assert_message_prefix.begin(),
                       statement.assert_message_prefix.end(), contains_current_loop_control);
}

struct AsyncLocalSymbol final {
    std::string name;
    Type type;
};

using DictionarySlotProofs = std::unordered_map<FlowSymbolId, std::unordered_set<std::string>>;

void collect_local_dictionary_candidates(const std::vector<typed::Statement>& statements,
                                         DictionarySlotProofs& candidates) {
    for (const auto& statement : statements) {
        if (statement.kind == typed::StatementKind::variable && !statement.is_constant &&
            statement.symbol_identity != 0 &&
            statement.declared_type.kind == TypeKind::dictionary &&
            statement.declared_type.name == "Dictionary" && statement.expression &&
            statement.expression->kind == typed::ExpressionKind::dictionary_literal &&
            statement.expression->type.kind == TypeKind::dictionary &&
            statement.expression->type.name == "Dictionary") {
            auto& keys = candidates[statement.symbol_identity];
            for (std::size_t index = 0; index + 1 < statement.expression->operands.size();
                 index += 2) {
                if (!statement.expression->operands[index])
                    continue;
                const auto& key = *statement.expression->operands[index];
                if (key.kind == typed::ExpressionKind::literal &&
                    (key.literal_kind == typed::LiteralKind::string ||
                     key.literal_kind == typed::LiteralKind::string_name)) {
                    keys.insert(key.value);
                }
            }
        }
        collect_local_dictionary_candidates(statement.body, candidates);
        collect_local_dictionary_candidates(statement.else_body, candidates);
        collect_local_dictionary_candidates(statement.guard_prefix, candidates);
        collect_local_dictionary_candidates(statement.assert_condition_prefix, candidates);
        collect_local_dictionary_candidates(statement.assert_message_prefix, candidates);
    }
}

void audit_local_dictionary_statements(const std::vector<typed::Statement>& statements,
                                       DictionarySlotProofs& candidates,
                                       bool allow_named_receiver = true);

void audit_local_dictionary_expression(const typed::Expression& expression,
                                       DictionarySlotProofs& candidates,
                                       const bool allowed_named_receiver = false,
                                       const bool allow_named_receiver = true) {
    if (expression.kind == typed::ExpressionKind::identifier &&
        candidates.find(expression.symbol_identity) != candidates.end() &&
        !allowed_named_receiver) {
        candidates.erase(expression.symbol_identity);
    }

    if (expression.kind == typed::ExpressionKind::lambda && expression.lambda) {
        // A captured Dictionary can outlive this function or be invoked re-entrantly. Keep
        // closure lowering on the general checked path even when the current body only performs
        // named access.
        audit_local_dictionary_statements(expression.lambda->body, candidates, false);
        return;
    }

    const bool named_member = allow_named_receiver &&
                              expression.kind == typed::ExpressionKind::member &&
                              expression.resolution == typed::ResolutionKind::dynamic_property &&
                              !expression.operands.empty();
    for (std::size_t index = 0; index < expression.operands.size(); ++index) {
        if (!expression.operands[index])
            continue;
        audit_local_dictionary_expression(*expression.operands[index], candidates,
                                          named_member && index == 0, allow_named_receiver);
    }
}

void audit_local_dictionary_pattern(const typed::MatchPattern& pattern,
                                    DictionarySlotProofs& candidates,
                                    const bool allow_named_receiver) {
    if (pattern.expression)
        audit_local_dictionary_expression(*pattern.expression, candidates, false,
                                          allow_named_receiver);
    for (const auto& key : pattern.keys)
        if (key)
            audit_local_dictionary_expression(*key, candidates, false, allow_named_receiver);
    for (const auto& child : pattern.elements)
        audit_local_dictionary_pattern(child, candidates, allow_named_receiver);
}

void audit_local_dictionary_statements(const std::vector<typed::Statement>& statements,
                                       DictionarySlotProofs& candidates,
                                       const bool allow_named_receiver) {
    for (const auto& statement : statements) {
        if (statement.expression)
            audit_local_dictionary_expression(*statement.expression, candidates, false,
                                              allow_named_receiver);
        if (statement.condition)
            audit_local_dictionary_expression(*statement.condition, candidates, false,
                                              allow_named_receiver);
        for (const auto& pattern : statement.patterns)
            audit_local_dictionary_pattern(pattern, candidates, allow_named_receiver);
        audit_local_dictionary_statements(statement.body, candidates, allow_named_receiver);
        audit_local_dictionary_statements(statement.else_body, candidates, allow_named_receiver);
        audit_local_dictionary_statements(statement.guard_prefix, candidates, allow_named_receiver);
        audit_local_dictionary_statements(statement.assert_condition_prefix, candidates,
                                          allow_named_receiver);
        audit_local_dictionary_statements(statement.assert_message_prefix, candidates,
                                          allow_named_receiver);
    }
}

DictionarySlotProofs prove_local_dictionary_slots(const std::vector<typed::Statement>& statements) {
    DictionarySlotProofs candidates;
    collect_local_dictionary_candidates(statements, candidates);
    audit_local_dictionary_statements(statements, candidates);
    return candidates;
}

void collect_match_declarations(const typed::MatchPattern& pattern,
                                std::unordered_set<FlowSymbolId>& symbols) {
    if (pattern.kind == typed::MatchPatternKind::binding && pattern.symbol_identity != 0)
        symbols.insert(pattern.symbol_identity);
    for (const auto& child : pattern.elements)
        collect_match_declarations(child, symbols);
}

void collect_declared_symbols(const std::vector<typed::Statement>& statements,
                              std::unordered_set<FlowSymbolId>& symbols) {
    for (const auto& statement : statements) {
        if (statement.kind == typed::StatementKind::variable ||
            statement.kind == typed::StatementKind::await_variable ||
            statement.kind == typed::StatementKind::for_statement) {
            if (statement.symbol_identity != 0)
                symbols.insert(statement.symbol_identity);
        }
        for (const auto& pattern : statement.patterns)
            collect_match_declarations(pattern, symbols);
        collect_declared_symbols(statement.body, symbols);
        collect_declared_symbols(statement.else_body, symbols);
        collect_declared_symbols(statement.guard_prefix, symbols);
        collect_declared_symbols(statement.assert_condition_prefix, symbols);
        collect_declared_symbols(statement.assert_message_prefix, symbols);
    }
}

void collect_assigned_symbols(const std::vector<typed::Statement>& statements,
                              std::unordered_map<FlowSymbolId, AsyncLocalSymbol>& symbols) {
    for (const auto& statement : statements) {
        if (statement.kind == typed::StatementKind::assignment && statement.condition &&
            statement.condition->kind == typed::ExpressionKind::identifier &&
            statement.condition->resolution == typed::ResolutionKind::none &&
            statement.condition->symbol_identity != 0) {
            const auto& target = *statement.condition;
            symbols.try_emplace(target.symbol_identity,
                                AsyncLocalSymbol{
                                    target.value,
                                    target.storage_type.kind == TypeKind::unknown
                                        ? target.type
                                        : target.storage_type,
                                });
        }
        collect_assigned_symbols(statement.body, symbols);
        collect_assigned_symbols(statement.else_body, symbols);
        collect_assigned_symbols(statement.guard_prefix, symbols);
        collect_assigned_symbols(statement.assert_condition_prefix, symbols);
        collect_assigned_symbols(statement.assert_message_prefix, symbols);
    }
}

bool native_statements_fall_through(const std::vector<typed::Statement>& statements);

bool flat_async_statement_supported(const typed::Statement& statement) {
    switch (statement.kind) {
    case typed::StatementKind::expression:
    case typed::StatementKind::assignment:
    case typed::StatementKind::await_statement:
    case typed::StatementKind::pass_statement:
    case typed::StatementKind::breakpoint_statement:
        break;
    case typed::StatementKind::assert_statement:
        return statement.assert_condition_prefix.empty() && statement.assert_message_prefix.empty();
    case typed::StatementKind::if_statement:
        return std::all_of(statement.body.begin(), statement.body.end(),
                           flat_async_statement_supported) &&
               std::all_of(statement.else_body.begin(), statement.else_body.end(),
                           flat_async_statement_supported);
    case typed::StatementKind::return_statement:
    case typed::StatementKind::await_variable:
    case typed::StatementKind::variable:
    case typed::StatementKind::for_statement:
    case typed::StatementKind::while_statement:
    case typed::StatementKind::match_statement:
    case typed::StatementKind::match_branch:
    case typed::StatementKind::break_statement:
    case typed::StatementKind::continue_statement:
        return false;
    }
    return statement.body.empty() && statement.else_body.empty();
}

bool native_statement_falls_through(const typed::Statement& statement) {
    switch (statement.kind) {
    case typed::StatementKind::return_statement:
    case typed::StatementKind::break_statement:
    case typed::StatementKind::continue_statement:
        return false;
    case typed::StatementKind::if_statement:
        return statement.else_body.empty() || native_statements_fall_through(statement.body) ||
               native_statements_fall_through(statement.else_body);
    case typed::StatementKind::match_statement:
        // The sentinel-based native lowering is conservatively fall-through to C++.
        return true;
    case typed::StatementKind::while_statement:
        if (statement.condition && statement.condition->kind == typed::ExpressionKind::literal &&
            statement.condition->literal_kind == typed::LiteralKind::boolean &&
            statement.condition->value == "true" && !contains_current_loop_break(statement.body))
            return false;
        return true;
    default:
        return true;
    }
}

bool native_statements_fall_through(const std::vector<typed::Statement>& statements) {
    bool falls_through = true;
    for (const auto& statement : statements) {
        if (!falls_through)
            return false;
        falls_through = native_statement_falls_through(statement);
    }
    return falls_through;
}

std::string escaped_string(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\a':
            result += "\\a";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\v':
            result += "\\v";
            break;
        default:
            if (const auto byte = static_cast<unsigned char>(character);
                byte < 0x20U || byte == 0x7fU) {
                result.push_back('\\');
                result.push_back(static_cast<char>('0' + ((byte >> 6U) & 0x07U)));
                result.push_back(static_cast<char>('0' + ((byte >> 3U) & 0x07U)));
                result.push_back(static_cast<char>('0' + (byte & 0x07U)));
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

bool is_ascii(std::string_view value) {
    return std::all_of(value.begin(), value.end(),
                       [](const char byte) { return static_cast<unsigned char>(byte) < 0x80; });
}

// godot-cpp's const char* constructors use Latin-1 for compatibility. Keep the
// compact form for ASCII, but explicitly decode UTF-8 whenever customer source
// contains non-ASCII text (node names, translations, methods, signals, etc.).
std::string godot_text_argument(const std::string& value) {
    if (is_ascii(value))
        return escaped_string(value);
    return "godot::String::utf8(" + escaped_string(value) + ")";
}

std::string godot_string(const std::string& value) {
    if (is_ascii(value))
        return "godot::String(" + escaped_string(value) + ")";
    return "godot::String::utf8(" + escaped_string(value) + ")";
}

std::string godot_string_name(const std::string& value) {
    return "godot::StringName(" + godot_text_argument(value) + ")";
}

bool has_rpc_configuration(const std::vector<typed::Function>& functions) {
    return std::any_of(functions.begin(), functions.end(),
                       [](const typed::Function& function) { return function.rpc.has_value(); });
}

bool has_rpc_configuration(const typed::Class& declaration) {
    return has_rpc_configuration(declaration.functions) ||
           std::any_of(declaration.classes.begin(), declaration.classes.end(),
                       [](const typed::Class& nested) { return has_rpc_configuration(nested); });
}

bool has_rpc_configuration(const typed::Module& module) {
    return has_rpc_configuration(module.functions) ||
           std::any_of(module.classes.begin(), module.classes.end(),
                       [](const typed::Class& nested) { return has_rpc_configuration(nested); });
}

void emit_rpc_configurations(std::ostringstream& source,
                             const std::vector<typed::Function>& functions,
                             const std::size_t indentation) {
    const auto prefix = indent(indentation);
    for (const auto& function : functions) {
        if (!function.rpc)
            continue;
        const auto& rpc = *function.rpc;
        source << prefix << "{\n"
               << prefix << "    godot::Dictionary gdpp_rpc_config;\n"
               << prefix << "    gdpp_rpc_config[\"rpc_mode\"] = godot::MultiplayerAPI::"
               << (rpc.permission == RpcPermission::any_peer ? "RPC_MODE_ANY_PEER"
                                                             : "RPC_MODE_AUTHORITY")
               << ";\n";
        if (rpc.transfer_mode_explicit) {
            source << prefix << "    gdpp_rpc_config[\"transfer_mode\"] = godot::MultiplayerPeer::";
            switch (rpc.transfer_mode) {
            case RpcTransferMode::unreliable:
                source << "TRANSFER_MODE_UNRELIABLE";
                break;
            case RpcTransferMode::unreliable_ordered:
                source << "TRANSFER_MODE_UNRELIABLE_ORDERED";
                break;
            case RpcTransferMode::reliable:
                source << "TRANSFER_MODE_RELIABLE";
                break;
            }
            source << ";\n";
        }
        if (rpc.call_local_explicit)
            source << prefix << "    gdpp_rpc_config[\"call_local\"] = "
                   << (rpc.call_local ? "true" : "false") << ";\n";
        if (rpc.channel_explicit)
            source << prefix << "    gdpp_rpc_config[\"channel\"] = int64_t{" << rpc.channel
                   << "};\n";
        source << prefix << "    rpc_config(" << godot_string_name(function.name)
               << ", gdpp_rpc_config);\n"
               << prefix << "}\n";
    }
}

std::string godot_node_path(const std::string& value) {
    return "godot::NodePath(" + godot_text_argument(value) + ")";
}

std::string builtin_constant_expression(std::string value) {
    std::size_t position = 0;
    while ((position = value.find("inf", position)) != std::string::npos) {
        value.replace(position, 3, "Math_INF");
        position += 8;
    }
    return "godot::" + value;
}

std::string to_snake_case(std::string_view value) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (std::isupper(byte) != 0) {
            const bool previous_is_lower =
                index > 0 && std::islower(static_cast<unsigned char>(value[index - 1])) != 0;
            const bool previous_is_digit =
                index > 0 && std::isdigit(static_cast<unsigned char>(value[index - 1])) != 0;
            const bool next_starts_word =
                index > 0 && index + 1 < value.size() &&
                std::islower(static_cast<unsigned char>(value[index + 1])) != 0;
            if (!result.empty() && result.back() != '_' &&
                (previous_is_lower || previous_is_digit || next_starts_word))
                result.push_back('_');
            result.push_back(static_cast<char>(std::tolower(byte)));
        } else if (std::isalnum(byte) != 0 || value[index] == '_') {
            result.push_back(value[index]);
        } else {
            result.push_back('_');
        }
    }
    for (const std::string_view dimension : {"2_d", "3_d"}) {
        std::size_t position = 0;
        while ((position = result.find(dimension, position)) != std::string::npos)
            result.erase(position + 1, 1);
    }
    return result.empty() ? "generated_script" : result;
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

std::string godot_cpp_class_name(const std::string_view name) {
    // godot-cpp owns godot::ClassDB for extension registration. The engine singleton therefore
    // uses the generated ClassDBSingleton wrapper even though GDScript exposes it as ClassDB.
    if (name == "ClassDB")
        return "ClassDBSingleton";
    return std::string{name};
}

std::string header_for_base(const std::string& base) {
    if (base == "ClassDB")
        return "godot_cpp/classes/class_db_singleton.hpp";
    if (base == "Object")
        return "godot_cpp/core/object.hpp";
    return "godot_cpp/classes/" + to_snake_case(base) + ".hpp";
}

struct NativeTypeIncludes {
    std::set<std::string> builtins;
    std::set<std::string> objects;
    std::set<std::string> scripts;
    std::set<std::string> script_resources;
    std::set<std::string> complete_scripts;
    std::set<std::string> complete_script_resources;
    // Script display names are not identities: unnamed scripts can share a stem and project
    // globals can shadow engine types. Resolved native names remain unique across the project.
    std::set<std::string> resolved_native_scripts;
    std::set<std::string> container_objects;
    bool typed_array{false};
    bool typed_dictionary{false};
};

void collect_type(const Type& type, NativeTypeIncludes& includes, const GodotApi& api,
                  const ScriptSymbolTable* script_symbols) {
    if (const auto container = describe_container_type(type)) {
        includes.typed_array = includes.typed_array || container->kind == ContainerTypeKind::array;
        includes.typed_dictionary =
            includes.typed_dictionary || container->kind == ContainerTypeKind::dictionary;
        for (const auto& argument : container->arguments) {
            const auto argument_type = type_from_annotation(argument);
            if (argument_type.kind == TypeKind::object) {
                includes.container_objects.insert(argument);
            } else {
                collect_type(argument_type, includes, api, script_symbols);
            }
        }
    }
    if (type.kind == TypeKind::builtin)
        includes.builtins.insert(type.name);
    else if (type.kind == TypeKind::enumeration && script_symbols) {
        const auto separator = type.name.rfind("::");
        if (separator != std::string::npos) {
            const auto native_owner = type.name.substr(0, separator);
            if (script_symbols->find_native_class(native_owner)) {
                includes.resolved_native_scripts.insert(native_owner);
            } else if (const auto* inner = script_symbols->find_inner_native(native_owner)) {
                if (const auto* owner = script_symbols->owner_of(*inner))
                    includes.resolved_native_scripts.insert(owner->native_class_name);
            }
        }
    } else if (type.kind == TypeKind::object && api.find_class(type.name))
        includes.objects.insert(type.name);
    else if (type.kind == TypeKind::object && script_symbols &&
             script_symbols->find_class(type.name))
        includes.scripts.insert(type.name);
    else if (type.kind == TypeKind::object && script_symbols &&
             script_symbols->find_native_class(type.name))
        includes.resolved_native_scripts.insert(type.name);
    else if (type.kind == TypeKind::object && script_symbols) {
        if (const auto* inner = script_symbols->find_inner_native(type.name)) {
            if (const auto* owner = script_symbols->owner_of(*inner))
                includes.complete_script_resources.insert(owner->path);
        }
    } else if (type.kind == TypeKind::script_resource && script_symbols &&
               script_symbols->find_path(type.name))
        includes.script_resources.insert(type.name);
}

void collect_statement_types(const typed::Statement& statement, NativeTypeIncludes& includes,
                             const GodotApi& api, const ScriptSymbolTable* script_symbols);

void collect_header_statement_dependencies(const typed::Statement& statement,
                                           NativeTypeIncludes& includes);
void collect_header_expression_dependencies(const typed::Expression& expression,
                                            NativeTypeIncludes& includes);
void collect_expression_types(const typed::Expression& expression, NativeTypeIncludes& includes,
                              const GodotApi& api, const ScriptSymbolTable* script_symbols);

void collect_header_match_pattern_dependencies(const typed::MatchPattern& pattern,
                                               NativeTypeIncludes& includes) {
    if (pattern.expression)
        collect_header_expression_dependencies(*pattern.expression, includes);
    for (const auto& key : pattern.keys) {
        if (key)
            collect_header_expression_dependencies(*key, includes);
    }
    for (const auto& element : pattern.elements)
        collect_header_match_pattern_dependencies(element, includes);
}

void collect_header_expression_dependencies(const typed::Expression& expression,
                                            NativeTypeIncludes& includes) {
    if (expression.resolution == typed::ResolutionKind::script_type &&
        expression.type.kind == TypeKind::object) {
        includes.complete_scripts.insert(expression.type.name);
    }
    if (expression.resolution == typed::ResolutionKind::script_resource &&
        expression.type.kind == TypeKind::script_resource) {
        includes.complete_script_resources.insert(expression.type.name);
    }
    for (const auto& operand : expression.operands)
        collect_header_expression_dependencies(*operand, includes);
    if (expression.lambda) {
        for (const auto& parameter : expression.lambda->parameters) {
            for (const auto& statement : parameter.default_prefix)
                collect_header_statement_dependencies(statement, includes);
            if (parameter.default_value)
                collect_header_expression_dependencies(*parameter.default_value, includes);
        }
        for (const auto& statement : expression.lambda->body)
            collect_header_statement_dependencies(statement, includes);
    }
}

void collect_header_statement_dependencies(const typed::Statement& statement,
                                           NativeTypeIncludes& includes) {
    if (statement.expression)
        collect_header_expression_dependencies(*statement.expression, includes);
    if (statement.condition)
        collect_header_expression_dependencies(*statement.condition, includes);
    for (const auto& child : statement.body)
        collect_header_statement_dependencies(child, includes);
    for (const auto& child : statement.else_body)
        collect_header_statement_dependencies(child, includes);
    for (const auto& child : statement.guard_prefix)
        collect_header_statement_dependencies(child, includes);
    for (const auto& child : statement.assert_condition_prefix)
        collect_header_statement_dependencies(child, includes);
    for (const auto& child : statement.assert_message_prefix)
        collect_header_statement_dependencies(child, includes);
    for (const auto& pattern : statement.patterns)
        collect_header_match_pattern_dependencies(pattern, includes);
}

void collect_match_pattern_types(const typed::MatchPattern& pattern, NativeTypeIncludes& includes,
                                 const GodotApi& api, const ScriptSymbolTable* script_symbols) {
    collect_type(pattern.type, includes, api, script_symbols);
    if (pattern.expression)
        collect_expression_types(*pattern.expression, includes, api, script_symbols);
    for (const auto& key : pattern.keys) {
        if (key)
            collect_expression_types(*key, includes, api, script_symbols);
    }
    for (const auto& element : pattern.elements)
        collect_match_pattern_types(element, includes, api, script_symbols);
}

void collect_expression_types(const typed::Expression& expression, NativeTypeIncludes& includes,
                              const GodotApi& api, const ScriptSymbolTable* script_symbols) {
    collect_type(expression.type, includes, api, script_symbols);
    collect_type(expression.storage_type, includes, api, script_symbols);
    if (expression.call_contract) {
        for (const auto& parameter : expression.call_contract->parameters)
            collect_type(parameter, includes, api, script_symbols);
    }
    // A GDScript property can intentionally publish a broader type than its native getter. For
    // example, SceneTree.root is a Node property while godot-cpp returns Window*. Include the
    // concrete getter result so C++ can see the inheritance needed for the implicit upcast.
    if (expression.resolution == typed::ResolutionKind::godot_property &&
        !expression.getter.empty()) {
        if (const auto* getter = api.find_method(expression.resolved_owner, expression.getter)) {
            if (std::string_view{getter->return_type}.size() != 0) {
                collect_type(type_from_godot_api(getter->return_type), includes, api,
                             script_symbols);
            }
        }
    }
    if ((expression.resolution == typed::ResolutionKind::script_autoload ||
         expression.resolution == typed::ResolutionKind::script_type) &&
        !expression.resolved_owner.empty()) {
        includes.resolved_native_scripts.insert(expression.resolved_owner);
    }
    if (expression.kind == typed::ExpressionKind::call && !expression.operands.empty()) {
        const auto& callee = *expression.operands.front();
        if (callee.resolution == typed::ResolutionKind::godot_method) {
            if (const auto* method = api.find_method(callee.resolved_owner, callee.value)) {
                const auto provided = expression.operands.size() - 1;
                const auto count =
                    std::min(provided, static_cast<std::size_t>(method->maximum_arguments));
                for (std::size_t index = 0; index < count; ++index) {
                    if (const auto* argument = api.argument(*method, index))
                        collect_type(type_from_godot_api(argument->type), includes, api,
                                     script_symbols);
                }
            }
        }
    }
    for (const auto& operand : expression.operands)
        collect_expression_types(*operand, includes, api, script_symbols);
    if (expression.lambda) {
        collect_type(expression.lambda->return_type, includes, api, script_symbols);
        for (const auto& parameter : expression.lambda->parameters) {
            collect_type(parameter.type, includes, api, script_symbols);
            for (const auto& statement : parameter.default_prefix)
                collect_statement_types(statement, includes, api, script_symbols);
            if (parameter.default_value)
                collect_expression_types(*parameter.default_value, includes, api, script_symbols);
        }
        if (expression.lambda->rest_parameter)
            collect_type(expression.lambda->rest_parameter->type, includes, api, script_symbols);
        for (const auto& statement : expression.lambda->body)
            collect_statement_types(statement, includes, api, script_symbols);
    }
}

void collect_statement_types(const typed::Statement& statement, NativeTypeIncludes& includes,
                             const GodotApi& api, const ScriptSymbolTable* script_symbols) {
    collect_type(statement.declared_type, includes, api, script_symbols);
    if (statement.expression)
        collect_expression_types(*statement.expression, includes, api, script_symbols);
    if (statement.condition)
        collect_expression_types(*statement.condition, includes, api, script_symbols);
    for (const auto& child : statement.body)
        collect_statement_types(child, includes, api, script_symbols);
    for (const auto& child : statement.else_body)
        collect_statement_types(child, includes, api, script_symbols);
    for (const auto& child : statement.guard_prefix)
        collect_statement_types(child, includes, api, script_symbols);
    for (const auto& child : statement.assert_condition_prefix)
        collect_statement_types(child, includes, api, script_symbols);
    for (const auto& child : statement.assert_message_prefix)
        collect_statement_types(child, includes, api, script_symbols);
    for (const auto& pattern : statement.patterns)
        collect_match_pattern_types(pattern, includes, api, script_symbols);
}

void collect_class_types(const typed::Class& declaration, NativeTypeIncludes& includes,
                         const GodotApi& api, const ScriptSymbolTable* script_symbols) {
    collect_type({TypeKind::object, declaration.base_type}, includes, api, script_symbols);
    for (const auto& field : declaration.fields) {
        collect_type(field.type, includes, api, script_symbols);
        if (field.initializer)
            collect_expression_types(*field.initializer, includes, api, script_symbols);
        if (field.getter) {
            for (const auto& statement : field.getter->body)
                collect_statement_types(statement, includes, api, script_symbols);
        }
        if (field.setter) {
            for (const auto& statement : field.setter->body)
                collect_statement_types(statement, includes, api, script_symbols);
        }
    }
    for (const auto& function : declaration.functions) {
        collect_type(function.return_type, includes, api, script_symbols);
        for (const auto& parameter : function.parameters)
            collect_type(parameter.type, includes, api, script_symbols);
        for (const auto& parameter : function.parameters) {
            for (const auto& statement : parameter.default_prefix) {
                collect_header_statement_dependencies(statement, includes);
                collect_statement_types(statement, includes, api, script_symbols);
            }
            if (parameter.default_value) {
                collect_header_expression_dependencies(*parameter.default_value, includes);
                collect_expression_types(*parameter.default_value, includes, api, script_symbols);
            }
        }
        for (const auto& statement : function.body)
            collect_statement_types(statement, includes, api, script_symbols);
    }
    for (const auto& inner : declaration.classes)
        collect_class_types(inner, includes, api, script_symbols);
}

NativeTypeIncludes collect_native_types(const typed::Module& module, const GodotApi& api,
                                        const ScriptSymbolTable* script_symbols) {
    NativeTypeIncludes includes;
    for (const auto& field : module.fields) {
        collect_type(field.type, includes, api, script_symbols);
        if (field.initializer)
            collect_expression_types(*field.initializer, includes, api, script_symbols);
        if (field.getter) {
            for (const auto& statement : field.getter->body)
                collect_statement_types(statement, includes, api, script_symbols);
        }
        if (field.setter) {
            for (const auto& statement : field.setter->body)
                collect_statement_types(statement, includes, api, script_symbols);
        }
    }
    for (const auto& signal : module.signals) {
        for (const auto& parameter : signal.parameters)
            collect_type(parameter.type, includes, api, script_symbols);
    }
    for (const auto& function : module.functions) {
        collect_type(function.return_type, includes, api, script_symbols);
        for (const auto& parameter : function.parameters)
            collect_type(parameter.type, includes, api, script_symbols);
        for (const auto& parameter : function.parameters) {
            for (const auto& statement : parameter.default_prefix) {
                collect_header_statement_dependencies(statement, includes);
                collect_statement_types(statement, includes, api, script_symbols);
            }
            if (parameter.default_value)
                collect_header_expression_dependencies(*parameter.default_value, includes);
        }
        for (const auto& statement : function.body)
            collect_statement_types(statement, includes, api, script_symbols);
    }
    for (const auto& declaration : module.classes)
        collect_class_types(declaration, includes, api, script_symbols);
    return includes;
}

std::string variant_type(const Type& type) {
    switch (type.kind) {
    case TypeKind::boolean:
        return "godot::Variant::BOOL";
    case TypeKind::integer:
        return "godot::Variant::INT";
    case TypeKind::floating:
        return "godot::Variant::FLOAT";
    case TypeKind::string:
        return "godot::Variant::STRING";
    case TypeKind::string_name:
        return "godot::Variant::STRING_NAME";
    case TypeKind::array:
        return "godot::Variant::ARRAY";
    case TypeKind::dictionary:
        return "godot::Variant::DICTIONARY";
    case TypeKind::enumeration:
        return "godot::Variant::INT";
    case TypeKind::script_resource:
        return "godot::Variant::OBJECT";
    case TypeKind::builtin: {
        static const std::unordered_map<std::string, std::string> builtin_types{
            {"Vector2", "VECTOR2"},
            {"Vector2i", "VECTOR2I"},
            {"Rect2", "RECT2"},
            {"Rect2i", "RECT2I"},
            {"Vector3", "VECTOR3"},
            {"Vector3i", "VECTOR3I"},
            {"Transform2D", "TRANSFORM2D"},
            {"Vector4", "VECTOR4"},
            {"Vector4i", "VECTOR4I"},
            {"Plane", "PLANE"},
            {"Quaternion", "QUATERNION"},
            {"AABB", "AABB"},
            {"Basis", "BASIS"},
            {"Transform3D", "TRANSFORM3D"},
            {"Projection", "PROJECTION"},
            {"Color", "COLOR"},
            {"NodePath", "NODE_PATH"},
            {"RID", "RID"},
            {"Callable", "CALLABLE"},
            {"Signal", "SIGNAL"},
            {"PackedByteArray", "PACKED_BYTE_ARRAY"},
            {"PackedInt32Array", "PACKED_INT32_ARRAY"},
            {"PackedInt64Array", "PACKED_INT64_ARRAY"},
            {"PackedFloat32Array", "PACKED_FLOAT32_ARRAY"},
            {"PackedFloat64Array", "PACKED_FLOAT64_ARRAY"},
            {"PackedStringArray", "PACKED_STRING_ARRAY"},
            {"PackedVector2Array", "PACKED_VECTOR2_ARRAY"},
            {"PackedVector3Array", "PACKED_VECTOR3_ARRAY"},
            {"PackedColorArray", "PACKED_COLOR_ARRAY"},
            {"PackedVector4Array", "PACKED_VECTOR4_ARRAY"},
        };
        if (const auto found = builtin_types.find(type.name); found != builtin_types.end())
            return "godot::Variant::" + found->second;
        return "godot::Variant::NIL";
    }
    case TypeKind::object:
        return "godot::Variant::OBJECT";
    default:
        return "godot::Variant::NIL";
    }
}

std::string property_hint(const typed::Field& field, const GodotApi& api,
                          const ScriptSymbolTable* script_symbols) {
    if (!field.property)
        return {};
    if (field.property_type.kind == TypeKind::enumeration) {
        if (script_symbols) {
            if (const auto separator = field.property_type.name.rfind("::");
                separator != std::string::npos) {
                const auto owner_name = field.property_type.name.substr(0, separator);
                const auto enum_name = field.property_type.name.substr(separator + 2);
                if (const auto* owner = script_symbols->find_native_class(owner_name)) {
                    if (const auto* enumeration = script_symbols->find_enum(*owner, enum_name);
                        enumeration && enumeration->is_bitfield) {
                        return "godot::PROPERTY_HINT_FLAGS";
                    }
                } else if (const auto* inner = script_symbols->find_inner_native(owner_name)) {
                    const auto enumeration = std::find_if(inner->enums.begin(), inner->enums.end(),
                                                          [&](const ScriptEnumSymbol& candidate) {
                                                              return candidate.name == enum_name;
                                                          });
                    if (enumeration != inner->enums.end() && enumeration->is_bitfield)
                        return "godot::PROPERTY_HINT_FLAGS";
                }
            }
            const auto separator = field.property_type.name.find('.');
            if (separator != std::string::npos) {
                if (const auto* owner = script_symbols->find_external(
                        field.property_type.name.substr(0, separator))) {
                    if (const auto* enumeration = script_symbols->find_external_enum(
                            *owner, field.property_type.name.substr(separator + 1));
                        enumeration && enumeration->is_bitfield) {
                        return "godot::PROPERTY_HINT_FLAGS";
                    }
                }
            }
        }
        return "godot::PROPERTY_HINT_ENUM";
    }
    // Godot 4.4 predates PROPERTY_HINT_FILE_PATH. The latest-language annotation is still
    // accepted and lowered to its closest 4.4 inspector behavior instead of emitting a C++ enum
    // constant that does not exist in the selected SDK.
    if (field.property->name == "export_file_path" && api.version().rfind("4.4", 0) == 0)
        return "godot::PROPERTY_HINT_FILE";
    static const std::unordered_map<std::string, std::string> hints{
        {"export_range", "godot::PROPERTY_HINT_RANGE"},
        {"export_enum", "godot::PROPERTY_HINT_ENUM"},
        {"export_flags", "godot::PROPERTY_HINT_FLAGS"},
        {"export_file", "godot::PROPERTY_HINT_FILE"},
        {"export_file_path", "godot::PROPERTY_HINT_FILE_PATH"},
        {"export_global_file", "godot::PROPERTY_HINT_GLOBAL_FILE"},
        {"export_dir", "godot::PROPERTY_HINT_DIR"},
        {"export_global_dir", "godot::PROPERTY_HINT_GLOBAL_DIR"},
        {"export_multiline", "godot::PROPERTY_HINT_MULTILINE_TEXT"},
        {"export_color_no_alpha", "godot::PROPERTY_HINT_COLOR_NO_ALPHA"},
        {"export_node_path", "godot::PROPERTY_HINT_NODE_PATH_VALID_TYPES"},
        {"export_placeholder", "godot::PROPERTY_HINT_PLACEHOLDER_TEXT"},
        {"export_exp_easing", "godot::PROPERTY_HINT_EXP_EASING"},
        {"export_tool_button", "godot::PROPERTY_HINT_TOOL_BUTTON"},
        {"export_flags_2d_render", "godot::PROPERTY_HINT_LAYERS_2D_RENDER"},
        {"export_flags_2d_physics", "godot::PROPERTY_HINT_LAYERS_2D_PHYSICS"},
        {"export_flags_2d_navigation", "godot::PROPERTY_HINT_LAYERS_2D_NAVIGATION"},
        {"export_flags_3d_render", "godot::PROPERTY_HINT_LAYERS_3D_RENDER"},
        {"export_flags_3d_physics", "godot::PROPERTY_HINT_LAYERS_3D_PHYSICS"},
        {"export_flags_3d_navigation", "godot::PROPERTY_HINT_LAYERS_3D_NAVIGATION"},
        {"export_flags_avoidance", "godot::PROPERTY_HINT_LAYERS_AVOIDANCE"},
    };
    if (field.property->name == "export_custom" && !field.property->arguments.empty()) {
        auto hint = field.property->arguments.front().value;
        if (hint.rfind("PROPERTY_HINT_", 0) == 0)
            hint = "godot::" + hint;
        return "static_cast<godot::PropertyHint>(" + hint + ")";
    }
    if (const auto found = hints.find(field.property->name); found != hints.end())
        return found->second;
    if (field.property_type.kind == TypeKind::object &&
        api.inherits(field.property_type.name, "Resource")) {
        return "godot::PROPERTY_HINT_RESOURCE_TYPE";
    }
    if (field.property_type.kind == TypeKind::object &&
        api.inherits(field.property_type.name, "Node")) {
        return "godot::PROPERTY_HINT_NODE_TYPE";
    }
    return {};
}

std::string property_hint_string(const typed::Field& field,
                                 const ScriptSymbolTable* script_symbols) {
    if (!field.property)
        return {};
    if (field.property_type.kind == TypeKind::enumeration) {
        if (!field.enum_hint.empty())
            return field.enum_hint;
        if (script_symbols) {
            if (const auto separator = field.property_type.name.rfind("::");
                separator != std::string::npos) {
                const auto owner_name = field.property_type.name.substr(0, separator);
                const auto enum_name = field.property_type.name.substr(separator + 2);
                const ScriptEnumSymbol* enumeration = nullptr;
                if (const auto* owner = script_symbols->find_native_class(owner_name)) {
                    enumeration = script_symbols->find_enum(*owner, enum_name);
                } else if (const auto* inner = script_symbols->find_inner_native(owner_name)) {
                    const auto found = std::find_if(inner->enums.begin(), inner->enums.end(),
                                                    [&](const ScriptEnumSymbol& candidate) {
                                                        return candidate.name == enum_name;
                                                    });
                    if (found != inner->enums.end())
                        enumeration = &*found;
                }
                if (enumeration) {
                    std::string result;
                    for (const auto& entry : enumeration->entries) {
                        if (!result.empty())
                            result.push_back(',');
                        result += entry.name + ":" + std::to_string(entry.value);
                    }
                    return result;
                }
            }
            const auto separator = field.property_type.name.find('.');
            if (separator != std::string::npos) {
                if (const auto* owner = script_symbols->find_global(
                        field.property_type.name.substr(0, separator))) {
                    if (const auto* enumeration = script_symbols->find_enum(
                            *owner, field.property_type.name.substr(separator + 1))) {
                        std::string result;
                        for (const auto& entry : enumeration->entries) {
                            if (!result.empty())
                                result.push_back(',');
                            result += entry.name + ":" + std::to_string(entry.value);
                        }
                        return result;
                    }
                }
                if (const auto* owner = script_symbols->find_external(
                        field.property_type.name.substr(0, separator))) {
                    if (const auto* enumeration = script_symbols->find_external_enum(
                            *owner, field.property_type.name.substr(separator + 1))) {
                        std::string result;
                        for (const auto& entry : enumeration->entries) {
                            if (!result.empty())
                                result.push_back(',');
                            result += entry.name + ":" + std::to_string(entry.value);
                        }
                        return result;
                    }
                }
            }
        }
        return {};
    }
    if (field.property->name == "export" && field.property_type.kind == TypeKind::object)
        return field.property_type.name;
    std::size_t argument_begin = 0;
    std::size_t argument_end = field.property->arguments.size();
    if (field.property->name == "export_custom") {
        argument_begin = std::min<std::size_t>(1, argument_end);
        argument_end = std::min<std::size_t>(2, argument_end);
    }
    std::string result;
    for (std::size_t index = argument_begin; index < argument_end; ++index) {
        const auto& argument = field.property->arguments[index];
        if (!result.empty())
            result.push_back(',');
        auto value = argument.value;
        if (argument.kind == typed::PropertyArgumentKind::number) {
            value.erase(std::remove(value.begin(), value.end(), '_'), value.end());
        }
        result += value;
    }
    return result;
}

bool is_bound_property(const typed::Field& field) {
    // GDScript instance variables are always visible through Object.get/set,
    // even without @export or custom accessors. Generated native classes must
    // expose the same surface or cross-script expressions such as
    // `other.open_bag` silently lose their semantics after AOT conversion.
    // property_info() marks plain variables as SCRIPT_VARIABLE-only, so they
    // stay dynamically accessible without becoming editor/storage properties.
    return !field.is_static && !field.is_constant;
}

std::string property_info(const typed::Field& field, const GodotApi& api,
                          const ScriptSymbolTable* script_symbols, const std::string& native_type) {
    if (describe_container_type(field.property_type) &&
        (!field.property || field.property->name == "export" ||
         field.property->name == "export_storage")) {
        std::string usage;
        if (!field.property)
            usage = "godot::PROPERTY_USAGE_SCRIPT_VARIABLE";
        else if (field.property->name == "export_storage")
            usage = "godot::PROPERTY_USAGE_STORAGE";
        std::string result = "[] { auto info = godot::GetTypeInfo<" + native_type +
                             ">::get_class_info(); info.name = " + godot_string_name(field.name) +
                             ";";
        if (!usage.empty())
            result += " info.usage = " + usage + ";";
        return "(" + result + " return info; }())";
    }
    std::string result = "godot::PropertyInfo(" + variant_type(field.property_type) + ", " +
                         godot_text_argument(field.name);
    const auto hint = property_hint(field, api, script_symbols);
    const auto hint_string = property_hint_string(field, script_symbols);
    std::string usage;
    if (field.property && field.property->name == "export_storage") {
        usage = "godot::PROPERTY_USAGE_STORAGE";
    } else if (field.property && field.property->name == "export_tool_button") {
        usage = "godot::PROPERTY_USAGE_EDITOR";
    } else if (field.property && field.property->name == "export_custom" &&
               field.property->arguments.size() >= 3) {
        usage = field.property->arguments[2].value;
        if (usage.rfind("PROPERTY_USAGE_", 0) == 0)
            usage = "godot::" + usage;
        // GDScript always marks declared fields as script variables in addition to the explicit
        // @export_custom usage mask. Omitting this bit changes reflection and serialization even
        // when the user supplied PROPERTY_USAGE_DEFAULT verbatim.
        usage = "(static_cast<uint32_t>(" + usage +
                ") | static_cast<uint32_t>(godot::PROPERTY_USAGE_SCRIPT_VARIABLE))";
    } else if (!field.property) {
        usage = "godot::PROPERTY_USAGE_SCRIPT_VARIABLE";
    }
    if (field.property_type.kind == TypeKind::variant) {
        if (usage.empty())
            usage = "godot::PROPERTY_USAGE_DEFAULT";
        usage = "(static_cast<uint32_t>(" + usage +
                ") | static_cast<uint32_t>(godot::PROPERTY_USAGE_NIL_IS_VARIANT))";
    }
    // PropertyInfo's hint and usage occupy one fixed argument tuple. Building them in separate
    // annotation branches used to duplicate the hint pair for combinations such as
    // `@export_storage` plus an enum/resource type.
    if (!hint.empty() || !hint_string.empty() || !usage.empty()) {
        result += ", " + (hint.empty() ? "godot::PROPERTY_HINT_NONE" : hint) + ", " +
                  godot_string(hint_string);
        if (!usage.empty())
            result += ", " + usage;
    }
    return result + ")";
}

std::string variant_operator(const std::string& operation) {
    static const std::unordered_map<std::string, std::string> operators{
        {"==", "OP_EQUAL"},       {"!=", "OP_NOT_EQUAL"}, {"<", "OP_LESS"},
        {"<=", "OP_LESS_EQUAL"},  {">", "OP_GREATER"},    {">=", "OP_GREATER_EQUAL"},
        {"+", "OP_ADD"},          {"-", "OP_SUBTRACT"},   {"*", "OP_MULTIPLY"},
        {"/", "OP_DIVIDE"},       {"%", "OP_MODULE"},     {"<<", "OP_SHIFT_LEFT"},
        {">>", "OP_SHIFT_RIGHT"}, {"&", "OP_BIT_AND"},    {"|", "OP_BIT_OR"},
        {"^", "OP_BIT_XOR"},      {"**", "OP_POWER"},     {"and", "OP_AND"},
        {"or", "OP_OR"},          {"in", "OP_IN"},        {"not", "OP_NOT"},
        {"~", "OP_BIT_NEGATE"},
    };
    const auto found = operators.find(operation);
    return found == operators.end() ? std::string{} : found->second;
}

bool has_direct_cpp_binary_operator(const std::string_view operation, const Type& left,
                                    const Type& right) {
    const bool integer_left = left.kind == TypeKind::integer || left.kind == TypeKind::enumeration;
    const bool integer_right =
        right.kind == TypeKind::integer || right.kind == TypeKind::enumeration;
    if (integer_left && integer_right)
        return true;
    if (left.is_numeric() && right.is_numeric())
        return operation != "%";
    if (left.kind == TypeKind::string && right.kind == TypeKind::string) {
        return operation == "+" || operation == "==" || operation == "!=" || operation == "<" ||
               operation == "<=" || operation == ">" || operation == ">=";
    }
    if (left.kind == TypeKind::string_name && right.kind == TypeKind::string_name)
        return operation == "==" || operation == "!=" || operation == "<";
    if (left.kind == TypeKind::boolean && right.kind == TypeKind::boolean)
        return operation == "==" || operation == "!=";
    return false;
}

std::string godot_cpp_method_identifier(const std::string_view source_name) {
    // These are public godot-cpp wrapper names, not GDPP-owned symbols. Match the binding
    // generator's stable escape table instead of applying the compiler's injective encoding for
    // customer identifiers.
    static const std::unordered_map<std::string_view, std::string_view> escaped{
        {"class", "_class"},       {"char", "_char"},         {"short", "_short"},
        {"bool", "_bool"},         {"int", "_int"},           {"default", "_default"},
        {"case", "_case"},         {"switch", "_switch"},     {"export", "_export"},
        {"template", "_template"}, {"new", "new_"},           {"operator", "_operator"},
        {"typeof", "type_of"},     {"typename", "type_name"}, {"enum", "_enum"},
    };
    if (const auto found = escaped.find(source_name); found != escaped.end())
        return std::string{found->second};
    return std::string{source_name};
}

} // namespace

std::string CodeGenerator::sanitize_identifier(const std::string& value) {
    static const std::unordered_set<std::string_view> reserved{
        "alignas",      "alignof",  "and",       "and_eq",   "asm",       "auto",
        "bitand",       "bitor",    "bool",      "break",    "case",      "catch",
        "char",         "class",    "compl",     "const",    "constexpr", "const_cast",
        "continue",     "decltype", "default",   "delete",   "do",        "double",
        "dynamic_cast", "else",     "enum",      "explicit", "export",    "extern",
        "false",        "float",    "for",       "friend",   "goto",      "if",
        "inline",       "int",      "long",      "mutable",  "namespace", "new",
        "noexcept",     "not",      "not_eq",    "nullptr",  "operator",  "or",
        "or_eq",        "private",  "protected", "public",   "register",  "reinterpret_cast",
        "return",       "short",    "signed",    "sizeof",   "static",    "static_assert",
        "static_cast",  "struct",   "switch",    "template", "this",      "thread_local",
        "throw",        "true",     "try",       "typedef",  "typeid",    "typename",
        "typeof",       "union",    "unsigned",  "using",    "virtual",   "void",
        "volatile",     "wchar_t",  "while",     "xor",      "xor_eq",
    };

    constexpr std::string_view encoded_prefix{"_gdpp_id_"};
    constexpr std::string_view internal_prefix{"_gdpp_"};
    const auto is_plain_identifier =
        !value.empty() &&
        (std::isalpha(static_cast<unsigned char>(value.front())) != 0 || value.front() == '_') &&
        std::all_of(value.begin() + 1, value.end(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '_';
        });
    const auto is_implementation_reserved =
        value.size() >= 2 && value.front() == '_' &&
        (value[1] == '_' || std::isupper(static_cast<unsigned char>(value[1])) != 0);
    if (is_plain_identifier && reserved.find(value) == reserved.end() &&
        value.rfind(internal_prefix, 0) != 0 && !is_implementation_reserved) {
        return value;
    }

    // Native identifiers are deliberately ASCII and injective. Encoding the complete UTF-8 byte
    // sequence avoids normalization/toolchain differences and prevents collisions such as
    // `template` versus a user-authored `_gdpp_id_74656d706c617465`.
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string encoded{encoded_prefix};
    encoded.reserve(encoded_prefix.size() + value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(hexadecimal[byte >> 4U]);
        encoded.push_back(hexadecimal[byte & 0x0fU]);
    }
    return encoded;
}

std::string CodeGenerator::enum_identifier(const std::string& value) {
    // System and third-party headers expose many object-like macros (SING is one common
    // example from <math.h>). Enum storage names are internal implementation details, so use a
    // collision-proof native prefix while ClassDB continues to publish the original GDS name.
    return "_gdpp_enum_" + sanitize_identifier(value);
}

void CodeGenerator::emit_named_enum_declaration(const typed::Enum& enumeration,
                                                std::ostringstream& header,
                                                const std::size_t indent) const {
    const std::string outer(indent, ' ');
    const std::string body(indent + 4, ' ');
    const std::string nested(indent + 8, ' ');
    header << outer << "struct " << sanitize_identifier(enumeration.name) << " {\n";
    for (const auto& entry : enumeration.entries) {
        header << body << "inline static constexpr int64_t " << enum_identifier(entry.name) << " = "
               << entry.value << ";\n";
    }
    header << body << "static const godot::Dictionary& _gdpp_dictionary() {\n"
           << nested << "return *gdpp::runtime::engine_lifetime_static_ptr([] {\n"
           << nested << "    godot::Dictionary result;\n";
    for (const auto& entry : enumeration.entries) {
        header << nested << "    result[" << godot_string(entry.name) << "] = int64_t{"
               << entry.value << "};\n";
    }
    header << nested << "    result.make_read_only();\n"
           << nested << "    return result;\n"
           << nested << "});\n"
           << body << "}\n"
           << outer << "};\n";
}

std::string CodeGenerator::sanitize_qualified_identifier(std::string_view value) {
    std::string result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto separator = value.find("::", begin);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        if (!result.empty())
            result += "::";
        result += sanitize_identifier(std::string{value.substr(begin, end - begin)});
        if (separator == std::string_view::npos)
            break;
        begin = separator + 2;
    }
    return result;
}

bool is_valid_base_type(const std::string& value) {
    if (value.empty() ||
        (std::isalpha(static_cast<unsigned char>(value.front())) == 0 && value.front() != '_')) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '_';
    });
}

std::string CodeGenerator::cpp_type(const Type& type) const {
    switch (type.kind) {
    case TypeKind::void_type:
        return "void";
    case TypeKind::boolean:
        return "bool";
    case TypeKind::integer:
        return "int64_t";
    case TypeKind::floating:
        return "double";
    case TypeKind::string:
        return "godot::String";
    case TypeKind::string_name:
        return "godot::StringName";
    case TypeKind::array:
        if (const auto container = describe_container_type(type)) {
            if (!container->has_runtime_constraint())
                return "godot::Array";
            const auto argument = container_cpp_argument(container->arguments.front());
            const auto argument_type = container_argument_type(container->arguments.front());
            const auto script_typed = argument_type.kind == TypeKind::object &&
                                      (argument_type.name == "GDScript" ||
                                       !attached_script_source_path(argument_type).empty());
            return (script_typed ? "gdpp::runtime::ScriptTypedArray<" : "godot::TypedArray<") +
                   argument + ">";
        }
        return "godot::Array";
    case TypeKind::dictionary:
        if (const auto container = describe_container_type(type)) {
            if (!container->has_runtime_constraint())
                return "godot::Dictionary";
            const auto key_type = container_argument_type(container->arguments.at(0));
            const auto value_type = container_argument_type(container->arguments.at(1));
            const auto script_typed =
                (key_type.kind == TypeKind::object &&
                 (key_type.name == "GDScript" ||
                  !attached_script_source_path(key_type).empty())) ||
                (value_type.kind == TypeKind::object &&
                 (value_type.name == "GDScript" ||
                  !attached_script_source_path(value_type).empty()));
            return (script_typed ? "gdpp::runtime::ScriptTypedDictionary<"
                                 : "godot::TypedDictionary<") +
                   container_cpp_argument(container->arguments.at(0)) + ", " +
                   container_cpp_argument(container->arguments.at(1)) + ">";
        }
        return "godot::Dictionary";
    case TypeKind::enumeration:
        return "int64_t";
    case TypeKind::script_resource:
        if (script_symbols_) {
            if (const auto* symbol = script_symbols_->find_path(type.name))
                return "gdpp::runtime::ScriptResource<" + symbol->native_class_name + ">";
        }
        return "godot::Variant";
    case TypeKind::builtin:
        if (type.is_packed_array()) {
            return "gdpp::runtime::SharedPackedArray<godot::" + type.name + ">";
        }
        return "godot::" + type.name;
    case TypeKind::object:
        if (type.name == "GDScript")
            return "godot::Ref<godot::Script>";
        if (!api_.find_class(type.name)) {
            if (const auto inner = inner_cpp_type(type.name); !inner.empty()) {
                const auto godot_base = inner_godot_base_type(type.name);
                if (api_.inherits(godot_base, "RefCounted"))
                    return "godot::Ref<godot::RefCounted>";
                return "gdpp::runtime::ObjectStorage<godot::Object>";
            }
            if (script_symbols_) {
                if (const auto* symbol = script_symbols_->find_class(type.name)) {
                    if (symbol->attached) {
                        if (api_.inherits(symbol->godot_base_type, "RefCounted"))
                            return "godot::Ref<godot::RefCounted>";
                        return "gdpp::runtime::ObjectStorage<godot::Object>";
                    }
                    if (api_.inherits(symbol->godot_base_type, "RefCounted"))
                        return "godot::Ref<" + symbol->native_class_name + ">";
                    return "gdpp::runtime::ObjectStorage<" + symbol->native_class_name + ">";
                }
                if (const auto* symbol = script_symbols_->find_native_class(type.name)) {
                    if (symbol->attached) {
                        if (api_.inherits(symbol->godot_base_type, "RefCounted"))
                            return "godot::Ref<godot::RefCounted>";
                        return "gdpp::runtime::ObjectStorage<godot::Object>";
                    }
                    if (api_.inherits(symbol->godot_base_type, "RefCounted"))
                        return "godot::Ref<" + symbol->native_class_name + ">";
                    return "gdpp::runtime::ObjectStorage<" + symbol->native_class_name + ">";
                }
            }
            return "godot::Variant";
        }
        if (api_.inherits(type.name, "RefCounted"))
            return "godot::Ref<godot::" + godot_cpp_class_name(type.name) + ">";
        return "gdpp::runtime::ObjectStorage<godot::" + godot_cpp_class_name(type.name) + ">";
    default:
        return "godot::Variant";
    }
}

std::string CodeGenerator::native_default_value(const Type& type) const {
    if (type.kind == TypeKind::void_type || type.is_dynamic() || type.kind == TypeKind::nil)
        return "godot::Variant{}";
    const auto native_type = cpp_type(type);
    if (!native_type.empty() && native_type.back() == '*')
        return "static_cast<" + native_type + ">(nullptr)";
    return native_type + "{}";
}

std::string CodeGenerator::self_object_expression() const {
    return attached_script_ ? "owner()" : "this";
}

std::string CodeGenerator::await_owner_expression() const {
    return current_coroutine_abi_
               ? "gdpp::runtime::coroutine_owner(" + current_coroutine_state_ + ")"
           : current_static_context_ ? "nullptr"
                                     : self_object_expression();
}

std::string CodeGenerator::godot_owner_expression() const {
    if (!attached_script_)
        return "this";
    return "godot::Object::cast_to<godot::" + attached_godot_base_type_ + ">(owner())";
}

std::string CodeGenerator::api_native_type(std::string_view api_type,
                                           const std::string_view native_meta) const {
    const auto comma = api_type.find(',');
    if (comma != std::string_view::npos)
        api_type = api_type.substr(0, comma);
    while (!api_type.empty() && api_type.front() == '-')
        api_type.remove_prefix(1);
    if (api_type.find('*') != std::string_view::npos) {
        std::string raw{api_type};
        const auto base_begin = raw.rfind("const ", 0) == 0 ? std::size_t{6} : std::size_t{0};
        const auto pointer = raw.find('*', base_begin);
        auto base_end = pointer;
        while (base_end > base_begin && raw[base_end - 1] == ' ')
            --base_end;
        const auto base = raw.substr(base_begin, base_end - base_begin);
        const bool primitive = base == "void" || base == "float" || base == "double" ||
                               base == "int8_t" || base == "int16_t" || base == "int32_t" ||
                               base == "int64_t" || base == "uint8_t" || base == "uint16_t" ||
                               base == "uint32_t" || base == "uint64_t";
        return raw.substr(0, base_begin) + (primitive ? "" : "godot::") + base +
               raw.substr(base_end);
    }
    const bool bitfield = api_type.rfind("bitfield::", 0) == 0;
    const bool enumeration = api_type.rfind("enum::", 0) == 0;
    if (bitfield || enumeration) {
        api_type.remove_prefix(bitfield ? std::string_view{"bitfield::"}.size()
                                        : std::string_view{"enum::"}.size());
        std::string qualified{api_type};
        std::replace(qualified.begin(), qualified.end(), '.', ':');
        for (std::size_t position = 0;
             (position = qualified.find(':', position)) != std::string::npos; position += 2) {
            if (position + 1 >= qualified.size() || qualified[position + 1] != ':')
                qualified.insert(position, 1, ':');
        }
        const auto native = "godot::" + qualified;
        return bitfield ? "godot::BitField<" + native + ">" : native;
    }
    if (const auto scalar = godot_native_scalar_cpp_type(godot_native_meta(native_meta));
        !scalar.empty()) {
        return std::string{scalar};
    }
    const auto type = type_from_godot_api(api_type);
    if (type.is_packed_array())
        return "godot::" + type.name;
    return cpp_type(type);
}

std::string CodeGenerator::virtual_parameter_type(const GodotMethodRecord& method,
                                                  const std::size_t index) const {
    const auto* argument = api_.argument(method, index);
    if (!argument)
        return "godot::Variant";
    auto result = api_native_type(argument->type, argument->meta);
    if (std::string_view{argument->type}.find('*') != std::string_view::npos)
        return result;
    const auto type = type_from_godot_api(argument->type);
    const bool ref_counted =
        type.kind == TypeKind::object && api_.inherits(type.name, "RefCounted");
    const bool builtin_reference =
        type.kind == TypeKind::string || type.kind == TypeKind::string_name ||
        type.kind == TypeKind::array || type.kind == TypeKind::dictionary ||
        type.kind == TypeKind::variant || type.kind == TypeKind::builtin;
    if (ref_counted || builtin_reference)
        result = "const " + result + "&";
    return result;
}

std::string CodeGenerator::virtual_return_type(const GodotMethodRecord& method) const {
    return api_native_type(method.return_type, method.return_meta);
}

std::string CodeGenerator::native_property_info(const Type& type,
                                                const std::string_view name) const {
    // Variant is encoded as NIL plus PROPERTY_USAGE_NIL_IS_VARIANT in Godot's reflection ABI.
    // A plain NIL PropertyInfo means an actual nil value and causes editor/runtime introspection
    // to lose the dynamic argument contract.
    if (type.kind == TypeKind::variant || describe_container_type(type)) {
        return "([] { auto info = godot::GetTypeInfo<" + cpp_type(type) +
               ">::get_class_info(); info.name = " + godot_string_name(std::string{name}) +
               "; return info; }())";
    }
    return "godot::PropertyInfo(" + variant_type(type) + ", " +
           godot_text_argument(std::string{name}) + ")";
}

Type CodeGenerator::container_argument_type(const std::string_view type_name) const {
    const auto name = std::string{type_name};
    if (container_enum_types_.find(name) != container_enum_types_.end() ||
        api_.has_global_enum(name)) {
        return {TypeKind::enumeration, name};
    }
    if (const auto separator = name.rfind('.'); separator != std::string::npos) {
        const auto owner_name = name.substr(0, separator);
        const auto enum_name = name.substr(separator + 1);
        if (api_.has_class_enum(owner_name, enum_name))
            return {TypeKind::enumeration, name};
        if (script_symbols_) {
            if (const auto* owner = script_symbols_->find_global(owner_name);
                owner && script_symbols_->find_enum(*owner, enum_name)) {
                return {TypeKind::enumeration, name};
            }
            if (const auto* owner = script_symbols_->find_external(owner_name);
                owner && script_symbols_->find_external_enum(*owner, enum_name)) {
                return {TypeKind::enumeration, name};
            }
        }
    }
    if (const auto separator = name.rfind("::"); separator != std::string::npos) {
        const auto owner_name = name.substr(0, separator);
        const auto enum_name = name.substr(separator + 2);
        if (owner_name == current_native_class_name_ &&
            container_enum_types_.find(enum_name) != container_enum_types_.end()) {
            return {TypeKind::enumeration, name};
        }
        if (script_symbols_) {
            if (const auto* owner = script_symbols_->find_native_class(owner_name);
                owner && script_symbols_->find_enum(*owner, enum_name)) {
                return {TypeKind::enumeration, name};
            }
            if (const auto* owner = script_symbols_->find_inner_native(owner_name);
                owner &&
                std::any_of(owner->enums.begin(), owner->enums.end(), [&](const auto& enumeration) {
                    return enumeration.name == enum_name;
                })) {
                return {TypeKind::enumeration, name};
            }
        }
    }
    if (current_script_ && script_symbols_ && script_symbols_->find_enum(*current_script_, name))
        return {TypeKind::enumeration, name};
    if (current_inner_script_ &&
        std::any_of(current_inner_script_->enums.begin(), current_inner_script_->enums.end(),
                    [&](const auto& enumeration) { return enumeration.name == name; }))
        return {TypeKind::enumeration, name};
    return type_from_annotation(name);
}

std::string CodeGenerator::container_cpp_argument(const std::string_view type_name) const {
    const auto type = container_argument_type(type_name);
    if (type.kind == TypeKind::object && type.name == "GDScript")
        return "gdpp::runtime::GDScriptContainerTag";
    switch (type.kind) {
    case TypeKind::variant:
    case TypeKind::unknown:
        return "godot::Variant";
    case TypeKind::boolean:
        return "bool";
    case TypeKind::integer:
    case TypeKind::enumeration:
        return "int64_t";
    case TypeKind::floating:
        return "double";
    case TypeKind::string:
        return "godot::String";
    case TypeKind::string_name:
        return "godot::StringName";
    case TypeKind::array:
        return "godot::Array";
    case TypeKind::dictionary:
        return "godot::Dictionary";
    case TypeKind::builtin:
        return "godot::" + type.name;
    case TypeKind::object:
        return "gdpp::runtime::container_tags::ContainerObjectTag_" +
               sanitize_identifier(container_object_tag_identity(type_name));
    case TypeKind::nil:
    case TypeKind::script_resource:
    case TypeKind::void_type:
        return "godot::Variant";
    }
    return "godot::Variant";
}

std::string CodeGenerator::container_object_tag_identity(const std::string_view type_name) const {
    const auto type = container_argument_type(type_name);
    if (type.kind != TypeKind::object)
        return std::string{type_name};
    if (api_.find_class(type.name))
        return type.name;
    if (const auto inner = inner_cpp_type(type.name); !inner.empty())
        return inner;
    if (script_symbols_) {
        if (const auto* script = script_symbols_->find_class(type.name))
            return script->native_class_name;
        if (const auto* script = script_symbols_->find_native_class(type.name))
            return script->native_class_name;
    }
    return type.name.empty() ? std::string{type_name} : type.name;
}

std::string CodeGenerator::container_object_runtime_name(const std::string_view type_name) const {
    const Type type{TypeKind::object, std::string{type_name}};
    if (api_.find_class(type.name))
        return type.name;
    if (!attached_script_source_path(type).empty()) {
        if (const auto inner = inner_cpp_type(type_name); !inner.empty())
            return inner_attached_native_base_type(type_name);
        if (script_symbols_) {
            auto* script = script_symbols_->find_class(std::string{type_name});
            if (!script)
                script = script_symbols_->find_native_class(std::string{type_name});
            if (script) {
                return script->attached_native_base.empty() ? script->godot_base_type
                                                            : script->attached_native_base;
            }
        }
    }
    if (const auto inner = inner_cpp_type(type_name); !inner.empty())
        return inner;
    if (script_symbols_) {
        if (const auto* script = script_symbols_->find_class(std::string{type_name}))
            return script->native_class_name;
        if (const auto* external = script_symbols_->find_external(std::string{type_name}))
            return external->name;
    }
    return std::string{type_name};
}

std::string CodeGenerator::inner_cpp_type(std::string_view name) const {
    if (const auto exact = inner_native_names_.find(std::string{name});
        exact != inner_native_names_.end()) {
        return exact->second;
    }
    const auto native_match = std::find_if(inner_native_names_.begin(), inner_native_names_.end(),
                                           [&](const auto& entry) { return entry.second == name; });
    if (native_match != inner_native_names_.end())
        return native_match->second;
    if (script_symbols_) {
        if (const auto* inner = script_symbols_->find_inner_native(std::string{name}))
            return inner->native_class_name;
    }
    const auto separator = name.rfind('.');
    if (separator != std::string_view::npos)
        name.remove_prefix(separator + 1);
    const std::string* unique = nullptr;
    for (const auto& [qualified, native] : inner_native_names_) {
        const auto leaf_separator = qualified.rfind('.');
        const auto leaf = leaf_separator == std::string::npos
                              ? std::string_view{qualified}
                              : std::string_view{qualified}.substr(leaf_separator + 1);
        if (leaf != name)
            continue;
        if (unique)
            return {};
        unique = &native;
    }
    return unique ? *unique : std::string{};
}

std::string CodeGenerator::inner_godot_base_type(std::string_view name) const {
    if (const auto exact = inner_godot_base_types_.find(std::string{name});
        exact != inner_godot_base_types_.end()) {
        return exact->second;
    }
    const auto native_match = std::find_if(inner_native_names_.begin(), inner_native_names_.end(),
                                           [&](const auto& entry) { return entry.second == name; });
    if (native_match != inner_native_names_.end()) {
        if (const auto base = inner_godot_base_types_.find(native_match->first);
            base != inner_godot_base_types_.end()) {
            return base->second;
        }
    }
    if (script_symbols_) {
        if (const auto* inner = script_symbols_->find_inner_native(std::string{name}))
            return inner->godot_base_type;
    }
    const auto separator = name.rfind('.');
    if (separator != std::string_view::npos)
        name.remove_prefix(separator + 1);
    const auto found = inner_godot_base_types_.find(std::string{name});
    return found == inner_godot_base_types_.end() ? std::string{name} : found->second;
}

std::string CodeGenerator::inner_attached_native_base_type(std::string_view name) const {
    if (const auto exact = inner_attached_native_base_types_.find(std::string{name});
        exact != inner_attached_native_base_types_.end()) {
        return exact->second;
    }
    const auto native_match = std::find_if(inner_native_names_.begin(), inner_native_names_.end(),
                                           [&](const auto& entry) { return entry.second == name; });
    if (native_match != inner_native_names_.end()) {
        if (const auto base = inner_attached_native_base_types_.find(native_match->first);
            base != inner_attached_native_base_types_.end()) {
            return base->second;
        }
    }
    if (script_symbols_) {
        if (const auto* inner = script_symbols_->find_inner_native(std::string{name})) {
            return inner->attached_native_base.empty() ? inner->godot_base_type
                                                       : inner->attached_native_base;
        }
    }
    const auto separator = name.rfind('.');
    if (separator != std::string_view::npos)
        name.remove_prefix(separator + 1);
    const auto found = inner_attached_native_base_types_.find(std::string{name});
    return found == inner_attached_native_base_types_.end() ? inner_godot_base_type(name)
                                                            : found->second;
}

void CodeGenerator::record_generated_symbol(const GeneratedSymbolKind kind,
                                            std::string source_symbol, std::string native_symbol,
                                            const SourceSpan span) const {
    generated_symbols_.push_back({kind, std::move(source_symbol), std::move(native_symbol), span});
}

std::string CodeGenerator::serialize_generated_symbols(const GeneratedUnit& unit) const {
    const auto kind_name = [](const GeneratedSymbolKind kind) -> std::string_view {
        switch (kind) {
        case GeneratedSymbolKind::method:
            return "method";
        case GeneratedSymbolKind::virtual_adapter:
            return "virtual_adapter";
        case GeneratedSymbolKind::variant_callback:
            return "variant_callback";
        case GeneratedSymbolKind::property_getter:
            return "property_getter";
        case GeneratedSymbolKind::property_setter:
            return "property_setter";
        case GeneratedSymbolKind::synthesized_ready:
            return "synthesized_ready";
        }
        return "invalid";
    };
    std::ostringstream output;
    output << "GDPP_SYMBOL_MAP 1\n"
           << "source " << std::quoted("res://" + current_source_path_) << '\n'
           << "generated " << std::quoted(unit.source_file_name) << '\n'
           << "class " << std::quoted(unit.class_name) << '\n'
           << "symbols " << generated_symbols_.size() << '\n';
    for (const auto& symbol : generated_symbols_) {
        output << kind_name(symbol.kind) << ' ' << std::quoted(symbol.source_symbol) << ' '
               << std::quoted(symbol.native_symbol) << ' ' << symbol.span.begin.offset << ' '
               << symbol.span.begin.line << ' ' << symbol.span.begin.column << ' '
               << symbol.span.end.offset << ' ' << symbol.span.end.line << ' '
               << symbol.span.end.column << '\n';
    }
    return output.str();
}

std::string
CodeGenerator::attached_script_source_path(const Type& type,
                                           const std::string_view resolved_owner) const {
    if (type.kind != TypeKind::object)
        return {};
    if (api_.find_class(type.name))
        return {};

    const auto current_inner_path = [&](const std::string_view identity) {
        if (identity.empty())
            return std::string{};
        const auto native = inner_cpp_type(identity);
        if (native.empty())
            return std::string{};
        const auto found = std::find_if(inner_native_names_.begin(), inner_native_names_.end(),
                                        [&](const auto& entry) { return entry.second == native; });
        return found == inner_native_names_.end()
                   ? std::string{}
                   : "res://" + current_source_path_ + "::" + found->first;
    };
    if (auto path = current_inner_path(type.name); !path.empty())
        return path;
    if (auto path = current_inner_path(resolved_owner); !path.empty())
        return path;
    if (!script_symbols_)
        return {};

    const auto find_script = [&](const std::string_view identity) {
        if (identity.empty())
            return static_cast<const ScriptClassSymbol*>(nullptr);
        const auto name = std::string{identity};
        if (const auto* script = script_symbols_->find_native_class(name))
            return script;
        return api_.find_class(name) ? nullptr : script_symbols_->find_class(name);
    };
    const auto find_inner = [&](const std::string_view identity) {
        if (identity.empty())
            return static_cast<const ScriptInnerClassSymbol*>(nullptr);
        const auto name = std::string{identity};
        if (const auto* inner = script_symbols_->find_inner_native(name))
            return inner;
        if (current_script_) {
            if (const auto* inner = script_symbols_->find_inner(*current_script_, name))
                return inner;
        }
        const auto native = inner_cpp_type(identity);
        return native.empty() ? static_cast<const ScriptInnerClassSymbol*>(nullptr)
                              : script_symbols_->find_inner_native(native);
    };
    const auto resource_path = [](const std::string& path) {
        return path.rfind("res://", 0) == 0 ? path : "res://" + path;
    };

    if (const auto* script = find_script(type.name); script && script->attached)
        return resource_path(script->path);
    if (const auto* script = find_script(resolved_owner); script && script->attached)
        return resource_path(script->path);

    const auto* inner = find_inner(type.name);
    if (!inner)
        inner = find_inner(resolved_owner);
    if (!inner)
        return {};
    const auto* owner = script_symbols_->owner_of(*inner);
    return owner ? resource_path(owner->path) + "::" + inner->name : std::string{};
}

std::string CodeGenerator::emit_attached_script_cast(const Type& target, std::string value,
                                                     const SourceSpan* source_span) const {
    const auto source_path = attached_script_source_path(target);
    if (source_path.empty())
        return value;
    const auto* location_span = source_span ? source_span : current_expression_span_;
    const auto location = location_span ? ", " + script_location(*location_span)
                                        : ", gdpp::runtime::ScriptSourceLocation{}";
    auto object = "gdpp::runtime::strict_attached_script_storage("
                  "gdpp::runtime::to_variant(" +
                  std::move(value) + "), " + godot_string(source_path) + location + ")";
    return emit_object_pointer_storage(target, std::move(object));
}

std::string CodeGenerator::emit_object_pointer_storage(const Type& target,
                                                       std::string value) const {
    const auto target_cpp = cpp_type(target);
    const std::string ref_prefix{"godot::Ref<"};
    if (target_cpp.rfind(ref_prefix, 0) == 0 && target_cpp.back() == '>') {
        const auto object_cpp =
            target_cpp.substr(ref_prefix.size(), target_cpp.size() - ref_prefix.size() - 1);
        return target_cpp + "(godot::Object::cast_to<" + object_cpp + ">(" + value + "))";
    }
    const std::string storage_prefix{"gdpp::runtime::ObjectStorage<"};
    if (target_cpp.rfind(storage_prefix, 0) == 0 && target_cpp.back() == '>') {
        const auto object_cpp =
            target_cpp.substr(storage_prefix.size(), target_cpp.size() - storage_prefix.size() - 1);
        return target_cpp + "(godot::Object::cast_to<" + object_cpp + ">(" + value + "))";
    }
    if (target_cpp == "godot::Variant")
        return "gdpp::runtime::to_variant(" + value + ")";
    return value;
}

bool CodeGenerator::is_ref_counted_object(const Type& type) const noexcept {
    if (type.kind != TypeKind::object)
        return false;
    if (api_.inherits(type.name, "RefCounted"))
        return true;
    if (script_symbols_) {
        auto* script = script_symbols_->find_class(type.name);
        if (!script)
            script = script_symbols_->find_native_class(type.name);
        if (!script && current_script_ &&
            (current_script_->script_name == type.name ||
             current_script_->native_class_name == type.name)) {
            script = current_script_;
        }
        if (script)
            return api_.inherits(script->godot_base_type, "RefCounted");
        const auto* inner = script_symbols_->find_inner_native(type.name);
        if (!inner) {
            const auto native = inner_cpp_type(type.name);
            if (!native.empty())
                inner = script_symbols_->find_inner_native(native);
        }
        if (inner)
            return api_.inherits(inner->godot_base_type, "RefCounted");
    }
    const auto native = inner_cpp_type(type.name);
    return !native.empty() && api_.inherits(inner_godot_base_type(type.name), "RefCounted");
}

std::string CodeGenerator::native_super_owner(std::string_view owner) const {
    const auto inner = inner_cpp_type(owner);
    return inner.empty() ? std::string{owner} : inner;
}

bool CodeGenerator::same_native_method_abi(const ScriptMemberSymbol& derived,
                                           const ScriptMemberSymbol& base) const {
    const auto native_return_type = [&](const ScriptMemberSymbol& method) {
        // Attached behavior methods use Variant for coroutine completion even when the source
        // name also denotes a Godot virtual. The exact ClassDB virtual wrapper is a separate ABI
        // layer and must not weaken the generated script inheritance contract.
        return method.is_coroutine ? std::string{"godot::Variant"} : cpp_type(method.type);
    };
    const auto native_parameter_type = [&](const ScriptMemberSymbol& method,
                                           const std::size_t index) {
        const bool uses_default_slot =
            index < method.default_parameters.size() && method.default_parameters[index];
        return uses_default_slot ? std::string{"godot::Variant"}
                                 : cpp_type(method.parameters[index]);
    };
    if (derived.is_static != base.is_static || derived.is_vararg != base.is_vararg ||
        derived.parameters.size() != base.parameters.size() ||
        native_return_type(derived) != native_return_type(base)) {
        return false;
    }
    for (std::size_t index = 0; index < derived.parameters.size(); ++index) {
        if (native_parameter_type(derived, index) != native_parameter_type(base, index))
            return false;
    }
    return true;
}

bool CodeGenerator::same_native_function_abi(const typed::Function& derived,
                                             const typed::Function& base) const {
    const auto native_return_type = [&](const typed::Function& function) {
        return function.is_coroutine ? std::string{"godot::Variant"}
                                     : cpp_type(function.return_type);
    };
    if (derived.is_static != base.is_static ||
        derived.rest_parameter.has_value() != base.rest_parameter.has_value() ||
        derived.parameters.size() != base.parameters.size() ||
        native_return_type(derived) != native_return_type(base)) {
        return false;
    }
    for (std::size_t index = 0; index < derived.parameters.size(); ++index) {
        if (parameter_native_type(derived.parameters[index]) !=
            parameter_native_type(base.parameters[index])) {
            return false;
        }
    }
    return true;
}

const typed::Function*
CodeGenerator::find_inherited_inner_function(const std::string_view base,
                                             const std::string_view method,
                                             std::string* declaration_owner) const noexcept {
    std::string current{base};
    std::unordered_set<std::string> visited;
    while (!current.empty() && visited.insert(current).second) {
        if (const auto declaration = inner_declarations_.find(current);
            declaration != inner_declarations_.end()) {
            const auto function = std::find_if(
                declaration->second->functions.begin(), declaration->second->functions.end(),
                [&](const auto& candidate) { return candidate.name == method; });
            if (function != declaration->second->functions.end()) {
                if (declaration_owner)
                    *declaration_owner = current;
                return &*function;
            }
        }
        const auto parent = inner_base_names_.find(current);
        current = parent == inner_base_names_.end() ? std::string{} : parent->second;
    }
    return nullptr;
}

std::string
CodeGenerator::typed_inner_method_implementation_name(const std::string_view owner,
                                                      const typed::Function& method) const {
    const auto source_name = sanitize_identifier(method.name);
    if (!method.is_static && method.name != "_init") {
        const auto base = inner_base_names_.find(std::string{owner});
        if (base != inner_base_names_.end()) {
            std::string declaration_owner;
            const auto* inherited =
                find_inherited_inner_function(base->second, method.name, &declaration_owner);
            if (inherited) {
                const auto inherited_owner =
                    declaration_owner.empty() ? base->second : declaration_owner;
                if (same_native_function_abi(method, *inherited)) {
                    return typed_inner_method_implementation_name(inherited_owner, *inherited);
                }
                return "_gdpp_native_override_" + source_name;
            }
        }
    }
    return "_gdpp_script_method_" + source_name;
}

std::string CodeGenerator::script_method_native_name(const ScriptClassSymbol& owner,
                                                     const ScriptMemberSymbol& method) const {
    const auto source_name = sanitize_identifier(method.name);
    if (!script_symbols_ || method.is_static || method.name == "_init")
        return source_name;
    const auto inherited = script_symbols_->inherited_members(owner);
    const auto base = std::find_if(inherited.begin(), inherited.end(), [&](const auto* candidate) {
        return candidate->kind == ScriptMemberKind::function && !candidate->is_static &&
               candidate->name == method.name;
    });
    if (base == inherited.end())
        return source_name;
    const ScriptClassSymbol* base_owner = script_symbols_->base_of(owner);
    while (base_owner) {
        const auto declared = std::find_if(
            base_owner->members.begin(), base_owner->members.end(), [&](const auto& candidate) {
                return candidate.kind == ScriptMemberKind::function && !candidate.is_static &&
                       candidate.name == method.name;
            });
        if (declared != base_owner->members.end())
            break;
        base_owner = script_symbols_->base_of(*base_owner);
    }
    const auto same_native_abi = same_native_method_abi(method, **base);
    // GDScript permits an override to change annotations and to become a coroutine. C++ cannot
    // overload solely by return type, so an ABI-incompatible override receives a private native
    // symbol while ClassDB continues to publish the original source method name. Once a hierarchy
    // has entered such an isolated ABI family, compatible descendants must keep overriding that
    // exact native symbol instead of reverting to the source-name family.
    if (!same_native_abi)
        return "_gdpp_native_override_" + source_name;
    return base_owner ? script_method_native_name(*base_owner, **base) : source_name;
}

std::string
CodeGenerator::script_method_implementation_name(const ScriptClassSymbol& owner,
                                                 const ScriptMemberSymbol& method) const {
    const auto source_name = sanitize_identifier(method.name);
    const auto script_native_name = script_method_native_name(owner, method);
    const auto* engine_method =
        method.is_static ? nullptr : api_.find_method(owner.godot_base_type, method.name);
    if (!engine_method || !engine_method->is_virtual) {
        // Generated behavior classes inherit godot-cpp's Wrapped implementation. A legal
        // GDScript method such as `_get`, `_set`, `_notification`, `_to_string`, `get_class` or
        // any future engine member must never hide a C++ hook used by GDCLASS. Keep ABI-mismatch
        // symbols, which are already isolated, and namespace every ordinary script method.
        return script_native_name == source_name ? "_gdpp_script_method_" + source_name
                                                 : script_native_name;
    }
    return script_native_name == source_name
               ? "_gdpp_virtual_impl_" + source_name
               : "_gdpp_native_override__gdpp_virtual_impl_" + source_name;
}

std::string
CodeGenerator::local_function_implementation_name(const std::string_view source_name) const {
    if (const auto method = local_function_native_names_.find(std::string{source_name});
        method != local_function_native_names_.end()) {
        return method->second;
    }
    return "_gdpp_script_method_" + sanitize_identifier(std::string{source_name});
}

const ScriptInnerClassSymbol*
CodeGenerator::inner_base_of(const ScriptInnerClassSymbol& owner) const noexcept {
    return script_symbols_ ? script_symbols_->inner_base_of(owner) : nullptr;
}

CodeGenerator::InnerMethodDeclaration
CodeGenerator::find_inner_method_declaration(const ScriptInnerClassSymbol& owner,
                                             const std::string_view method,
                                             const bool include_owner) const noexcept {
    const ScriptInnerClassSymbol* current = include_owner ? &owner : inner_base_of(owner);
    std::unordered_set<const ScriptInnerClassSymbol*> visited;
    while (current && visited.insert(current).second) {
        const auto declared = std::find_if(
            current->members.begin(), current->members.end(), [&](const auto& candidate) {
                return candidate.kind == ScriptMemberKind::function && candidate.name == method;
            });
        if (declared != current->members.end())
            return {&*declared, current, nullptr};
        if (const auto* local_base = inner_base_of(*current)) {
            current = local_base;
            continue;
        }
        if (script_symbols_) {
            const auto* script_base = script_symbols_->base_of(*current);
            while (script_base) {
                const auto script_declared =
                    std::find_if(script_base->members.begin(), script_base->members.end(),
                                 [&](const auto& candidate) {
                                     return candidate.kind == ScriptMemberKind::function &&
                                            candidate.name == method;
                                 });
                if (script_declared != script_base->members.end())
                    return {&*script_declared, nullptr, script_base};
                script_base = script_symbols_->base_of(*script_base);
            }
        }
        current = nullptr;
    }
    if (!include_owner && !inner_base_of(owner) && script_symbols_) {
        const auto* script_base = script_symbols_->base_of(owner);
        while (script_base) {
            const auto declared = std::find_if(
                script_base->members.begin(), script_base->members.end(),
                [&](const auto& candidate) {
                    return candidate.kind == ScriptMemberKind::function && candidate.name == method;
                });
            if (declared != script_base->members.end())
                return {&*declared, nullptr, script_base};
            script_base = script_symbols_->base_of(*script_base);
        }
    }
    return {};
}

std::string CodeGenerator::inner_method_native_name(const ScriptInnerClassSymbol& owner,
                                                    const ScriptMemberSymbol& method) const {
    const auto source_name = sanitize_identifier(method.name);
    if (method.is_static || method.name == "_init")
        return source_name;
    const auto inherited = find_inner_method_declaration(owner, method.name, false);
    if (!inherited.method || inherited.method->is_static)
        return source_name;
    if (!same_native_method_abi(method, *inherited.method)) {
        return "_gdpp_native_override_" + source_name;
    }
    if (inherited.inner_owner)
        return inner_method_native_name(*inherited.inner_owner, *inherited.method);
    if (inherited.script_owner)
        return script_method_native_name(*inherited.script_owner, *inherited.method);
    return source_name;
}

std::string
CodeGenerator::inner_method_implementation_name(const ScriptInnerClassSymbol& owner,
                                                const ScriptMemberSymbol& method) const {
    const auto source_name = sanitize_identifier(method.name);
    const auto script_native_name = inner_method_native_name(owner, method);
    const auto* engine_method =
        method.is_static ? nullptr : api_.find_method(owner.godot_base_type, method.name);
    if (!engine_method || !engine_method->is_virtual)
        return script_native_name == source_name ? "_gdpp_script_method_" + source_name
                                                 : script_native_name;
    return script_native_name == source_name
               ? "_gdpp_virtual_impl_" + source_name
               : "_gdpp_native_override__gdpp_virtual_impl_" + source_name;
}

bool CodeGenerator::inner_overrides_method(const ScriptInnerClassSymbol& owner,
                                           const ScriptMemberSymbol& method) const {
    if (method.is_static || method.name == "_init")
        return false;
    const auto inherited = find_inner_method_declaration(owner, method.name, false);
    if (!inherited.method || inherited.method->is_static)
        return false;
    return same_native_method_abi(method, *inherited.method);
}

bool CodeGenerator::managed_constant_field(const typed::Field& field) const {
    if (!field.is_constant)
        return false;
    const auto uses_signature = [&](const auto& members) -> std::optional<bool> {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) {
            return member.kind == ScriptMemberKind::constant && member.name == field.name;
        });
        if (found == members.end())
            return std::nullopt;
        return managed_static_constant(found->type);
    };
    if (current_inner_script_) {
        if (const auto result = uses_signature(current_inner_script_->members))
            return *result;
    } else if (current_script_) {
        if (const auto result = uses_signature(current_script_->members))
            return *result;
    }
    return managed_static_constant(field.type);
}

bool CodeGenerator::managed_constant_reference(const typed::Expression& expression) const {
    const auto uses_signature = [&](const auto& members) -> std::optional<bool> {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) {
            return member.kind == ScriptMemberKind::constant && member.name == expression.value;
        });
        if (found == members.end())
            return std::nullopt;
        return managed_static_constant(found->type);
    };
    if (expression.resolved_owner.empty()) {
        if (current_inner_script_) {
            if (const auto result = uses_signature(current_inner_script_->members))
                return *result;
        } else if (current_script_) {
            if (const auto result = uses_signature(current_script_->members))
                return *result;
        }
    }
    return managed_static_constant(expression.type);
}

std::string CodeGenerator::emit_conversion(const Type& target, const Type& source,
                                           std::string value, const SourceSpan* source_span) const {
    const auto* location_span = source_span ? source_span : current_expression_span_;
    const auto location = location_span ? ", " + script_location(*location_span) : std::string{};
    if (target.kind == TypeKind::void_type)
        return "(static_cast<void>(" + value + "))";
    if (target.is_dynamic())
        return source.is_dynamic() ? value : "gdpp::runtime::to_variant(" + value + ")";
    if (attached_script_ && target.kind == TypeKind::object && value == self_object_expression()) {
        return emit_object_pointer_storage(target, std::move(value));
    }
    if (target == source)
        return value;
    if (target.kind == TypeKind::script_resource && source.kind == TypeKind::nil)
        return cpp_type(target) + "::missing()";
    if (!attached_script_source_path(target).empty())
        return emit_attached_script_cast(target, std::move(value), source_span);
    if (target.kind == TypeKind::object && target.name == "GDScript") {
        const auto script_location =
            location.empty() ? ", gdpp::runtime::ScriptSourceLocation{}" : location;
        return "gdpp::runtime::strict_gdscript_storage(gdpp::runtime::to_variant(" + value + ")" +
               script_location + ")";
    }
    const bool target_external = target.kind == TypeKind::object && script_symbols_ &&
                                 script_symbols_->find_external(target.name);
    const bool source_external = source.kind == TypeKind::object && script_symbols_ &&
                                 script_symbols_->find_external(source.name);
    if (target_external) {
        if (source.is_dynamic()) {
            return "gdpp::runtime::strict_external_object_storage("
                   "gdpp::runtime::to_variant(" +
                   value + "), " + godot_string_name(target.name) + location + ")";
        }
        return "gdpp::runtime::to_variant(" + value + ")";
    }
    // Project symbol refinement can express the same nested enum through its source-qualified
    // name on a declaration and its collision-safe native-qualified name in typed container
    // storage. Semantic analysis has already rejected unrelated enum assignments; the native
    // representation of every accepted enum identity is int64_t.
    if (target.kind == TypeKind::enumeration && source.kind == TypeKind::enumeration)
        return value;
    if (source.kind == TypeKind::nil && target.kind == TypeKind::object) {
        const auto target_cpp = cpp_type(target);
        return target_cpp + "{}";
    }
    if (describe_container_type(target)) {
        if (is_explicitly_typed_container(target) && target != source) {
            return "gdpp::runtime::strict_typed_storage<" + cpp_type(target) +
                   ">(gdpp::runtime::to_variant(" + value + ")" + location + ")";
        }
        if (source.is_dynamic()) {
            return "gdpp::runtime::strict_builtin_storage<" + cpp_type(target) +
                   ">(gdpp::runtime::to_variant(" + value + "), " + variant_type(target) +
                   location + ")";
        }
        return cpp_type(target) + "(gdpp::runtime::to_variant(" + value + "))";
    }
    if (target.is_packed_array()) {
        return "gdpp::runtime::strict_packed_array_storage<godot::" + target.name +
               ">(gdpp::runtime::to_variant(" + value + ")" + location + ")";
    }
    if (target.kind == TypeKind::object && source.kind == TypeKind::object &&
        target.name != source.name) {
        auto source_object = value;
        if (source_external)
            source_object =
                "(gdpp::runtime::to_variant(" + source_object + ")).get_validated_object()";
        const bool source_ref_counted = is_ref_counted_object(source);
        if (source_ref_counted && !source_external)
            source_object = "(" + source_object + ").ptr()";
        const auto target_cpp = cpp_type(target);
        const std::string ref_prefix{"godot::Ref<"};
        if (target_cpp.rfind(ref_prefix, 0) == 0 && target_cpp.back() == '>') {
            const auto object_cpp =
                target_cpp.substr(ref_prefix.size(), target_cpp.size() - ref_prefix.size() - 1);
            return target_cpp + "(godot::Object::cast_to<" + object_cpp + ">(" + source_object +
                   "))";
        }
        return target_cpp + "(gdpp::runtime::to_variant(" + value + "))";
    }
    if (target.kind == TypeKind::object) {
        const auto target_cpp = cpp_type(target);
        const std::string ref_prefix{"godot::Ref<"};
        if (target_cpp.rfind(ref_prefix, 0) == 0 && target_cpp.back() == '>') {
            const auto object_cpp =
                target_cpp.substr(ref_prefix.size(), target_cpp.size() - ref_prefix.size() - 1);
            return "gdpp::runtime::strict_native_ref_storage<" + object_cpp +
                   ">(gdpp::runtime::to_variant(" + value + "), " + godot_string_name(target.name) +
                   location + ")";
        }
        const std::string storage_prefix{"gdpp::runtime::ObjectStorage<"};
        if (target_cpp.rfind(storage_prefix, 0) != 0 ||
            target_cpp.size() <= storage_prefix.size() || target_cpp.back() != '>') {
            diagnostics_.error("GDS3008",
                               "object type '" + target.display_name() +
                                   "' has no valid native storage representation",
                               location_span ? *location_span : SourceSpan{});
            return "gdpp::runtime::to_variant(" + value + ")";
        }
        const auto object_cpp =
            target_cpp.substr(storage_prefix.size(), target_cpp.size() - storage_prefix.size() - 1);
        return "gdpp::runtime::strict_native_object_value_storage<" + object_cpp +
               ">(gdpp::runtime::to_variant(" + value + "), " + godot_string_name(target.name) +
               location + ")";
    }
    const auto conversion = classify_conversion(target, source);
    if (conversion == ConversionKind::dynamic) {
        return "gdpp::runtime::strict_builtin_storage<" + cpp_type(target) +
               ">(gdpp::runtime::to_variant(" + value + "), " + variant_type(target) + location +
               ")";
    }
    if (conversion == ConversionKind::implicit) {
        return "static_cast<" + cpp_type(target) + ">(gdpp::runtime::to_variant(" + value + "))";
    }
    diagnostics_.error("GDS3008",
                       "unresolved conversion from " + source.display_name() + " to " +
                           target.display_name() + " reached code generation",
                       {});
    return value;
}

std::string CodeGenerator::emit_explicit_conversion(const Type& target, const Type& source,
                                                    std::string value) const {
    if (target == source)
        return value;
    if (is_explicitly_typed_container(target) && source.is_dynamic())
        return emit_conversion(target, source, std::move(value));
    if (is_explicitly_typed_container(target) && target != source) {
        // `as Array[T]` and `as Dictionary[K, V]` construct a container with the target runtime
        // signature. Returning only the untyped Array/Dictionary base loses both native and
        // attached-script element identity, and also cannot initialize the generated typed
        // wrapper because its validating constructors are intentionally explicit.
        return cpp_type(target) + "(gdpp::runtime::to_variant(" + value + "))";
    }
    if (target.kind == TypeKind::enumeration)
        return emit_conversion(target, source, std::move(value));
    const auto target_variant = variant_type(target);
    if (target_variant == "godot::Variant::NIL")
        return emit_conversion(target, source, std::move(value));
    return "gdpp::runtime::explicit_variant_cast<" + cpp_type(target) +
           ">(gdpp::runtime::to_variant(" + value + "), " + target_variant +
           (current_expression_span_ ? ", " + script_location(*current_expression_span_)
                                     : std::string{}) +
           ")";
}

std::string CodeGenerator::emit_parameter_default(const typed::Parameter& parameter) const {
    if (!parameter.default_value)
        return {};
    // C++ default arguments are evaluated in the caller and cannot reference the callee's
    // fields. Route every GDScript default through a private Variant marker so even typed,
    // instance-dependent expressions retain GDScript's per-invocation semantics.
    return "gdpp::runtime::default_argument()";
}

std::string CodeGenerator::parameter_native_type(const typed::Parameter& parameter) const {
    return parameter.default_value ? "godot::Variant" : cpp_type(parameter.type);
}

std::string CodeGenerator::parameter_native_name(const typed::Parameter& parameter) const {
    const auto name = sanitize_identifier(parameter.name);
    return parameter.default_value ? "_gdpp_argument_" + name : name;
}

std::string
CodeGenerator::emit_parameter_default_initializers(const std::vector<typed::Parameter>& parameters,
                                                   const std::size_t indent) const {
    std::string result;
    for (std::size_t index = 0; index < parameters.size(); ++index)
        result += emit_parameter_initializer(parameters[index], index, {}, indent, false);
    return result;
}

std::string CodeGenerator::emit_parameter_initializer(const typed::Parameter& parameter,
                                                      const std::size_t index,
                                                      const std::string_view arguments,
                                                      const std::size_t indentation,
                                                      const bool continuation_context) const {
    const auto prefix = indent(indentation);
    const auto source_name = sanitize_identifier(parameter.name);
    if (arguments.empty()) {
        if (!parameter.default_value)
            return {};
        if (!parameter.default_prefix.empty()) {
            diagnostics_.error("GDS3006",
                               "awaited parameter default reached the synchronous initializer",
                               parameter.span);
            return {};
        }
        const auto native_name = parameter_native_name(parameter);
        auto fallback = emit_conversion(parameter.type, emitted_type(*parameter.default_value),
                                        emit_expression(*parameter.default_value),
                                        &parameter.default_value->span);
        if (fallback == "{}") {
            const auto native_type = cpp_type(parameter.type);
            fallback =
                !native_type.empty() && native_type.back() == '*' ? "nullptr" : native_type + "{}";
        }
        if (parameter.type.is_dynamic())
            fallback = "gdpp::runtime::to_variant(" + fallback + ")";
        auto supplied = emit_conversion(parameter.type, {TypeKind::variant, "Variant"}, native_name,
                                        &parameter.span);
        const auto storage_type = cpp_type(parameter.type);
        // Both branches must have the exact declared storage representation. A Godot object
        // default can be a native pointer while the supplied Variant conversion is ObjectStorage;
        // leaving C++ to select a common type is ambiguous because both conversions are valid.
        if (!parameter.type.is_dynamic()) {
            fallback = storage_type + "(" + fallback + ")";
            supplied = storage_type + "(" + supplied + ")";
        }
        return prefix + storage_type + " " + source_name + " = " +
               "gdpp::runtime::is_default_argument(" + native_name + ") ? " + fallback + " : " +
               supplied + ";\n" + emit_script_failure_return(indentation, continuation_context);
    }

    if (!parameter.default_prefix.empty()) {
        diagnostics_.error("GDS3006", "awaited lambda default reached the synchronous initializer",
                           parameter.span);
        return {};
    }
    const auto argument = "gdpp::runtime::local_callable_argument<" + std::to_string(index) + ">(" +
                          std::string{arguments} + ")";
    const Type variant_type_descriptor{TypeKind::variant, "Variant"};
    const bool direct_builtin =
        !parameter.type.is_dynamic() && !describe_container_type(parameter.type) &&
        !parameter.type.is_packed_array() && parameter.type.kind != TypeKind::object &&
        classify_conversion(parameter.type, variant_type_descriptor) == ConversionKind::dynamic;
    const auto supplied =
        direct_builtin
            ? "gdpp::runtime::local_callable_typed_argument<" + cpp_type(parameter.type) + ", " +
                  std::to_string(index) + ">(" + std::string{arguments} + ", " +
                  variant_type(parameter.type) + ", " + script_location(parameter.span) + ")"
            : emit_conversion(parameter.type, variant_type_descriptor, argument, &parameter.span);
    std::string value = supplied;
    if (parameter.default_value) {
        value = std::string{arguments} + ".size() > " + std::to_string(index) + " ? " + supplied +
                " : " +
                emit_conversion(parameter.type, emitted_type(*parameter.default_value),
                                emit_expression(*parameter.default_value),
                                &parameter.default_value->span);
    }
    return prefix + "[[maybe_unused]] " + cpp_type(parameter.type) + " " + source_name + " = " +
           value + ";\n" + emit_script_failure_return(indentation, continuation_context);
}

bool CodeGenerator::has_parameter_control_flow(
    const std::vector<typed::Parameter>& parameters) noexcept {
    return std::any_of(parameters.begin(), parameters.end(),
                       [](const auto& parameter) { return !parameter.default_prefix.empty(); });
}

std::string CodeGenerator::emit_parameter_cells(const std::vector<typed::Parameter>& parameters,
                                                const std::size_t indentation) const {
    std::string result;
    for (const auto& parameter : parameters) {
        if (parameter.default_prefix.empty())
            continue;
        if (parameter.symbol_identity == 0) {
            diagnostics_.error("GDS3006",
                               "awaited parameter default is missing its lexical identity",
                               parameter.span);
            continue;
        }
        const auto cell = "_gdpp_parameter_cell_" + std::to_string(parameter.symbol_identity);
        result += indent(indentation) + "const auto " + cell + " = std::make_shared<" +
                  cpp_type(parameter.type) + ">();\n";
        local_expression_overrides_[parameter.symbol_identity] = "(*" + cell + ")";
    }
    return result;
}

std::string CodeGenerator::emit_parameter_initialization_chain(
    const std::vector<typed::Parameter>& parameters,
    const std::optional<typed::Parameter>& rest_parameter,
    const std::vector<typed::Statement>& body, const std::string_view arguments,
    const std::size_t indentation, const std::size_t begin, const bool continuation_context) const {
    std::string result;
    for (std::size_t index = begin; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        if (parameter.default_prefix.empty()) {
            result += emit_parameter_initializer(parameter, index, arguments, indentation,
                                                 continuation_context);
            continue;
        }

        const auto suffix = std::to_string(temporary_counter_++);
        const auto after = "_gdpp_after_parameter_" + suffix;
        const auto commit = "_gdpp_commit_parameter_" + suffix;
        const auto cell = "_gdpp_parameter_cell_" + std::to_string(parameter.symbol_identity);
        const auto prefix = indent(indentation);
        const auto next_index = index + 1U;
        const auto continuation = emit_parameter_initialization_chain(
            parameters, rest_parameter, body, arguments, indentation + 1U, next_index, true);
        result += prefix + "auto " + after + " = [=]() mutable {\n";
        result += continuation;
        result += prefix + "};\n";
        result += prefix + "auto " + commit +
                  " = [=](const godot::Variant &_gdpp_parameter_value) mutable {\n";
        result += indent(indentation + 1) + "(*" + cell + ") = " +
                  emit_conversion(parameter.type, {TypeKind::variant, "Variant"},
                                  "_gdpp_parameter_value", &parameter.span) +
                  ";\n";
        result += emit_script_failure_return(indentation + 1, true);
        result += indent(indentation + 1) + after + "();\n";
        result += prefix + "};\n";

        const auto supplied =
            arguments.empty()
                ? "!gdpp::runtime::is_default_argument(" + parameter_native_name(parameter) + ")"
                : std::string{arguments} + ".size() > " + std::to_string(index);
        const auto supplied_value = arguments.empty() ? parameter_native_name(parameter)
                                                      : "gdpp::runtime::local_callable_argument<" +
                                                            std::to_string(index) + ">(" +
                                                            std::string{arguments} + ")";
        result += prefix + "if (" + supplied + ") {\n";
        result += indent(indentation + 1) + commit + "(gdpp::runtime::to_variant(" +
                  supplied_value + "));\n";
        result += async_return(indentation + 1, continuation_context);
        result += prefix + "}\n";

        const auto terminal = commit + "(gdpp::runtime::to_variant(" +
                              emit_expression(*parameter.default_value) + "));";
        const auto prefix_suspends = std::any_of(
            parameter.default_prefix.begin(), parameter.default_prefix.end(),
            [this](const auto& statement) { return statement_contains_await(statement); });
        result += emit_async_statements(parameter.default_prefix, indentation, 0, {}, terminal,
                                        continuation_context);
        if (!prefix_suspends)
            result += async_return(indentation, continuation_context);
        return result;
    }

    if (!arguments.empty() && rest_parameter) {
        const auto rest_name = sanitize_identifier(rest_parameter->name);
        result += indent(indentation) + "[[maybe_unused]] godot::Array " + rest_name + ";\n";
        result += indent(indentation) + rest_name + ".resize(" + std::string{arguments} +
                  ".size() > " + std::to_string(parameters.size()) + " ? " +
                  std::string{arguments} + ".size() - " + std::to_string(parameters.size()) +
                  " : 0);\n";
        result += indent(indentation) +
                  "for (std::int64_t _gdpp_rest_index = " + std::to_string(parameters.size()) +
                  "; _gdpp_rest_index < " + std::string{arguments} +
                  ".size(); ++_gdpp_rest_index) " + rest_name + "[_gdpp_rest_index - " +
                  std::to_string(parameters.size()) + "] = " + std::string{arguments} +
                  "[static_cast<std::size_t>(_gdpp_rest_index)];\n";
    }
    result += emit_async_statements(body, indentation, 0, {}, {}, continuation_context);
    return result;
}

std::string CodeGenerator::emit_bound_parameter_defaults(
    const std::vector<typed::Parameter>& parameters) const {
    std::string result;
    for (const auto& parameter : parameters) {
        if (!parameter.default_value)
            continue;
        // The bound method receives the same marker as direct native calls and evaluates the
        // source expression inside the target instance.
        result += ", DEFVAL(gdpp::runtime::default_argument())";
    }
    return result;
}

std::string CodeGenerator::method_callback_name(const typed::Function& function) {
    return "_gdpp_variant_call_" + sanitize_identifier(function.name);
}

std::string CodeGenerator::emit_method_callback_declaration(const typed::Function& function) const {
    return "    static void " + method_callback_name(function) +
           "(void*, GDExtensionClassInstancePtr, const GDExtensionConstVariantPtr*, "
           "GDExtensionInt, GDExtensionVariantPtr, GDExtensionCallError*);\n";
}

std::string CodeGenerator::emit_method_callback_definition(
    const typed::Function& function, const std::string_view native_class,
    const std::string_view native_method, const std::string_view native_return_type) const {
    const auto callback = method_callback_name(function);
    const auto fixed_count = function.parameters.size();
    const auto required = static_cast<std::size_t>(
        std::count_if(function.parameters.begin(), function.parameters.end(),
                      [](const auto& parameter) { return !parameter.default_value; }));
    const auto fail_return = "godot::gdextension_interface::variant_new_nil(r_return); return;";
    std::ostringstream result;
    result << "void " << native_class << "::" << callback
           << "(void*, GDExtensionClassInstancePtr p_instance, "
              "const GDExtensionConstVariantPtr* p_args, GDExtensionInt p_argument_count, "
              "GDExtensionVariantPtr r_return, GDExtensionCallError* r_error) {\n"
           << "    r_error->error = GDEXTENSION_CALL_OK;\n"
           << "    r_error->argument = 0;\n"
           << "    r_error->expected = 0;\n"
           << "    (void)p_args;\n"
           << "    if (p_argument_count < " << required << ") {\n"
           << "        r_error->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;\n"
           << "        r_error->expected = " << required << ";\n"
           << "        " << fail_return << "\n"
           << "    }\n";
    if (!function.rest_parameter) {
        result << "    if (p_argument_count > " << fixed_count << ") {\n"
               << "        r_error->error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;\n"
               << "        r_error->expected = " << fixed_count << ";\n"
               << "        " << fail_return << "\n"
               << "    }\n";
    }
    if (!function.is_static) {
        result << "    if (!p_instance) {\n"
               << "        r_error->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;\n"
               << "        " << fail_return << "\n"
               << "    }\n";
    } else {
        result << "    (void)p_instance;\n";
    }
    if (fixed_count != 0) {
        result << "    std::array<const godot::Variant*, " << fixed_count << "> positional{};\n"
               << "    for (std::size_t index = 0; index < " << fixed_count
               << " && static_cast<GDExtensionInt>(index) < p_argument_count; ++index)\n"
               << "        positional[index] = "
                  "reinterpret_cast<const godot::Variant*>(p_args[index]);\n";
    }
    for (std::size_t index = 0; index < fixed_count; ++index) {
        const auto& parameter = function.parameters[index];
        const auto local = "_gdpp_positional_" + std::to_string(index);
        const auto native_type = parameter_native_type(parameter);
        result << "    " << native_type << ' ' << local << " = ";
        if (parameter.default_value) {
            result << "p_argument_count > " << index << " ? gdpp::runtime::to_variant(*positional["
                   << index << "]) : gdpp::runtime::default_argument();\n";
        } else if (parameter.type.is_dynamic()) {
            result << "gdpp::runtime::to_variant(*positional[" << index << "]);\n";
        } else {
            result << "godot::VariantCasterAndValidate<" << native_type
                   << ">::cast(positional.data(), " << index << ", *r_error);\n"
                   << "    if (r_error->error != GDEXTENSION_CALL_OK) { " << fail_return << " }\n";
        }
    }
    const auto rest_name = "_gdpp_rest_arguments";
    if (function.rest_parameter) {
        result << "    godot::Array " << rest_name << ";\n"
               << "    " << rest_name << ".resize(p_argument_count > " << fixed_count
               << " ? p_argument_count - " << fixed_count << " : 0);\n"
               << "    for (GDExtensionInt index = " << fixed_count
               << "; index < p_argument_count; ++index)\n"
               << "        " << rest_name << "[index - " << fixed_count
               << "] = *reinterpret_cast<const godot::Variant*>(p_args[index]);\n";
    }
    if (native_return_type != "void")
        result << "    godot::Variant result_value = gdpp::runtime::to_variant(";
    result << "    ";
    if (function.is_static)
        result << native_class << "::";
    else
        result << "static_cast<" << native_class << "*>(p_instance)->";
    result << native_method << '(';
    for (std::size_t index = 0; index < fixed_count; ++index) {
        if (index != 0)
            result << ", ";
        result << "_gdpp_positional_" << index;
    }
    if (function.rest_parameter) {
        if (fixed_count != 0)
            result << ", ";
        result << rest_name;
    }
    result << ')';
    if (native_return_type != "void")
        result << ')';
    result << ";\n";
    if (native_return_type == "void")
        result << "    godot::Variant result_value;\n";
    result << "    godot::gdextension_interface::variant_new_copy(r_return, "
              "result_value._native_ptr());\n"
           << "}\n\n";
    return result.str();
}

std::string
CodeGenerator::emit_method_registration(const typed::Function& function,
                                        const std::string_view native_class,
                                        const std::string_view native_return_type) const {
    std::ostringstream result;
    result << "    {\n"
           << "        godot::MethodInfo method(" << native_property_info(function.return_type, "")
           << ", " << godot_string_name(function.name);
    for (const auto& parameter : function.parameters)
        result << ", " << native_property_info(parameter.type, parameter.name);
    result << ");\n";
    if (function.is_static)
        result << "        method.flags |= GDEXTENSION_METHOD_FLAG_STATIC;\n";
    for (const auto& parameter : function.parameters) {
        if (parameter.default_value) {
            result << "        method.default_arguments.push_back("
                      "gdpp::runtime::default_argument());\n";
        }
    }
    result << "        gdpp::runtime::bind_"
           << (function.rest_parameter ? "vararg_method(" : "variant_method(") << native_class
           << "::get_class_static(), method, &" << native_class
           << "::" << method_callback_name(function) << ", "
           << (native_return_type == "void" ? "false" : "true") << ");\n"
           << "    }\n";
    return result.str();
}

std::string CodeGenerator::emit_api_argument(std::string_view api_type,
                                             std::string_view native_meta, const Type& source,
                                             std::string value) const {
    const auto comma = api_type.find(',');
    if (comma != std::string_view::npos)
        api_type = api_type.substr(0, comma);
    while (!api_type.empty() && api_type.front() == '-')
        api_type.remove_prefix(1);
    const bool bitfield = api_type.rfind("bitfield::", 0) == 0;
    if (bitfield || api_type.rfind("enum::", 0) == 0) {
        api_type.remove_prefix(bitfield ? std::string_view{"bitfield::"}.size()
                                        : std::string_view{"enum::"}.size());
        std::string cpp_name{api_type};
        std::replace(cpp_name.begin(), cpp_name.end(), '.', ':');
        for (std::size_t position = 0;
             (position = cpp_name.find(':', position)) != std::string::npos; position += 2) {
            if (position + 1 >= cpp_name.size() || cpp_name[position + 1] != ':')
                cpp_name.insert(position, 1, ':');
        }
        const auto integer = "static_cast<int64_t>(gdpp::runtime::to_variant(" + value + "))";
        if (bitfield)
            return "godot::BitField<godot::" + cpp_name + ">(" + integer + ")";
        return "static_cast<godot::" + cpp_name + ">(" + integer + ")";
    }
    const auto target = type_from_godot_api(api_type);
    auto converted = emit_conversion(target, source, std::move(value));
    if (target.is_packed_array())
        converted = "gdpp::runtime::packed_native_argument(" + converted + ")";
    const auto native_type = godot_native_scalar_cpp_type(godot_native_meta(native_meta));
    if (!native_type.empty())
        return "static_cast<" + std::string{native_type} + ">(" + converted + ")";
    return converted;
}

std::string CodeGenerator::emit_api_return(const Type& target, std::string value) const {
    // extension_api.json exposes integer and float as GDScript's fixed semantic types while the
    // generated godot-cpp method can use uint32_t, uint64_t, float or real_t. Make that ABI
    // boundary explicit so strict GCC/MSVC builds do not rely on an implicit narrowing or signed
    // conversion. Non-scalar Godot values already use the exact generated wrapper type.
    if (target.kind == TypeKind::integer || target.kind == TypeKind::enumeration)
        return "static_cast<int64_t>(" + value + ")";
    if (target.kind == TypeKind::floating)
        return "static_cast<double>(" + value + ")";
    if (target.kind == TypeKind::boolean)
        return "static_cast<bool>(" + value + ")";
    if (target.is_packed_array())
        return cpp_type(target) + "(" + value + ")";
    return value;
}

std::string CodeGenerator::emit_subscript_read(const Type& container, const Type& result,
                                               std::string target, std::string index,
                                               const SourceSpan span) const {
    const auto source_location = ", _gdpp_source_path, " + std::to_string(span.begin.line) + ", " +
                                 std::to_string(span.begin.column);
    std::string value;
    if (container.kind == TypeKind::array) {
        const bool native_value = !result.is_dynamic() && result.kind != TypeKind::object &&
                                  result.kind != TypeKind::script_resource;
        if (native_value) {
            value = "gdpp::runtime::checked_typed_array_get<" + cpp_type(result) + ">(" + target +
                    ", " + index + source_location + ")";
            return emit_conversion(result, result, std::move(value));
        }
        value = "gdpp::runtime::checked_array_get(" + target + ", " + index + source_location + ")";
    } else if (container.is_packed_array()) {
        value = "gdpp::runtime::checked_packed_array_get(" + target + ", " + index +
                source_location + ")";
        return emit_conversion(result, result, std::move(value));
    } else if (container.kind == TypeKind::dictionary) {
        value = "gdpp::runtime::checked_dictionary_get(" + target + ", gdpp::runtime::to_variant(" +
                index + "), " + script_location(span) + ")";
    } else {
        value = "gdpp::runtime::get_key(gdpp::runtime::to_variant(" + target +
                "), gdpp::runtime::to_variant(" + index + "), " + script_location(span) + ")";
    }
    return emit_conversion(result, {TypeKind::variant, "Variant"}, std::move(value));
}

std::string CodeGenerator::emit_subscript_store(const Type& container, std::string value) const {
    std::string native_type;
    if (container.name == "PackedByteArray")
        native_type = "uint8_t";
    else if (container.name == "PackedInt32Array")
        native_type = "int32_t";
    else if (container.name == "PackedInt64Array")
        native_type = "int64_t";
    else if (container.name == "PackedFloat32Array")
        native_type = "float";
    else if (container.name == "PackedFloat64Array")
        native_type = "double";
    if (!native_type.empty())
        return "static_cast<" + native_type + ">(" + value + ")";
    return value;
}

std::string CodeGenerator::emit_storage_assignment(const Type& target_type, std::string target,
                                                   std::string value) const {
    const bool reference_backed_builtin =
        target_type.kind == TypeKind::string || target_type.kind == TypeKind::string_name ||
        target_type.kind == TypeKind::array || target_type.kind == TypeKind::dictionary ||
        (target_type.kind == TypeKind::builtin &&
         (target_type.name == "NodePath" || target_type.name == "Callable" ||
          target_type.name == "Signal" || target_type.is_packed_array()));
    if (reference_backed_builtin)
        return "gdpp::runtime::assign_native_storage(" + std::move(target) + ", " +
               std::move(value) + ")";
    return std::move(target) + " = " + std::move(value);
}

std::string CodeGenerator::emit_direct_builtin_member(std::string_view owner, std::string object,
                                                      std::string_view member,
                                                      const SourceSpan* source_span) const {
    const auto component_index = [](std::string_view name) -> int {
        if (name == "x")
            return 0;
        if (name == "y")
            return 1;
        if (name == "z")
            return 2;
        return name == "w" ? 3 : -1;
    };
    const auto index = component_index(member);
    std::string result;
    if (owner == "Basis" && index >= 0)
        result = object + ".get_column(" + std::to_string(index) + ")";
    else if (owner == "Projection" && index >= 0)
        result = object + "[" + std::to_string(index) + "]";
    else if (owner == "Transform2D" && index >= 0 && index < 2)
        result = object + "[" + std::to_string(index) + "]";
    else if (owner == "Plane" && index >= 0 && index < 3)
        result = object + ".normal." + std::string{member};
    else if (owner == "Transform2D" && member == "origin")
        result = object + ".get_origin()";
    else if ((owner == "Rect2" || owner == "Rect2i" || owner == "AABB") && member == "end")
        result = object + ".get_end()";
    else if (owner == "Color" &&
             (member == "r8" || member == "g8" || member == "b8" || member == "a8" ||
              member == "h" || member == "s" || member == "v"))
        result = object + ".get_" + std::string{member} + "()";
    else if (owner == "Color" &&
             (member == "ok_hsl_h" || member == "ok_hsl_s" || member == "ok_hsl_l")) {
        const auto* location_span = source_span ? source_span : current_expression_span_;
        const auto location = location_span ? script_location(*location_span)
                                            : "gdpp::runtime::ScriptSourceLocation{}";
        result = "gdpp::runtime::get_builtin_member(" + object + ", " +
                 godot_string_name(std::string{member}) + ", " + location + ")";
    } else
        result = object + "." + sanitize_identifier(std::string{member});
    if (const auto* property = api_.find_property(owner, member))
        return emit_api_return(api_.property_getter_type(*property), std::move(result));
    return result;
}

std::string CodeGenerator::emit_direct_builtin_assignment(std::string_view owner,
                                                          std::string object,
                                                          std::string_view member,
                                                          std::string value,
                                                          const SourceSpan* source_span) const {
    const auto component_index = [](std::string_view name) -> int {
        if (name == "x")
            return 0;
        if (name == "y")
            return 1;
        if (name == "z")
            return 2;
        return name == "w" ? 3 : -1;
    };
    const auto index = component_index(member);
    if (owner == "Basis" && index >= 0)
        return object + ".set_column(" + std::to_string(index) + ", " + value + ")";
    if (owner == "Projection" && index >= 0)
        return object + "[" + std::to_string(index) + "] = " + value;
    if (owner == "Transform2D" && index >= 0 && index < 2)
        return object + "[" + std::to_string(index) + "] = " + value;
    if (owner == "Plane" && index >= 0 && index < 3) {
        return object + ".normal." + std::string{member} + " = static_cast<godot::real_t>(" +
               value + ")";
    }
    if (owner == "Transform2D" && member == "origin")
        return object + ".set_origin(" + value + ")";
    if ((owner == "Rect2" || owner == "Rect2i" || owner == "AABB") && member == "end")
        return object + ".set_end(" + value + ")";
    if ((owner == "Vector2i" || owner == "Vector3i" || owner == "Vector4i") && index >= 0) {
        return object + "." + std::string{member} + " = static_cast<int32_t>(" + value + ")";
    }
    if ((owner == "Vector2" || owner == "Vector3" || owner == "Vector4" || owner == "Quaternion") &&
        index >= 0) {
        return object + "." + std::string{member} + " = static_cast<godot::real_t>(" + value + ")";
    }
    if (owner == "Plane" && member == "d")
        return object + ".d = static_cast<godot::real_t>(" + value + ")";
    if (owner == "Color" && (member == "r" || member == "g" || member == "b" || member == "a")) {
        return object + "." + std::string{member} + " = static_cast<float>(" + value + ")";
    }
    if (owner == "Color" &&
        (member == "r8" || member == "g8" || member == "b8" || member == "a8")) {
        return object + ".set_" + std::string{member} + "(static_cast<int32_t>(" + value + "))";
    }
    if (owner == "Color" && (member == "h" || member == "s" || member == "v")) {
        return object + ".set_" + std::string{member} + "(static_cast<float>(" + value + "))";
    }
    if (owner == "Color" &&
        (member == "ok_hsl_h" || member == "ok_hsl_s" || member == "ok_hsl_l")) {
        const auto* location_span = source_span ? source_span : current_expression_span_;
        const auto location = location_span ? script_location(*location_span)
                                            : "gdpp::runtime::ScriptSourceLocation{}";
        return "gdpp::runtime::set_builtin_member(" + object + ", " +
               godot_string_name(std::string{member}) + ", gdpp::runtime::to_variant(" + value +
               "), " + location + ")";
    }
    return object + "." + sanitize_identifier(std::string{member}) + " = " + value;
}

std::string CodeGenerator::emit_dynamic_assignment(const typed::Statement& statement,
                                                   std::size_t indentation) const {
    const auto& target = *statement.condition;
    const auto prefix = indent(indentation);
    const auto nested_prefix = indent(indentation + 1);
    const auto suffix = std::to_string(temporary_counter_++);

    if (target.kind == typed::ExpressionKind::identifier) {
        const auto owner = "_gdpp_dynamic_owner_" + suffix;
        const auto value = "_gdpp_dynamic_value_" + suffix;
        std::string result = prefix + "{\n" + nested_prefix + "godot::Variant " + owner +
                             " = gdpp::runtime::to_variant(" + self_object_expression() + ");\n" +
                             nested_prefix + "godot::Variant " + value +
                             " = gdpp::runtime::to_variant(" +
                             emit_expression(*statement.expression) + ");\n" +
                             emit_script_failure_return(indentation + 1, in_async_continuation_);
        if (statement.operation != "=") {
            const auto operation = statement.operation.substr(0, statement.operation.size() - 1);
            result += nested_prefix + value +
                      " = gdpp::runtime::binary(godot::Variant::" + variant_operator(operation) +
                      ", gdpp::runtime::get_named(" + owner + ", " +
                      godot_string_name(target.value) + ", " + script_location(target.span) +
                      "), " + value + ", " + script_location(target.span) + ");\n";
            result += emit_script_failure_return(indentation + 1, in_async_continuation_);
        }
        result += nested_prefix + "gdpp::runtime::set_named(" + owner + ", " +
                  godot_string_name(target.value) + ", " + value + ", " +
                  script_location(target.span) + ");\n" + prefix + "}\n";
        return result;
    }

    // A Variant access may cross value types such as Vector2, Color or Transform3D. Godot returns
    // those values by copy, so changing only the leaf silently loses the mutation. Preserve the
    // complete lvalue chain and write each changed child back into its parent in reverse order.
    std::vector<const typed::Expression*> access_chain;
    const typed::Expression* root = &target;
    while ((root->kind == typed::ExpressionKind::member ||
            root->kind == typed::ExpressionKind::subscript) &&
           !root->operands.empty()) {
        access_chain.push_back(root);
        root = root->operands.front().get();
    }
    std::reverse(access_chain.begin(), access_chain.end());
    if (access_chain.empty()) {
        diagnostics_.error("GDS3007", "dynamic assignment is missing an access chain", target.span);
        return prefix + "/* invalid dynamic assignment */;\n";
    }

    const auto root_name = "_gdpp_dynamic_root_" + suffix;
    std::string result = prefix + "{\n" + nested_prefix + "godot::Variant " + root_name +
                         " = gdpp::runtime::to_variant(" + emit_expression(*root) + ");\n" +
                         emit_script_failure_return(indentation + 1, in_async_continuation_);
    std::vector<std::string> containers{root_name};
    std::vector<std::string> keys(access_chain.size());

    for (std::size_t index = 0; index + 1 < access_chain.size(); ++index) {
        const auto* access = access_chain[index];
        const auto child_name = "_gdpp_dynamic_child_" + suffix + "_" + std::to_string(index);
        if (access->kind == typed::ExpressionKind::subscript) {
            keys[index] = "_gdpp_dynamic_key_" + suffix + "_" + std::to_string(index);
            result += nested_prefix + "const godot::Variant " + keys[index] + " = " +
                      "gdpp::runtime::to_variant(" + emit_expression(*access->operands.at(1)) +
                      ");\n" + emit_script_failure_return(indentation + 1, in_async_continuation_) +
                      nested_prefix + "godot::Variant " + child_name +
                      " = gdpp::runtime::get_key(" + containers.back() + ", " + keys[index] + ", " +
                      script_location(access->span) + ");\n" +
                      emit_script_failure_return(indentation + 1, in_async_continuation_);
        } else {
            result += nested_prefix + "godot::Variant " + child_name +
                      " = gdpp::runtime::get_named(" + containers.back() + ", " +
                      godot_string_name(access->value) + ", " + script_location(access->span) +
                      ");\n" + emit_script_failure_return(indentation + 1, in_async_continuation_);
        }
        containers.push_back(child_name);
    }

    const auto* leaf = access_chain.back();
    if (leaf->kind == typed::ExpressionKind::subscript) {
        const auto leaf_index = access_chain.size() - 1;
        keys[leaf_index] = "_gdpp_dynamic_key_" + suffix + "_" + std::to_string(leaf_index);
        result += nested_prefix + "const godot::Variant " + keys[leaf_index] + " = " +
                  "gdpp::runtime::to_variant(" + emit_expression(*leaf->operands.at(1)) + ");\n" +
                  emit_script_failure_return(indentation + 1, in_async_continuation_);
    }

    const bool compound = statement.operation != "=";
    const auto value_name = "_gdpp_dynamic_value_" + suffix;
    std::string current_name;
    if (compound) {
        current_name = "_gdpp_dynamic_current_" + suffix;
        result += nested_prefix + "const godot::Variant " + current_name + " = ";
        if (leaf->kind == typed::ExpressionKind::subscript) {
            result += "gdpp::runtime::get_key(" + containers.back() + ", " + keys.back() + ", " +
                      script_location(leaf->span) + ");\n";
        } else {
            result += "gdpp::runtime::get_named(" + containers.back() + ", " +
                      godot_string_name(leaf->value) + ", " + script_location(leaf->span) + ");\n";
        }
        result += emit_script_failure_return(indentation + 1, in_async_continuation_);
    }
    result += nested_prefix + "const godot::Variant " + value_name + " = " +
              "gdpp::runtime::to_variant(" + emit_expression(*statement.expression) + ");\n" +
              emit_script_failure_return(indentation + 1, in_async_continuation_);
    std::string assigned_value = value_name;
    if (compound) {
        const auto operation = statement.operation.substr(0, statement.operation.size() - 1);
        assigned_value = "gdpp::runtime::binary(godot::Variant::" + variant_operator(operation) +
                         ", " + current_name + ", " + value_name + ", " +
                         script_location(target.span) + ")";
        const auto assigned_name = "_gdpp_dynamic_assigned_" + suffix;
        result += nested_prefix + "const godot::Variant " + assigned_name + " = " + assigned_value +
                  ";\n" + emit_script_failure_return(indentation + 1, in_async_continuation_);
        assigned_value = assigned_name;
    }

    if (leaf->kind == typed::ExpressionKind::subscript) {
        result += nested_prefix + "gdpp::runtime::set_key(" + containers.back() + ", " +
                  keys.back() + ", " + assigned_value + ", " + script_location(leaf->span) + ");\n";
    } else {
        result += nested_prefix + "gdpp::runtime::set_named(" + containers.back() + ", " +
                  godot_string_name(leaf->value) + ", " + assigned_value + ", " +
                  script_location(leaf->span) + ");\n";
    }
    result += emit_script_failure_return(indentation + 1, in_async_continuation_);

    for (std::size_t child_index = containers.size() - 1; child_index > 0; --child_index) {
        const auto access_index = child_index - 1;
        const auto* access = access_chain[access_index];
        if (access->kind == typed::ExpressionKind::subscript) {
            result += nested_prefix + "gdpp::runtime::set_key(" + containers[child_index - 1] +
                      ", " + keys[access_index] + ", " + containers[child_index] + ", " +
                      script_location(access->span) + ");\n";
        } else {
            result += nested_prefix + "gdpp::runtime::set_named(" + containers[child_index - 1] +
                      ", " + godot_string_name(access->value) + ", " + containers[child_index] +
                      ", " + script_location(access->span) + ");\n";
        }
        result += emit_script_failure_return(indentation + 1, in_async_continuation_);
    }

    // The lvalue's declared storage, rather than a flow-refined expression view, determines
    // whether the root must be rebound. Object references and shared containers are mutated
    // through their Variant identity. Dynamic and ordinary value storage must receive the
    // reconstructed root so Variant-held builtins and copy-valued members are preserved.
    const auto& root_storage_type =
        root->storage_type.kind == TypeKind::unknown ? root->type : root->storage_type;
    const auto root_ownership = root_storage_type.ownership();
    const bool root_requires_writeback = root->kind == typed::ExpressionKind::identifier &&
                                         root_ownership != OwnershipKind::object_reference &&
                                         root_ownership != OwnershipKind::shared_container;
    if (root_requires_writeback) {
        auto converted =
            emit_conversion(root->type, {TypeKind::variant, "Variant"}, root_name, &root->span);
        if (root->resolution == typed::ResolutionKind::script_property) {
            result += nested_prefix + root->setter + "(" + converted + ");\n";
        } else if (root->resolution == typed::ResolutionKind::godot_property &&
                   !root->direct_access && !root->setter.empty()) {
            std::string index;
            const auto* setter = api_.find_method(root->resolved_owner, root->setter);
            if (root->indexed_argument >= 0) {
                index = std::to_string(root->indexed_argument);
                if (setter) {
                    if (const auto* argument = api_.argument(*setter, 0))
                        index = emit_api_argument(argument->type, argument->meta,
                                                  {TypeKind::integer, "int"}, std::move(index));
                }
                index += ", ";
            }
            if (setter) {
                const auto value_index = root->indexed_argument >= 0 ? 1U : 0U;
                if (const auto* argument = api_.argument(*setter, value_index))
                    converted = emit_api_argument(argument->type, argument->meta, root->type,
                                                  std::move(converted));
            }
            result += nested_prefix +
                      (attached_script_ ? godot_owner_expression() + "->" : std::string{}) +
                      root->setter + "(" + index + converted + ");\n";
        } else {
            result += nested_prefix + emit_expression(*root) + " = " + converted + ";\n";
        }
    }
    return result + prefix + "}\n";
}

std::string CodeGenerator::emit_dictionary_member_assignment(const typed::Statement& statement,
                                                             const std::size_t indentation) const {
    const auto& target = *statement.condition;
    const auto prefix = indent(indentation);
    const auto nested_prefix = indent(indentation + 1);
    const auto suffix = std::to_string(temporary_counter_++);
    const auto dictionary = "_gdpp_dictionary_target_" + suffix;
    const auto key = "_gdpp_dictionary_key_" + suffix;
    const auto value = "_gdpp_dictionary_value_" + suffix;

    const auto& receiver = *target.operands.at(0);
    const auto proven =
        receiver.kind == typed::ExpressionKind::identifier && receiver.symbol_identity != 0 &&
        proven_local_dictionary_slots_.find(receiver.symbol_identity) !=
            proven_local_dictionary_slots_.end() &&
        proven_local_dictionary_slots_.at(receiver.symbol_identity).find(target.value) !=
            proven_local_dictionary_slots_.at(receiver.symbol_identity).end();
    std::string result = prefix + "{\n" + nested_prefix + "auto &&" + dictionary + " = " +
                         emit_expression(receiver) + ";\n" + nested_prefix;
    if (statement.operation != "=" && proven) {
        const auto slot = "_gdpp_dictionary_slot_" + suffix;
        result += "const auto& " + key +
                  " = *gdpp::runtime::engine_lifetime_static_ptr([] { return godot::Variant{" +
                  godot_string_name(target.value) + "}; });\n" + nested_prefix + "godot::Variant &" +
                  slot + " = " + dictionary + "[" + key + "];\n" + nested_prefix + "const auto " +
                  value + " = " +
                  emit_expression(*statement.expression) + ";\n" +
                  emit_script_failure_return(indentation + 1, in_async_continuation_);
        const auto operation = statement.operation.substr(0, statement.operation.size() - 1);
        if (statement.expression->type.kind == TypeKind::integer ||
            statement.expression->type.kind == TypeKind::enumeration) {
            result += nested_prefix + "gdpp::runtime::compound_assign_integer(" + slot +
                      ", godot::Variant::" + variant_operator(operation) + ", " + value + ", " +
                      script_location(target.span) + ");\n";
        } else {
            result += nested_prefix + "gdpp::runtime::compound_assign(" + slot +
                      ", godot::Variant::" + variant_operator(operation) + ", " + value + ", " +
                      script_location(target.span) + ");\n";
        }
        result += emit_script_failure_return(indentation + 1, in_async_continuation_);
    } else if (statement.operation != "=") {
        const auto current = "_gdpp_dictionary_current_" + suffix;
        result += "const auto& " + key +
                  " = *gdpp::runtime::engine_lifetime_static_ptr([] { return " +
                  godot_string_name(target.value) + "; });\n" + nested_prefix + "godot::Variant " +
                  current + " = gdpp::runtime::checked_dictionary_get_named(" + dictionary +
                  ", " + key + ", " + script_location(target.span) + ");\n" +
                  emit_script_failure_return(indentation + 1, in_async_continuation_) +
                  nested_prefix + "const auto " + value + " = " +
                  emit_expression(*statement.expression) + ";\n" +
                  emit_script_failure_return(indentation + 1, in_async_continuation_);
        const auto operation = statement.operation.substr(0, statement.operation.size() - 1);
        if (statement.expression->type.kind == TypeKind::integer ||
            statement.expression->type.kind == TypeKind::enumeration) {
            result += nested_prefix + "gdpp::runtime::compound_assign_integer(" + current +
                      ", godot::Variant::" + variant_operator(operation) + ", " + value + ", " +
                      script_location(target.span) + ");\n";
        } else {
            result += nested_prefix + "gdpp::runtime::compound_assign(" + current +
                      ", godot::Variant::" + variant_operator(operation) + ", " + value + ", " +
                      script_location(target.span) + ");\n";
        }
        result += emit_script_failure_return(indentation + 1, in_async_continuation_) +
                  nested_prefix + "gdpp::runtime::unchecked_dictionary_set_named(" + dictionary +
                  ", " + key + ", " + current + ", " + script_location(target.span) + ");\n" +
                  emit_script_failure_return(indentation + 1, in_async_continuation_);
    } else {
        result += "const auto& " + key +
                  " = *gdpp::runtime::engine_lifetime_static_ptr([] { return " +
                  godot_string_name(target.value) + "; });\n" + nested_prefix +
                  "const godot::Variant " + value + " = " + "gdpp::runtime::to_variant(" +
                  emit_expression(*statement.expression) + ");\n" +
                  emit_script_failure_return(indentation + 1, in_async_continuation_) +
                  nested_prefix + "gdpp::runtime::checked_dictionary_set_named(" + dictionary +
                  ", " + key + ", " + value + ", " + script_location(target.span) + ");\n" +
                  emit_script_failure_return(indentation + 1, in_async_continuation_);
    }
    return result + prefix + "}\n";
}

std::string CodeGenerator::emit_truthy(const typed::Expression& expression) const {
    auto value = emit_expression(expression);
    switch (expression.type.truthiness()) {
    case TruthinessKind::invalid:
        diagnostics_.error("GDS3007", "void expression reached a generated boolean context",
                           expression.span);
        return "false";
    case TruthinessKind::always_false:
        return "(static_cast<void>(" + value + "), false)";
    case TruthinessKind::zero_value:
    case TruthinessKind::dynamic_value:
        if (expression.type.kind == TypeKind::boolean)
            return "static_cast<bool>(" + value + ")";
        return "(gdpp::runtime::to_variant(" + value + ")).booleanize()";
    case TruthinessKind::object_validity:
        break;
    }
    if (expression.type.kind == TypeKind::object) {
        if (script_symbols_ && script_symbols_->find_external(expression.type.name))
            return "(gdpp::runtime::to_variant(" + value + ")).booleanize()";
        const bool ref_counted = is_ref_counted_object(expression.type);
        if (value == "this")
            return "true";
        if (attached_script_ && value == self_object_expression())
            return "(" + value + " != nullptr)";
        return ref_counted ? "(" + value + ").is_valid()" : "(" + value + " != nullptr)";
    }
    return "(gdpp::runtime::to_variant(" + value + ")).booleanize()";
}

bool CodeGenerator::conversion_may_fail(const Type& target, const Type& source) const {
    if (target.kind == TypeKind::void_type || target.is_dynamic() || target == source)
        return false;
    if (target.kind == TypeKind::enumeration && source.kind == TypeKind::enumeration)
        return false;
    if (source.kind == TypeKind::nil && target.kind == TypeKind::object)
        return false;
    if (source.kind == TypeKind::nil && target.kind == TypeKind::script_resource)
        return false;
    if (target.kind == TypeKind::object && target.name == "GDScript" && target != source)
        return true;

    // Dynamic-to-static storage is the principal checked conversion boundary. Packed arrays and
    // typed containers also validate their element contract even when the source itself is
    // statically known. All other accepted static conversions lower to native casts, object
    // identity casts, or value-wrapper construction and cannot set the GDPP script-fault state.
    if (source.is_dynamic())
        return true;
    if (target.is_packed_array() && target != source)
        return true;
    const bool constrained_container =
        (target.kind == TypeKind::array && target.name != "Array") ||
        (target.kind == TypeKind::dictionary && target.name != "Dictionary");
    return constrained_container && target != source;
}

bool CodeGenerator::expression_may_fail(const typed::Expression& expression) const {
    const auto any_operand_may_fail = [&]() {
        return std::any_of(
            expression.operands.begin(), expression.operands.end(),
            [&](const auto& operand) { return operand && expression_may_fail(*operand); });
    };

    switch (expression.kind) {
    case typed::ExpressionKind::literal:
    case typed::ExpressionKind::lambda:
        return false;
    case typed::ExpressionKind::identifier:
        switch (expression.resolution) {
        case typed::ResolutionKind::external_singleton:
        case typed::ResolutionKind::external_callable:
        case typed::ResolutionKind::external_signal:
        case typed::ResolutionKind::godot_callable:
        case typed::ResolutionKind::dynamic_method:
        case typed::ResolutionKind::dynamic_property:
        case typed::ResolutionKind::script_property:
        case typed::ResolutionKind::script_runtime_static_field:
            return true;
        case typed::ResolutionKind::none:
        case typed::ResolutionKind::godot_method:
        case typed::ResolutionKind::godot_property:
        case typed::ResolutionKind::godot_constructor:
        case typed::ResolutionKind::godot_singleton:
        case typed::ResolutionKind::godot_type:
        case typed::ResolutionKind::external_type:
        case typed::ResolutionKind::script_type:
        case typed::ResolutionKind::script_autoload:
        case typed::ResolutionKind::script_constant:
        case typed::ResolutionKind::local_constant:
        case typed::ResolutionKind::script_enum_type:
        case typed::ResolutionKind::script_resource:
        case typed::ResolutionKind::script_constructor:
        case typed::ResolutionKind::external_constructor:
        case typed::ResolutionKind::external_static_method:
        case typed::ResolutionKind::external_super_method:
        case typed::ResolutionKind::inner_constructor:
        case typed::ResolutionKind::inner_type:
        case typed::ResolutionKind::script_super:
        case typed::ResolutionKind::script_signal:
        case typed::ResolutionKind::script_callable:
        case typed::ResolutionKind::script_static_callable:
        case typed::ResolutionKind::script_static_field:
        case typed::ResolutionKind::script_free:
        case typed::ResolutionKind::enum_member:
        case typed::ResolutionKind::utility_function:
        case typed::ResolutionKind::global_constant:
        case typed::ResolutionKind::global_enum_type:
        case typed::ResolutionKind::global_enum_value:
        case typed::ResolutionKind::builtin_constant:
        case typed::ResolutionKind::intrinsic:
            break;
        }
        return expression.storage_type.kind != TypeKind::unknown &&
               expression.storage_type != expression.type &&
               conversion_may_fail(expression.type, expression.storage_type);
    case typed::ExpressionKind::unary: {
        if (any_operand_may_fail())
            return true;
        if (expression.operands.empty())
            return true;
        const auto& operand = *expression.operands.front();
        const bool integer_like =
            operand.type.kind == TypeKind::integer || operand.type.kind == TypeKind::enumeration;
        if (expression.value == "not" ||
            (integer_like && (expression.value == "-" || expression.value == "~")) ||
            ((expression.value == "+" || expression.value == "-") && operand.type.is_numeric()))
            return false;
        return true;
    }
    case typed::ExpressionKind::await_expression:
    case typed::ExpressionKind::node_reference:
    case typed::ExpressionKind::subscript:
        return true;
    case typed::ExpressionKind::binary: {
        if (any_operand_may_fail())
            return true;
        if (expression.value == "and" || expression.value == "or" || expression.value == "is" ||
            expression.value == "is not")
            return false;
        if (expression.value == "as" || expression.value == "in" || expression.value == "not in" ||
            expression.value == "**")
            return true;
        if (expression.operands.size() < 2)
            return true;
        const auto& left = expression.operands.at(0)->type;
        const auto& right = expression.operands.at(1)->type;
        const bool integer_left =
            left.kind == TypeKind::integer || left.kind == TypeKind::enumeration;
        const bool integer_right =
            right.kind == TypeKind::integer || right.kind == TypeKind::enumeration;
        if (integer_left && integer_right)
            return expression.value == "/" || expression.value == "%";
        if (left.is_dynamic() || right.is_dynamic())
            return true;
        if (left.kind == TypeKind::builtin || right.kind == TypeKind::builtin)
            return true;
        if (left.is_numeric() && right.is_numeric())
            return false;
        return !((left.kind == TypeKind::string && right.kind == TypeKind::string) ||
                 (left.kind == TypeKind::string_name && right.kind == TypeKind::string_name) ||
                 (left.kind == TypeKind::boolean && right.kind == TypeKind::boolean) ||
                 ((left.kind == TypeKind::object || left.kind == TypeKind::nil) &&
                  (right.kind == TypeKind::object || right.kind == TypeKind::nil)));
    }
    case typed::ExpressionKind::call: {
        if (any_operand_may_fail() || expression.operands.empty())
            return true;
        const auto& callee = *expression.operands.front();
        if (expression.resolution == typed::ResolutionKind::script_resource)
            return false;
        // String methods execute entirely inside a value wrapper: unlike Object, Callable,
        // Signal, Array, and Dictionary methods, they cannot re-enter project code or report a
        // GDPP runtime fault. This is deliberately a narrow proof, not a list of benchmark names.
        if (callee.resolution == typed::ResolutionKind::godot_method &&
            callee.kind == typed::ExpressionKind::member && !callee.operands.empty() &&
            (callee.operands.front()->type.kind == TypeKind::string ||
             callee.operands.front()->type.kind == TypeKind::string_name)) {
            const auto* method = api_.find_method(callee.resolved_owner, callee.value);
            if (!method)
                return true;
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                const auto* argument = api_.argument(*method, index - 1);
                if (!argument || conversion_may_fail(type_from_godot_api(argument->type),
                                                     expression.operands[index]->type)) {
                    return true;
                }
            }
            return false;
        }
        if (callee.resolution == typed::ResolutionKind::godot_constructor &&
            expression.operands.size() == 2) {
            return conversion_may_fail(expression.type, expression.operands.at(1)->type);
        }
        return true;
    }
    case typed::ExpressionKind::member:
        if (any_operand_may_fail())
            return true;
        if (expression.resolution == typed::ResolutionKind::dynamic_property &&
            !expression.operands.empty() &&
            expression.operands.front()->type.kind == TypeKind::dictionary)
            return true;
        if (expression.resolution == typed::ResolutionKind::godot_method ||
            expression.resolution == typed::ResolutionKind::script_callable ||
            expression.resolution == typed::ResolutionKind::script_signal ||
            expression.resolution == typed::ResolutionKind::enum_member ||
            expression.resolution == typed::ResolutionKind::builtin_constant ||
            expression.resolution == typed::ResolutionKind::global_constant ||
            expression.resolution == typed::ResolutionKind::global_enum_value ||
            expression.resolution == typed::ResolutionKind::global_enum_type ||
            expression.resolution == typed::ResolutionKind::script_constant ||
            expression.resolution == typed::ResolutionKind::script_enum_type ||
            expression.resolution == typed::ResolutionKind::inner_type)
            return false;
        if (expression.resolution == typed::ResolutionKind::godot_property &&
            expression.direct_access && !expression.operands.empty() &&
            expression.operands.front()->type.kind == TypeKind::builtin)
            return false;
        return true;
    case typed::ExpressionKind::conditional:
        if (any_operand_may_fail() || expression.operands.size() < 3)
            return true;
        return conversion_may_fail(expression.type, expression.operands.at(0)->type) ||
               conversion_may_fail(expression.type, expression.operands.at(2)->type);
    case typed::ExpressionKind::array_literal:
    case typed::ExpressionKind::dictionary_literal: {
        if (any_operand_may_fail())
            return true;
        const bool constrained =
            (expression.type.kind == TypeKind::array && expression.type.name != "Array") ||
            (expression.type.kind == TypeKind::dictionary && expression.type.name != "Dictionary");
        return constrained && std::any_of(expression.operands.begin(), expression.operands.end(),
                                          [](const auto& operand) {
                                              return operand && operand->type.is_dynamic();
                                          });
    }
    }
    return true;
}

bool CodeGenerator::assignment_may_fail(const typed::Statement& statement) const {
    if (statement.kind != typed::StatementKind::assignment || !statement.condition ||
        !statement.expression)
        return true;
    const auto& target = *statement.condition;
    if (target.kind != typed::ExpressionKind::identifier ||
        target.resolution == typed::ResolutionKind::dynamic_property ||
        target.resolution == typed::ResolutionKind::script_property ||
        target.resolution == typed::ResolutionKind::script_runtime_static_field)
        return true;
    if (expression_may_fail(*statement.expression))
        return true;
    const auto& destination =
        target.assignment_type.kind == TypeKind::unknown ? target.type : target.assignment_type;
    if (statement.operation == "=")
        return conversion_may_fail(destination, emitted_type(*statement.expression));
    if (target.type.is_dynamic() || statement.expression->type.is_dynamic())
        return true;
    if (conversion_may_fail(destination, target.type))
        return true;
    std::string_view operation{statement.operation};
    if (!operation.empty())
        operation.remove_suffix(1);
    const bool integer_target =
        target.type.kind == TypeKind::integer || target.type.kind == TypeKind::enumeration;
    const bool integer_value = statement.expression->type.kind == TypeKind::integer ||
                               statement.expression->type.kind == TypeKind::enumeration;
    if (integer_target && integer_value)
        return operation == "/" || operation == "%" || operation == "**";
    if (target.type.is_numeric() && statement.expression->type.is_numeric())
        return false;
    if ((target.type.kind == TypeKind::string || target.type.kind == TypeKind::string_name) &&
        target.type == statement.expression->type && operation == "+")
        return false;
    return true;
}

std::string CodeGenerator::emit_integer_binary(const typed::Expression& expression) const {
    auto left = emit_expression(*expression.operands.at(0));
    auto right = emit_expression(*expression.operands.at(1));
    return emit_integer_operation(expression.value, std::move(left), std::move(right),
                                  expression.type, expression.span,
                                  expression_may_fail(*expression.operands.at(0)),
                                  expression_may_fail(*expression.operands.at(1)));
}

std::string CodeGenerator::emit_integer_operation(const std::string_view operation,
                                                  std::string left_value, std::string right_value,
                                                  const Type& result_type, const SourceSpan& span,
                                                  const bool left_may_fail,
                                                  const bool right_may_fail) const {
    const auto suffix = std::to_string(temporary_counter_++);
    const auto left = "_gdpp_integer_left_" + suffix;
    const auto right = "_gdpp_integer_right_" + suffix;
    std::string evaluated;
    if (operation == "+")
        evaluated = "gdpp::integer::add(" + left + ", " + right + ")";
    else if (operation == "-")
        evaluated = "gdpp::integer::subtract(" + left + ", " + right + ")";
    else if (operation == "*")
        evaluated = "gdpp::integer::multiply(" + left + ", " + right + ")";
    else if (operation == "/")
        evaluated = "gdpp::runtime::integer_divide(" + left + ", " + right + ", " +
                    script_location(span) + ")";
    else if (operation == "%")
        evaluated = "gdpp::runtime::integer_modulo(" + left + ", " + right + ", " +
                    script_location(span) + ")";
    else if (operation == "<<")
        evaluated = "gdpp::integer::shift_left(" + left + ", " + right + ")";
    else if (operation == ">>")
        evaluated = "gdpp::integer::shift_right(" + left + ", " + right + ")";
    else if (operation == "&")
        evaluated = "gdpp::integer::bit_and(" + left + ", " + right + ")";
    else if (operation == "|")
        evaluated = "gdpp::integer::bit_or(" + left + ", " + right + ")";
    else if (operation == "^")
        evaluated = "gdpp::integer::bit_xor(" + left + ", " + right + ")";
    else if (operation == "**")
        evaluated = "static_cast<int64_t>(gdpp::runtime::binary(godot::Variant::OP_POWER, " + left +
                    ", " + right + ", " + script_location(span) + "))";
    else
        evaluated = "(" + left + " " + std::string{operation} + " " + right + ")";
    const auto evaluated_name = "_gdpp_integer_result_" + suffix;
    const auto left_check =
        left_may_fail ? std::string{" if (script_function_failed()) return {};"} : std::string{};
    const auto right_check =
        right_may_fail ? std::string{" if (script_function_failed()) return {};"} : std::string{};
    const bool operation_may_fail = operation == "/" || operation == "%" || operation == "**";
    const auto result_check = operation_may_fail
                                  ? std::string{" if (script_function_failed()) return {};"}
                                  : std::string{};
    return "([&]() -> " + cpp_type(result_type) + " { const int64_t " + left + " = " +
           std::move(left_value) + ";" + left_check + " const int64_t " + right + " = " +
           std::move(right_value) + ";" + right_check + " const auto " + evaluated_name + " = " +
           evaluated + ";" + result_check + " return " + evaluated_name + "; }())";
}

std::string CodeGenerator::script_location(const SourceSpan& span) {
    return "gdpp::runtime::ScriptSourceLocation{_gdpp_source_path, " +
           std::to_string(span.begin.line) + ", " + std::to_string(span.begin.column) + "}";
}

std::string CodeGenerator::emit_script_static_callable(const typed::Expression& expression) const {
    const typed::Function* local_function = nullptr;
    const ScriptMemberSymbol* project_function = nullptr;
    std::string native_owner;
    if (expression.resolved_owner.empty()) {
        const auto found = local_functions_.find(expression.value);
        if (found != local_functions_.end()) {
            local_function = found->second;
            native_owner = current_native_class_name_;
        }
    } else {
        auto declaration = inner_declarations_.find(expression.resolved_owner);
        if (declaration == inner_declarations_.end()) {
            declaration = std::find_if(
                inner_declarations_.begin(), inner_declarations_.end(), [&](const auto& candidate) {
                    return inner_cpp_type(candidate.first) == expression.resolved_owner;
                });
        }
        if (declaration != inner_declarations_.end()) {
            const auto found =
                std::find_if(declaration->second->functions.begin(),
                             declaration->second->functions.end(), [&](const auto& function) {
                                 return function.is_static && function.name == expression.value;
                             });
            if (found != declaration->second->functions.end()) {
                local_function = &*found;
                native_owner = inner_cpp_type(declaration->first);
            }
        }
        if (!local_function && script_symbols_) {
            if (const auto* inner = script_symbols_->find_inner_native(expression.resolved_owner)) {
                project_function = script_symbols_->find_inner_member(*inner, expression.value);
                native_owner = inner_cpp_type(expression.resolved_owner);
            } else if (const auto* script =
                           script_symbols_->find_native_class(expression.resolved_owner)) {
                project_function = script_symbols_->find_member(*script, expression.value);
                native_owner = expression.resolved_owner;
            }
        }
    }
    if ((!local_function && (!project_function || !project_function->is_static)) ||
        native_owner.empty()) {
        diagnostics_.error("GDS3010", "static callable metadata is unavailable", expression.span);
        return "godot::Callable()";
    }
    const auto parameter_count =
        local_function ? local_function->parameters.size() : project_function->parameters.size();
    const auto required =
        local_function ? static_cast<std::size_t>(std::count_if(
                             local_function->parameters.begin(), local_function->parameters.end(),
                             [](const auto& parameter) { return !parameter.default_value; }))
                       : project_function->required_arguments;
    const bool is_vararg =
        local_function ? local_function->rest_parameter.has_value() : project_function->is_vararg;
    const auto return_type = local_function ? local_function->return_type : project_function->type;
    std::string result =
        "gdpp::runtime::make_named_callable(nullptr, " +
        godot_string_name(native_owner + "::" + expression.value) + ", " +
        std::to_string(required) + ", " + std::to_string(parameter_count) +
        (is_vararg ? ", true" : ", false");
    result += ", [](const godot::Array& _gdpp_static_arguments) -> godot::Variant { ";
    if (is_vararg) {
        result += "godot::Array _gdpp_static_rest; "
                  "_gdpp_static_rest.resize(_gdpp_static_arguments.size() > " +
                  std::to_string(parameter_count) + " ? _gdpp_static_arguments.size() - " +
                  std::to_string(parameter_count) + " : 0); ";
        result += "for (std::int64_t index = " + std::to_string(parameter_count) +
                  "; index < _gdpp_static_arguments.size(); ++index) "
                  "_gdpp_static_rest[index - " +
                  std::to_string(parameter_count) + "] = _gdpp_static_arguments[index]; ";
    }
    if (return_type.kind != TypeKind::void_type)
        result += "return gdpp::runtime::to_variant(";
    result += native_owner + "::_gdpp_script_method_" + sanitize_identifier(expression.value) + "(";
    for (std::size_t index = 0; index < parameter_count; ++index) {
        if (index != 0)
            result += ", ";
        const auto parameter_type = local_function ? local_function->parameters[index].type
                                                   : project_function->parameters[index];
        const bool has_default = local_function
                                     ? local_function->parameters[index].default_value != nullptr
                                     : index < project_function->default_parameters.size() &&
                                           project_function->default_parameters[index];
        const auto indexed = "_gdpp_static_arguments[" + std::to_string(index) + "]";
        if (has_default) {
            result += "_gdpp_static_arguments.size() > " + std::to_string(index) +
                      " ? gdpp::runtime::to_variant(" + indexed +
                      ") : gdpp::runtime::default_argument()";
        } else {
            result += emit_conversion(parameter_type, {TypeKind::variant, "Variant"}, indexed,
                                      &expression.span);
        }
    }
    if (is_vararg) {
        if (parameter_count != 0)
            result += ", ";
        result += "_gdpp_static_rest";
    }
    result += ")";
    if (return_type.kind != TypeKind::void_type)
        result += ")";
    else
        result += "; return godot::Variant()";
    return result + "; })";
}

std::string CodeGenerator::emit_godot_callable(const typed::Expression& expression) const {
    const auto method = godot_string_name(expression.value);
    const auto required = std::to_string(expression.callable_required_arguments);
    const auto positional = std::to_string(expression.callable_maximum_arguments);
    const auto vararg = expression.callable_is_vararg ? "true" : "false";
    const auto location = script_location(expression.span);
    const auto* owner = api_.find_class(expression.resolved_owner);
    const auto owner_type = type_from_annotation(expression.resolved_owner);
    const auto static_callable =
        owner && owner->builtin
            ? "gdpp::runtime::bound_builtin_static_method_at(" + variant_type(owner_type) + ", " +
                  method + ", " + required + ", " + positional + ", " + vararg + ", " + location +
                  ")"
            : "gdpp::runtime::bound_class_static_method_at(" +
                  godot_string_name(expression.resolved_owner) + ", " + method + ", " + required +
                  ", " + positional + ", " + vararg + ", " + location + ")";

    if (expression.direct_access) {
        if (expression.operands.empty())
            return static_callable;
        const auto& receiver = *expression.operands.front();
        const bool type_receiver = receiver.resolution == typed::ResolutionKind::godot_type ||
                                   receiver.resolution == typed::ResolutionKind::external_type ||
                                   receiver.resolution == typed::ResolutionKind::script_type ||
                                   receiver.resolution == typed::ResolutionKind::inner_type;
        if (type_receiver)
            return static_callable;
        const auto suffix = std::to_string(temporary_counter_++);
        const auto value = "_gdpp_static_callable_receiver_" + suffix;
        return "([&]() -> godot::Callable { const godot::Variant " + value +
               " = gdpp::runtime::to_variant(" + emit_expression(receiver) +
               "); if (script_function_failed()) return {}; static_cast<void>(" + value +
               "); return " + static_callable + "; }())";
    }

    const auto receiver =
        expression.operands.empty()
            ? "gdpp::runtime::to_variant(" + self_object_expression() + ")"
            : "gdpp::runtime::to_variant(" + emit_expression(*expression.operands.front()) + ")";
    const auto suffix = std::to_string(temporary_counter_++);
    const auto value = "_gdpp_bound_callable_receiver_" + suffix;
    return "([&]() -> godot::Callable { const godot::Variant " + value + " = " + receiver +
           "; if (script_function_failed()) return {}; return gdpp::runtime::bound_method_at(" +
           value + ", " + method + ", " + required + ", " + positional + ", " + vararg + ", " +
           location + "); }())";
}

std::string CodeGenerator::emit_expression(const typed::Expression& expression) const {
    struct ExpressionSpanScope final {
        const SourceSpan*& current;
        const SourceSpan* previous;
        ~ExpressionSpanScope() { current = previous; }
    };
    ExpressionSpanScope expression_scope{current_expression_span_, current_expression_span_};
    current_expression_span_ = &expression.span;
    if (const auto override = exact_expression_overrides_.find(&expression);
        override != exact_expression_overrides_.end()) {
        return override->second;
    }
    switch (expression.kind) {
    case typed::ExpressionKind::literal: {
        if (expression.literal_kind == typed::LiteralKind::boolean)
            return expression.value;
        if (expression.literal_kind == typed::LiteralKind::nil)
            return "godot::Variant()";
        if (expression.literal_kind == typed::LiteralKind::integer) {
            if (expression.value == "9223372036854775808" ||
                expression.value == "-9223372036854775808") {
                return "(-static_cast<int64_t>(9223372036854775807LL) - "
                       "static_cast<int64_t>(1))";
            }
            return "static_cast<int64_t>(" + expression.value + ")";
        }
        if (expression.literal_kind == typed::LiteralKind::floating) {
            if (expression.value == "inf")
                return "Math_INF";
            if (expression.value == "-inf")
                return "-Math_INF";
            if (expression.value == "nan" || expression.value == "-nan")
                return "Math_NAN";
            return expression.value;
        }
        if (expression.literal_kind == typed::LiteralKind::string_name)
            return godot_string_name(expression.value);
        if (expression.literal_kind == typed::LiteralKind::node_path)
            return godot_node_path(expression.value);
        return godot_string(expression.value);
    }
    case typed::ExpressionKind::node_reference: {
        auto path = expression.value;
        if (!path.empty() && path.front() == '$')
            path.erase(path.begin());
        return "gdpp::runtime::to_variant(" +
               (attached_script_ ? godot_owner_expression() + "->" : std::string{}) +
               "get_node<godot::Node>(" + godot_node_path(path) + "))";
    }
    case typed::ExpressionKind::identifier:
        if (expression.resolution == typed::ResolutionKind::none &&
            expression.symbol_identity != 0) {
            if (const auto override = local_expression_overrides_.find(expression.symbol_identity);
                override != local_expression_overrides_.end()) {
                return override->second;
            }
        }
        if (expression.resolution == typed::ResolutionKind::godot_constructor)
            return cpp_type(expression.type);
        if (expression.resolution == typed::ResolutionKind::godot_singleton)
            return "godot::" + godot_cpp_class_name(expression.resolved_owner) +
                   "::get_singleton()";
        if (expression.resolution == typed::ResolutionKind::external_singleton)
            return "gdpp::runtime::find_engine_singleton_at(" +
                   godot_string_name(expression.value) + ", " + script_location(expression.span) +
                   ")";
        if (expression.resolution == typed::ResolutionKind::godot_type)
            return expression.type.kind == TypeKind::object
                       ? "godot::" + godot_cpp_class_name(expression.resolved_owner)
                       : cpp_type(expression.type);
        if (expression.resolution == typed::ResolutionKind::external_type)
            return godot_string_name(expression.resolved_owner);
        if (expression.resolution == typed::ResolutionKind::external_callable)
            return "gdpp::runtime::external_callable_at(gdpp::runtime::to_variant(" +
                   self_object_expression() + "), " + godot_string_name(expression.value) + ", " +
                   script_location(expression.span) + ")";
        if (expression.resolution == typed::ResolutionKind::external_signal)
            return "gdpp::runtime::external_signal_at(gdpp::runtime::to_variant(" +
                   self_object_expression() + "), " + godot_string_name(expression.value) + ", " +
                   script_location(expression.span) + ")";
        if (expression.resolution == typed::ResolutionKind::script_type)
            return godot_string_name(expression.resolved_owner);
        if (expression.resolution == typed::ResolutionKind::script_resource)
            return cpp_type(expression.type) + "{}";
        if (expression.resolution == typed::ResolutionKind::inner_type)
            return inner_cpp_type(expression.resolved_owner);
        if (expression.resolution == typed::ResolutionKind::script_super)
            return native_super_owner(expression.resolved_owner);
        if (expression.resolution == typed::ResolutionKind::script_signal)
            return "godot::Signal(" + self_object_expression() + ", " +
                   godot_string_name(expression.value) + ")";
        if (expression.resolution == typed::ResolutionKind::script_autoload) {
            auto value =
                "gdpp::runtime::find_autoload(" + godot_string_name(expression.getter) + ")";
            if (!attached_script_source_path(expression.type, expression.resolved_owner).empty()) {
                return emit_attached_script_cast(expression.type, std::move(value),
                                                 &expression.span);
            }
            return emit_object_pointer_storage(expression.type, std::move(value));
        }
        if (expression.resolution == typed::ResolutionKind::script_callable)
            return "godot::Callable(" + self_object_expression() + ", " +
                   godot_string_name(expression.value) + ")";
        if (expression.resolution == typed::ResolutionKind::godot_callable)
            return emit_godot_callable(expression);
        if (expression.resolution == typed::ResolutionKind::script_static_callable)
            return emit_script_static_callable(expression);
        if (expression.resolution == typed::ResolutionKind::script_static_field)
            return "_gdpp_static_" + sanitize_identifier(expression.value) + "_storage()";
        if (expression.resolution == typed::ResolutionKind::script_enum_type &&
            !expression.resolved_owner.empty())
            return sanitize_qualified_identifier(expression.resolved_owner) +
                   "::_gdpp_dictionary()";
        if (expression.resolution == typed::ResolutionKind::enum_member)
            return enum_identifier(expression.value);
        if (expression.resolution == typed::ResolutionKind::script_constant &&
            managed_constant_reference(expression)) {
            return sanitize_identifier(expression.value) + "()";
        }
        if (expression.resolution == typed::ResolutionKind::godot_property &&
            !expression.direct_access && !expression.getter.empty()) {
            std::string index;
            if (expression.indexed_argument >= 0) {
                index = std::to_string(expression.indexed_argument);
                if (const auto* getter =
                        api_.find_method(expression.resolved_owner, expression.getter)) {
                    if (const auto* argument = api_.argument(*getter, 0))
                        index = emit_api_argument(argument->type, argument->meta,
                                                  {TypeKind::integer, "int"}, std::move(index));
                }
            }
            return emit_api_return(
                expression.type,
                (attached_script_ ? godot_owner_expression() + "->" : std::string{}) +
                    expression.getter + "(" + index + ")");
        }
        if (expression.resolution == typed::ResolutionKind::script_property)
            return expression.getter + "()";
        if (expression.resolution == typed::ResolutionKind::godot_method)
            return (attached_script_ ? godot_owner_expression() + "->" : std::string{}) +
                   sanitize_identifier(expression.value);
        if (expression.resolution == typed::ResolutionKind::dynamic_property) {
            auto value = "gdpp::runtime::get_named(gdpp::runtime::to_variant(" +
                         self_object_expression() + "), " + godot_string_name(expression.value) +
                         ", " + script_location(expression.span) + ")";
            return emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                   std::move(value));
        }
        if (expression.resolution == typed::ResolutionKind::global_constant ||
            expression.resolution == typed::ResolutionKind::global_enum_value ||
            expression.resolution == typed::ResolutionKind::global_enum_type)
            return expression.resolved_owner;
        {
            auto identifier = expression.value == "self" ? self_object_expression()
                                                         : sanitize_identifier(expression.value);
            if (expression.storage_type.kind != TypeKind::unknown &&
                expression.storage_type != expression.type &&
                (expression.resolution == typed::ResolutionKind::none ||
                 expression.resolution == typed::ResolutionKind::local_constant)) {
                identifier = emit_conversion(expression.type, expression.storage_type,
                                             std::move(identifier));
            }
            return identifier;
        }
    case typed::ExpressionKind::unary: {
        if (expression.value == "-" &&
            expression.operands.at(0)->kind == typed::ExpressionKind::literal &&
            expression.operands.at(0)->literal_kind == typed::LiteralKind::integer &&
            expression.operands.at(0)->value == "9223372036854775808")
            return emit_expression(*expression.operands.at(0));
        const auto& operand_type = expression.operands.at(0)->type;
        if (expression.value == "not")
            return "(!(" + emit_truthy(*expression.operands.at(0)) + "))";
        const auto integer_like =
            operand_type.kind == TypeKind::integer || operand_type.kind == TypeKind::enumeration;
        if (integer_like && expression.value == "-") {
            return "gdpp::integer::negate(" + emit_expression(*expression.operands.at(0)) + ")";
        }
        if (integer_like && expression.value == "~") {
            return "gdpp::integer::bit_not(" + emit_expression(*expression.operands.at(0)) + ")";
        }
        const bool has_direct_cpp_operator =
            (expression.value == "+" || expression.value == "-") && operand_type.is_numeric();
        if (operand_type.is_dynamic() || !has_direct_cpp_operator) {
            const auto operation_name = expression.value == "+" ? "OP_POSITIVE"
                                        : expression.value == "-"
                                            ? "OP_NEGATE"
                                            : variant_operator(expression.value);
            auto evaluated = "gdpp::runtime::unary(godot::Variant::" + operation_name + ", " +
                             emit_expression(*expression.operands.at(0)) + ", " +
                             script_location(expression.span) + ")";
            return operand_type.is_dynamic()
                       ? std::move(evaluated)
                       : emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                         std::move(evaluated));
        }
        return "(" + expression.value + emit_expression(*expression.operands.at(0)) + ")";
    }
    case typed::ExpressionKind::await_expression:
        diagnostics_.error("GDS3006", "unlowered await expression reached code generation",
                           expression.span);
        return "godot::Variant()";
    case typed::ExpressionKind::binary: {
        if (expression.value == "and" || expression.value == "or") {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto left_name = "_gdpp_logic_left_" + suffix;
            const auto right_name = "_gdpp_logic_right_" + suffix;
            std::string result = "([&]() -> bool { const bool " + left_name + " = " +
                                 emit_truthy(*expression.operands.at(0)) + "; ";
            if (expression_may_fail(*expression.operands.at(0)))
                result += "if (script_function_failed()) return false; ";
            if (expression.value == "and")
                result += "if (!" + left_name + ") return false; ";
            else
                result += "if (" + left_name + ") return true; ";
            result +=
                "const bool " + right_name + " = " + emit_truthy(*expression.operands.at(1)) + "; ";
            if (expression_may_fail(*expression.operands.at(1)))
                result += "if (script_function_failed()) return false; ";
            result += "return " + right_name + "; }())";
            return result;
        }
        const auto emit_ordered_operands = [&](const auto& evaluate) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto left_name = "_gdpp_binary_left_" + suffix;
            const auto right_name = "_gdpp_binary_right_" + suffix;
            auto left = emit_expression(*expression.operands.at(0));
            auto right = emit_expression(*expression.operands.at(1));
            const auto result_name = "_gdpp_binary_result_" + suffix;
            std::string result = "([&]() -> " + cpp_type(expression.type) + " { const auto " +
                                 left_name + " = " + std::move(left) + "; ";
            if (expression_may_fail(*expression.operands.at(0)))
                result += "if (script_function_failed()) return {}; ";
            result += "const auto " + right_name + " = " + std::move(right) + "; ";
            if (expression_may_fail(*expression.operands.at(1)))
                result += "if (script_function_failed()) return {}; ";
            result += "const auto " + result_name + " = " + evaluate(left_name, right_name) +
                      "; if (script_function_failed()) return {}; return " + result_name + "; }())";
            return result;
        };
        auto operation = expression.value;
        if (operation == "and")
            operation = "&&";
        if (operation == "or")
            operation = "||";
        if (operation == "in" || operation == "not in") {
            return emit_ordered_operands([&](const std::string& left, const std::string& right) {
                const auto membership =
                    "static_cast<bool>(gdpp::runtime::binary(godot::Variant::OP_IN, " + left +
                    ", " + right + ", " + script_location(expression.span) + "))";
                return operation == "not in" ? "!(" + membership + ")" : membership;
            });
        }
        if (operation == "is" || operation == "is not") {
            const bool negate = operation == "is not";
            const auto type_test = [negate](std::string value) {
                return negate ? "!(" + value + ")" : value;
            };
            const auto& value = *expression.operands.at(0);
            const auto& target = *expression.operands.at(1);
            const auto emitted_value = emit_expression(value);
            if (target.resolution != typed::ResolutionKind::godot_type &&
                target.resolution != typed::ResolutionKind::external_type &&
                target.resolution != typed::ResolutionKind::script_type &&
                target.resolution != typed::ResolutionKind::inner_type &&
                target.resolution != typed::ResolutionKind::script_enum_type &&
                target.resolution != typed::ResolutionKind::global_enum_type) {
                diagnostics_.error("GDS3001", "type test is missing its resolved target type",
                                   expression.span);
                return type_test("false");
            }
            if (target.type.kind == TypeKind::object) {
                if (target.resolution == typed::ResolutionKind::external_type) {
                    return type_test("gdpp::runtime::is_external_instance("
                                     "gdpp::runtime::to_variant(" +
                                     emitted_value + "), " +
                                     godot_string_name(target.resolved_owner) + ")");
                }
                if (const auto source_path =
                        attached_script_source_path(target.type, target.resolved_owner);
                    !source_path.empty()) {
                    return type_test("gdpp::runtime::is_attached_script_instance(("
                                     "gdpp::runtime::to_variant(" +
                                     emitted_value + ")).get_validated_object(), " +
                                     godot_string(source_path) + ")");
                }
                const auto target_cpp =
                    target.resolution == typed::ResolutionKind::script_type ? target.resolved_owner
                    : target.resolution == typed::ResolutionKind::inner_type
                        ? inner_cpp_type(target.resolved_owner)
                        : "godot::" + godot_cpp_class_name(target.resolved_owner);
                std::string object_value;
                if (value.type.kind == TypeKind::nil) {
                    object_value = "nullptr";
                } else if (value.type.is_dynamic()) {
                    object_value = "(" + emitted_value + ").get_validated_object()";
                } else if (value.type.kind == TypeKind::script_resource) {
                    object_value = "(" + emitted_value + ").resource().ptr()";
                } else if (value.type.kind != TypeKind::object) {
                    return type_test("(static_cast<void>(" + emitted_value + "), false)");
                } else {
                    object_value = emitted_value;
                    const bool external_value =
                        script_symbols_ && script_symbols_->find_external(value.type.name);
                    if (external_value)
                        object_value = "(gdpp::runtime::to_variant(" + object_value +
                                       ")).get_validated_object()";
                    const bool ref_counted = is_ref_counted_object(value.type);
                    if (ref_counted && !external_value && emitted_value != "this")
                        object_value = "(" + object_value + ").ptr()";
                }
                return type_test("(godot::Object::cast_to<" + target_cpp + ">(" + object_value +
                                 ") != nullptr)");
            }
            if (target.type.kind == TypeKind::variant) {
                return type_test("(static_cast<void>(" + emitted_value + "), true)");
            }
            const auto expected_variant_type = variant_type(target.type);
            if (expected_variant_type == "godot::Variant::NIL") {
                diagnostics_.error("GDS3001", "unsupported value type in 'is' expression",
                                   target.span);
                return type_test("false");
            }
            if (value.type.is_dynamic()) {
                return type_test("((" + emitted_value + ").get_type() == " + expected_variant_type +
                                 ")");
            }
            const auto matches = variant_type(value.type) == expected_variant_type;
            return type_test("(static_cast<void>(" + emitted_value + "), " +
                             std::string{matches ? "true" : "false"} + ")");
        }
        if (operation == "as") {
            const auto& value = *expression.operands.at(0);
            const auto& target = *expression.operands.at(1);
            const auto emitted_value = emit_expression(value);
            if (target.resolution == typed::ResolutionKind::external_type) {
                const auto suffix = std::to_string(temporary_counter_++);
                const auto temporary = "_gdpp_external_cast_" + suffix;
                return "([&]() -> godot::Variant { const godot::Variant " + temporary + " = " +
                       "gdpp::runtime::to_variant(" + emitted_value +
                       "); return gdpp::runtime::is_external_instance(" + temporary + ", " +
                       godot_string_name(target.resolved_owner) + ") ? " + temporary +
                       " : godot::Variant(); }())";
            }
            if (target.type.kind == TypeKind::object) {
                if (const auto source_path =
                        attached_script_source_path(target.type, target.resolved_owner);
                    !source_path.empty()) {
                    auto object = "gdpp::runtime::cast_attached_script(gdpp::runtime::to_variant(" +
                                  emitted_value + "), " + godot_string(source_path) + ")";
                    const auto target_cpp = cpp_type(expression.type);
                    if (target_cpp.rfind("godot::Ref<", 0) == 0) {
                        return target_cpp + "(godot::Object::cast_to<godot::RefCounted>(" + object +
                               "))";
                    }
                    return object;
                }
            }
            if (target.type.kind == TypeKind::object &&
                (target.resolution == typed::ResolutionKind::godot_type ||
                 target.resolution == typed::ResolutionKind::script_type ||
                 target.resolution == typed::ResolutionKind::inner_type)) {
                const auto target_cpp =
                    target.resolution == typed::ResolutionKind::script_type ? target.resolved_owner
                    : target.resolution == typed::ResolutionKind::inner_type
                        ? inner_cpp_type(target.resolved_owner)
                        : "godot::" + godot_cpp_class_name(target.resolved_owner);
                auto object = value.type.is_dynamic()
                                  ? "(" + emitted_value + ").get_validated_object()"
                                  : emitted_value;
                if (!value.type.is_dynamic() && value.type.kind == TypeKind::object) {
                    const bool ref_counted = is_ref_counted_object(value.type);
                    if (ref_counted && emitted_value != "this")
                        object = "(" + object + ").ptr()";
                }
                return "godot::Object::cast_to<" + target_cpp + ">(" + object + ")";
            }
            return emit_explicit_conversion(expression.type, value.type, emitted_value);
        }
        if (operation == "**") {
            return emit_ordered_operands([&](const std::string& left, const std::string& right) {
                return "static_cast<" + cpp_type(expression.type) +
                       ">(gdpp::runtime::binary(godot::Variant::OP_POWER, " + left + ", " + right +
                       ", " + script_location(expression.span) + "))";
            });
        }
        const auto& left_type = expression.operands.at(0)->type;
        const auto& right_type = expression.operands.at(1)->type;
        const auto reference_like = [](const Type& type) {
            return type.kind == TypeKind::object || type.kind == TypeKind::script_resource;
        };
        if ((operation == "==" || operation == "!=") && reference_like(left_type) &&
            reference_like(right_type)) {
            // Attached RefCounted scripts store typed values as Ref<T>, while `self` is the
            // provider-owned Object*. Direct C++ comparison is ill-formed even when both values
            // identify the same Godot object. Variant equality is Godot's canonical identity
            // boundary and also handles engine Object pointers, Ref values, and freed instances.
            return emit_ordered_operands([&](const std::string& left, const std::string& right) {
                const auto equality =
                    "static_cast<bool>(gdpp::runtime::binary(godot::Variant::OP_EQUAL, " + left +
                    ", " + right + ", " + script_location(expression.span) + "))";
                return operation == "==" ? equality : "!(" + equality + ")";
            });
        }
        if ((operation == "==" || operation == "!=") &&
            ((reference_like(left_type) && right_type.kind == TypeKind::nil) ||
             (left_type.kind == TypeKind::nil && reference_like(right_type)))) {
            const auto& object =
                reference_like(left_type) ? *expression.operands.at(0) : *expression.operands.at(1);
            if (object.type.kind == TypeKind::script_resource) {
                const auto null_test = emit_expression(object) + ".resource().is_null()";
                return operation == "==" ? "(" + null_test + ")" : "(!(" + null_test + "))";
            }
            if (script_symbols_ && script_symbols_->find_external(object.type.name)) {
                const auto equality =
                    "static_cast<bool>(gdpp::runtime::binary(godot::Variant::OP_EQUAL, " +
                    emit_expression(object) + ", godot::Variant(), " +
                    script_location(expression.span) + "))";
                return operation == "==" ? "(" + equality + ")" : "(!(" + equality + "))";
            }
            const bool ref_counted = is_ref_counted_object(object.type);
            const auto null_test = ref_counted ? emit_expression(object) + ".is_null()"
                                               : emit_expression(object) + " == nullptr";
            return operation == "==" ? "(" + null_test + ")" : "(!(" + null_test + "))";
        }
        const bool external_operand =
            script_symbols_ && ((left_type.kind == TypeKind::object &&
                                 script_symbols_->find_external(left_type.name)) ||
                                (right_type.kind == TypeKind::object &&
                                 script_symbols_->find_external(right_type.name)));
        if (left_type.is_dynamic() || right_type.is_dynamic() || external_operand) {
            std::string runtime_function{"gdpp::runtime::binary"};
            if ((left_type.is_dynamic() && right_type.kind == TypeKind::integer) ||
                (right_type.is_dynamic() && left_type.kind == TypeKind::integer)) {
                runtime_function = "gdpp::runtime::binary_integer";
            }
            return emit_ordered_operands([&](const std::string& left, const std::string& right) {
                const auto dynamic =
                    runtime_function + "(godot::Variant::" + variant_operator(expression.value) +
                    ", " + left + ", " + right + ", " + script_location(expression.span) + ")";
                return expression.type.kind == TypeKind::boolean
                           ? "static_cast<bool>(" + dynamic + ")"
                           : dynamic;
            });
        }
        if (left_type.kind == TypeKind::builtin || right_type.kind == TypeKind::builtin) {
            return emit_ordered_operands([&](const std::string& left, const std::string& right) {
                const auto evaluated =
                    "gdpp::runtime::binary(godot::Variant::" + variant_operator(expression.value) +
                    ", " + left + ", " + right + ", " + script_location(expression.span) + ")";
                return emit_conversion(expression.type, {TypeKind::variant, "Variant"}, evaluated);
            });
        }
        const auto integer_like = [](const Type& type) {
            return type.kind == TypeKind::integer || type.kind == TypeKind::enumeration;
        };
        if (integer_like(left_type) && integer_like(right_type))
            return emit_integer_binary(expression);
        if (!has_direct_cpp_binary_operator(operation, left_type, right_type)) {
            return emit_ordered_operands([&](const std::string& left, const std::string& right) {
                const auto evaluated =
                    "gdpp::runtime::binary(godot::Variant::" + variant_operator(expression.value) +
                    ", " + left + ", " + right + ", " + script_location(expression.span) + ")";
                return emit_conversion(expression.type, {TypeKind::variant, "Variant"}, evaluated);
            });
        }
        return emit_ordered_operands([&](const std::string& left, const std::string& right) {
            return "(" + left + " " + operation + " " + right + ")";
        });
    }
    case typed::ExpressionKind::conditional: {
        const auto emit_branch = [&](const typed::Expression& branch) {
            auto value = emit_expression(branch);
            if (expression.type.is_dynamic())
                return "gdpp::runtime::to_variant(" + value + ")";
            return emit_conversion(expression.type, branch.type, std::move(value));
        };
        const auto suffix = std::to_string(temporary_counter_++);
        const auto condition = "_gdpp_conditional_condition_" + suffix;
        const auto value = "_gdpp_conditional_value_" + suffix;
        const auto condition_check = expression_may_fail(*expression.operands.at(1))
                                         ? std::string{" if (script_function_failed()) return {};"}
                                         : std::string{};
        const auto true_check =
            expression_may_fail(*expression.operands.at(0)) ||
                    conversion_may_fail(expression.type, expression.operands.at(0)->type)
                ? std::string{" if (script_function_failed()) return {};"}
                : std::string{};
        const auto false_check =
            expression_may_fail(*expression.operands.at(2)) ||
                    conversion_may_fail(expression.type, expression.operands.at(2)->type)
                ? std::string{" if (script_function_failed()) return {};"}
                : std::string{};
        return "([&]() -> " + cpp_type(expression.type) + " { const bool " + condition + " = " +
               emit_truthy(*expression.operands.at(1)) + ";" + condition_check + " if (" +
               condition + ") { const auto " + value + " = " +
               emit_branch(*expression.operands.at(0)) + ";" + true_check + " return " + value +
               "; } const auto " + value + " = " + emit_branch(*expression.operands.at(2)) + ";" +
               false_check + " return " + value + "; }())";
    }
    case typed::ExpressionKind::call: {
        const auto expression_failure_return =
            !expression.coroutine_call && expression.type.kind == TypeKind::void_type
                ? std::string{"if (script_function_failed()) return; "}
                : std::string{"if (script_function_failed()) return {}; "};
        const auto call_failure_return =
            expression_may_fail(expression) ? expression_failure_return : std::string{};
        if (expression.resolution == typed::ResolutionKind::script_resource)
            return cpp_type(expression.type) + "{}";
        const auto& callee = *expression.operands.at(0);
        if (callee.resolution == typed::ResolutionKind::external_constructor) {
            return "gdpp::runtime::instantiate_external_class_at(" +
                   godot_string_name(callee.resolved_owner) + ", " +
                   script_location(expression.span) + ")";
        }
        if (callee.resolution == typed::ResolutionKind::external_super_method) {
            const auto suffix = std::to_string(temporary_counter_++);
            std::string invocation = "([&]() -> godot::Variant { ";
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                invocation += "const godot::Variant _gdpp_super_argument_" + suffix + "_" +
                              std::to_string(index - 1) + " = gdpp::runtime::to_variant(" +
                              emit_expression(*expression.operands[index]) +
                              "); if (script_function_failed()) return {}; ";
            }
            invocation +=
                "return gdpp::runtime::call_attached_native_base(" + self_object_expression() +
                ", " + godot_string_name(callee.resolved_owner) + ", " +
                godot_string_name(callee.setter.empty() ? callee.value : callee.setter) +
                ", static_cast<std::uint32_t>(" + std::to_string(callee.indexed_argument) + ")";
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                invocation += ", _gdpp_super_argument_" + suffix + "_" + std::to_string(index - 1);
            }
            invocation += "); if (script_function_failed()) return {}; return "
                          "_gdpp_super_result_" +
                          suffix + "; }())";
            const auto call_begin =
                invocation.rfind("return gdpp::runtime::call_attached_native_base");
            if (call_begin != std::string::npos) {
                invocation.replace(call_begin, std::string{"return "}.size(),
                                   "const godot::Variant _gdpp_super_result_" + suffix + " = ");
            }
            return expression.type.is_dynamic()
                       ? invocation
                       : emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                         invocation);
        }
        if (callee.resolution == typed::ResolutionKind::external_static_method) {
            const auto suffix = std::to_string(temporary_counter_++);
            std::string invocation = "([&]() -> godot::Variant { ";
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                invocation += "const godot::Variant _gdpp_static_argument_" + suffix + "_" +
                              std::to_string(index - 1) + " = gdpp::runtime::to_variant(" +
                              emit_expression(*expression.operands[index]) +
                              "); if (script_function_failed()) return {}; ";
            }
            invocation += "return gdpp::runtime::call_external_static_at(" +
                          script_location(expression.span) + ", " +
                          godot_string_name(callee.resolved_owner) + ", " +
                          godot_string_name(callee.value);
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                invocation += ", _gdpp_static_argument_" + suffix + "_" + std::to_string(index - 1);
            }
            invocation += "); if (script_function_failed()) return {}; return "
                          "_gdpp_static_result_" +
                          suffix + "; }())";
            const auto call_begin =
                invocation.rfind("return gdpp::runtime::call_external_static_at");
            if (call_begin != std::string::npos) {
                invocation.replace(call_begin, std::string{"return "}.size(),
                                   "const godot::Variant _gdpp_static_result_" + suffix + " = ");
            }
            return expression.type.is_dynamic()
                       ? invocation
                       : emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                         invocation);
        }
        if (callee.resolution == typed::ResolutionKind::godot_constructor &&
            callee.type.kind == TypeKind::object) {
            const auto native_type = "godot::" + godot_cpp_class_name(callee.resolved_owner);
            if (api_.inherits(callee.resolved_owner, "RefCounted"))
                return cpp_type(callee.type) + "(memnew(" + native_type + "))";
            return "memnew(" + native_type + ")";
        }
        if (callee.resolution == typed::ResolutionKind::godot_constructor &&
            expression.operands.size() == 2 &&
            (expression.type.kind == TypeKind::boolean ||
             expression.type.kind == TypeKind::integer ||
             expression.type.kind == TypeKind::floating ||
             expression.type.kind == TypeKind::string ||
             expression.type.kind == TypeKind::string_name)) {
            return emit_explicit_conversion(expression.type, expression.operands.at(1)->type,
                                            emit_expression(*expression.operands.at(1)));
        }
        const auto* godot_method =
            callee.resolution == typed::ResolutionKind::godot_method
                ? api_.find_method(callee.resolved_owner, callee.value)
            : callee.resolution == typed::ResolutionKind::script_super && !callee.getter.empty()
                ? api_.find_method(callee.getter,
                                   callee.setter.empty() ? callee.value : callee.setter)
                : nullptr;
        const auto* godot_constructor =
            callee.resolution == typed::ResolutionKind::godot_constructor &&
                    callee.indexed_argument >= 0
                ? api_.find_constructor(callee.resolved_owner, expression.operands.size() - 1,
                                        static_cast<std::size_t>(callee.indexed_argument))
                : nullptr;
        const bool node_get_node_method =
            godot_method && std::string_view{godot_method->owner} == "Node" &&
            std::string_view{godot_method->name} == "get_node";
        const ScriptMemberSymbol* script_method = nullptr;
        const ScriptClassSymbol* script_method_owner = nullptr;
        const ScriptInnerClassSymbol* inner_method_owner = nullptr;
        const typed::Function* typed_inner_method = nullptr;
        std::string typed_inner_method_owner;
        if (script_symbols_) {
            const ScriptClassSymbol* owner = nullptr;
            if (callee.kind == typed::ExpressionKind::identifier) {
                if (callee.resolution == typed::ResolutionKind::script_super) {
                    owner = script_symbols_->find_native_class(callee.resolved_owner);
                    if (!owner && current_script_ && !current_inner_script_)
                        owner = script_symbols_->base_of(*current_script_);
                } else {
                    owner = current_script_;
                }
            } else if (callee.kind == typed::ExpressionKind::member && !callee.operands.empty()) {
                if (callee.resolution == typed::ResolutionKind::script_super) {
                    owner = script_symbols_->find_native_class(callee.resolved_owner);
                    if (!owner && current_script_ && !current_inner_script_)
                        owner = script_symbols_->base_of(*current_script_);
                }
                if (!owner && callee.resolution == typed::ResolutionKind::script_static_callable &&
                    !callee.resolved_owner.empty()) {
                    owner = script_symbols_->find_native_class(callee.resolved_owner);
                    if (!owner) {
                        if (const auto* inner =
                                script_symbols_->find_inner_native(callee.resolved_owner)) {
                            const auto declaration =
                                find_inner_method_declaration(*inner, callee.value, true);
                            if (declaration.method) {
                                script_method = declaration.method;
                                inner_method_owner = declaration.inner_owner;
                                script_method_owner = declaration.script_owner;
                            }
                        }
                    }
                }
                if (!owner)
                    owner = script_symbols_->find_global(callee.operands.at(0)->type.name);
                if (!owner && callee.operands.at(0)->value == "self")
                    owner = current_script_;
            }
            if (owner) {
                const auto* member = script_symbols_->find_member(
                    *owner, callee.resolution == typed::ResolutionKind::script_super &&
                                    !callee.setter.empty()
                                ? callee.setter
                                : callee.value);
                if (member && member->kind == ScriptMemberKind::function) {
                    script_method = member;
                    const ScriptClassSymbol* declaration_owner = owner;
                    while (declaration_owner) {
                        const auto declared = std::find_if(
                            declaration_owner->members.begin(), declaration_owner->members.end(),
                            [&](const auto& candidate) {
                                return candidate.kind == ScriptMemberKind::function &&
                                       candidate.name == member->name;
                            });
                        if (declared != declaration_owner->members.end()) {
                            script_method_owner = declaration_owner;
                            script_method = &*declared;
                            break;
                        }
                        declaration_owner = script_symbols_->base_of(*declaration_owner);
                    }
                }
            }
            if (current_inner_script_) {
                const auto method_name = callee.resolution == typed::ResolutionKind::script_super &&
                                                 !callee.setter.empty()
                                             ? std::string_view{callee.setter}
                                             : std::string_view{callee.value};
                InnerMethodDeclaration declaration;
                if (callee.kind == typed::ExpressionKind::identifier) {
                    declaration = find_inner_method_declaration(
                        *current_inner_script_, method_name,
                        callee.resolution != typed::ResolutionKind::script_super);
                } else if (callee.kind == typed::ExpressionKind::member &&
                           !callee.operands.empty()) {
                    const ScriptInnerClassSymbol* receiver_owner = nullptr;
                    if (callee.resolution == typed::ResolutionKind::script_super) {
                        receiver_owner = current_inner_script_;
                    } else {
                        receiver_owner =
                            script_symbols_->find_inner_native(callee.operands.at(0)->type.name);
                        if (!receiver_owner && current_script_) {
                            receiver_owner = script_symbols_->find_inner(
                                *current_script_, callee.operands.at(0)->type.name);
                        }
                        if (!receiver_owner && callee.operands.at(0)->value == "self")
                            receiver_owner = current_inner_script_;
                    }
                    if (receiver_owner) {
                        declaration = find_inner_method_declaration(
                            *receiver_owner, method_name,
                            callee.resolution != typed::ResolutionKind::script_super);
                    }
                }
                if (declaration.method) {
                    script_method = declaration.method;
                    inner_method_owner = declaration.inner_owner;
                    script_method_owner = declaration.script_owner;
                }
            } else if (callee.kind == typed::ExpressionKind::member && !callee.operands.empty()) {
                const auto* receiver_owner =
                    script_symbols_->find_inner_native(callee.operands.at(0)->type.name);
                if (!receiver_owner && current_script_) {
                    receiver_owner = script_symbols_->find_inner(*current_script_,
                                                                 callee.operands.at(0)->type.name);
                }
                if (receiver_owner) {
                    const auto declaration =
                        find_inner_method_declaration(*receiver_owner, callee.value, true);
                    if (declaration.method) {
                        script_method = declaration.method;
                        inner_method_owner = declaration.inner_owner;
                        script_method_owner = declaration.script_owner;
                    }
                }
            }
            if (!script_method && callee.resolution == typed::ResolutionKind::script_constructor) {
                if (const auto* script_owner =
                        script_symbols_->find_native_class(callee.resolved_owner)) {
                    script_method = script_symbols_->find_member(*script_owner, "_init");
                }
            }
            if (!script_method && callee.resolution == typed::ResolutionKind::inner_constructor &&
                current_script_) {
                const auto* inner_owner = script_symbols_->find_inner_native(callee.resolved_owner);
                if (!inner_owner)
                    inner_owner =
                        script_symbols_->find_inner(*current_script_, callee.resolved_owner);
                if (inner_owner) {
                    script_method = script_symbols_->find_inner_member(*inner_owner, "_init");
                }
            }
        }
        if (!script_method && callee.kind == typed::ExpressionKind::member &&
            !callee.operands.empty()) {
            const auto declaration = std::find_if(
                inner_declarations_.begin(), inner_declarations_.end(), [&](const auto& candidate) {
                    const auto native = inner_cpp_type(candidate.first);
                    return candidate.first == callee.resolved_owner ||
                           native == callee.resolved_owner ||
                           candidate.first == callee.operands.at(0)->resolved_owner ||
                           native == callee.operands.at(0)->resolved_owner ||
                           candidate.first == callee.operands.at(0)->type.name ||
                           native == callee.operands.at(0)->type.name;
                });
            if (declaration != inner_declarations_.end()) {
                const auto method = std::find_if(
                    declaration->second->functions.begin(), declaration->second->functions.end(),
                    [&](const auto& candidate) { return candidate.name == callee.value; });
                if (method != declaration->second->functions.end()) {
                    typed_inner_method = &*method;
                    typed_inner_method_owner = declaration->first;
                }
            }
        }
        const auto script_native_name = [&]() {
            if (script_method && inner_method_owner) {
                return inner_method_implementation_name(*inner_method_owner, *script_method);
            }
            if (script_method && script_method_owner) {
                return script_method_implementation_name(*script_method_owner, *script_method);
            }
            if (typed_inner_method) {
                return typed_inner_method_implementation_name(typed_inner_method_owner,
                                                              *typed_inner_method);
            }
            if (callee.resolution == typed::ResolutionKind::script_super) {
                if (!callee.getter.empty())
                    return sanitize_identifier(callee.value);
                const auto* engine_method =
                    api_.find_method(current_godot_base_type_, callee.value);
                return engine_method && engine_method->is_virtual
                           ? "_gdpp_virtual_impl_" + sanitize_identifier(callee.value)
                           : "_gdpp_script_method_" + sanitize_identifier(callee.value);
            }
            const bool local_source_call =
                callee.kind == typed::ExpressionKind::identifier ||
                (callee.kind == typed::ExpressionKind::member && !callee.operands.empty() &&
                 callee.operands.at(0)->kind == typed::ExpressionKind::identifier &&
                 callee.operands.at(0)->value == "self");
            if (local_source_call) {
                if (const auto local = local_function_native_names_.find(callee.value);
                    local != local_function_native_names_.end()) {
                    return local->second;
                }
            }
            if (callee.kind == typed::ExpressionKind::member && !callee.operands.empty() &&
                callee.operands.at(0)->resolution == typed::ResolutionKind::inner_type &&
                callee.resolution != typed::ResolutionKind::godot_method) {
                return "_gdpp_script_method_" + sanitize_identifier(callee.value);
            }
            return callee.resolution == typed::ResolutionKind::godot_method
                       ? godot_cpp_method_identifier(callee.value)
                   : callee.resolution == typed::ResolutionKind::script_static_callable
                       ? "_gdpp_script_method_" + sanitize_identifier(callee.value)
                       : sanitize_identifier(callee.value);
        }();
        const std::vector<Type>* local_parameters = nullptr;
        const typed::Function* local_function = nullptr;
        const typed::Function* constructor_function = nullptr;
        if (typed_inner_method)
            local_function = typed_inner_method;
        if (!script_method &&
            (callee.kind == typed::ExpressionKind::identifier ||
             (callee.kind == typed::ExpressionKind::member && !callee.operands.empty() &&
              (callee.operands.at(0)->value == "self" ||
               callee.operands.at(0)->type.name == (current_inner_script_
                                                        ? current_inner_script_->name
                                                    : current_script_ ? current_script_->script_name
                                                                      : std::string{}))))) {
            if (const auto found = local_function_parameters_.find(callee.value);
                found != local_function_parameters_.end()) {
                local_parameters = &found->second;
                if (const auto function = local_functions_.find(callee.value);
                    function != local_functions_.end()) {
                    local_function = function->second;
                }
            }
        }
        if (callee.resolution == typed::ResolutionKind::script_constructor ||
            callee.resolution == typed::ResolutionKind::inner_constructor) {
            if (const auto found = constructor_functions_.find(callee.resolved_owner);
                found != constructor_functions_.end()) {
                constructor_function = found->second;
            }
        }
        const bool direct_vararg =
            (expression.call_contract && expression.call_contract->is_vararg) ||
            (script_method && script_method->is_vararg) ||
            (local_function && local_function->rest_parameter.has_value()) ||
            (constructor_function && constructor_function->rest_parameter.has_value());
        const auto vararg_positional_count =
            expression.call_contract && expression.call_contract->is_vararg
                ? expression.call_contract->parameters.size()
            : script_method && script_method->is_vararg        ? script_method->parameters.size()
            : local_function && local_function->rest_parameter ? local_function->parameters.size()
            : constructor_function && constructor_function->rest_parameter
                ? constructor_function->parameters.size()
                : std::size_t{0};
        const auto emit_call_argument = [&](std::size_t operand_index,
                                            std::string value) -> std::string {
            if (godot_constructor) {
                if (const auto* argument = api_.argument(*godot_constructor, operand_index - 1)) {
                    return emit_api_argument(argument->type, argument->meta,
                                             emitted_type(*expression.operands[operand_index]),
                                             std::move(value));
                }
            }
            if (godot_method) {
                if (const auto* argument = api_.argument(*godot_method, operand_index - 1)) {
                    return emit_api_argument(argument->type, argument->meta,
                                             emitted_type(*expression.operands[operand_index]),
                                             std::move(value));
                }
                if (godot_method->is_vararg)
                    return "gdpp::runtime::variant_constructor_argument(" + value + ")";
            }
            if (expression.call_contract &&
                operand_index - 1 < expression.call_contract->parameters.size()) {
                return emit_conversion(expression.call_contract->parameters[operand_index - 1],
                                       emitted_type(*expression.operands[operand_index]),
                                       std::move(value), &expression.operands[operand_index]->span);
            }
            if (script_method && operand_index - 1 < script_method->parameters.size()) {
                return emit_conversion(script_method->parameters[operand_index - 1],
                                       emitted_type(*expression.operands[operand_index]),
                                       std::move(value), &expression.operands[operand_index]->span);
            }
            if (local_parameters && operand_index - 1 < local_parameters->size()) {
                return emit_conversion((*local_parameters)[operand_index - 1],
                                       emitted_type(*expression.operands[operand_index]),
                                       std::move(value), &expression.operands[operand_index]->span);
            }
            if (constructor_function &&
                operand_index - 1 < constructor_function->parameters.size()) {
                return emit_conversion(constructor_function->parameters[operand_index - 1].type,
                                       emitted_type(*expression.operands[operand_index]),
                                       std::move(value), &expression.operands[operand_index]->span);
            }
            return value;
        };
        const auto append_required_variant_defaults = [&](std::string& call, std::size_t provided) {
            const bool needs_null_default = callee.resolved_owner == "Dictionary" &&
                                            provided == 1 &&
                                            (callee.value == "get" || callee.value == "get_or_add");
            const bool array_reduce_default =
                callee.resolved_owner == "Array" && callee.value == "reduce" && provided == 1;
            if (needs_null_default || array_reduce_default)
                call += (provided == 0 ? "godot::Variant()" : ", godot::Variant()");
        };
        if (callee.resolution == typed::ResolutionKind::utility_function ||
            callee.resolution == typed::ResolutionKind::intrinsic) {
            const bool is_intrinsic = callee.resolution == typed::ResolutionKind::intrinsic;
            if (is_intrinsic &&
                (callee.intrinsic == IntrinsicKind::load ||
                 callee.intrinsic == IntrinsicKind::preload) &&
                !expression.type.is_dynamic()) {
                return emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                       "gdpp::runtime::load_resource(" +
                                           emit_expression(*expression.operands.at(1)) + ")");
            }
            const auto suffix = std::to_string(temporary_counter_++);
            const auto function_name =
                callee.resolved_owner == "typeof" ? std::string{"type_of"} : callee.resolved_owner;
            std::string invocation;
            if (is_intrinsic) {
                const auto* feature = IntrinsicRegistry::latest().find(callee.intrinsic);
                invocation = feature ? std::string{feature->runtime_symbol} : std::string{};
                if (invocation.empty()) {
                    diagnostics_.error("GDS5016", "intrinsic has no registered C++ lowering",
                                       expression.span);
                    return "godot::Variant()";
                }
            } else {
                invocation = callee.resolved_owner == "is_instance_valid"
                                 ? std::string{"gdpp::runtime::is_instance_valid"}
                                 : "godot::UtilityFunctions::" + function_name;
            }
            const auto* utility_function =
                is_intrinsic ? nullptr : api_.find_utility_function(callee.resolved_owner);
            const auto emit_intrinsic_argument = [&](const std::size_t index) {
                const auto& argument = *expression.operands[index];
                if (callee.intrinsic == IntrinsicKind::dictionary_to_instance ||
                    callee.intrinsic == IntrinsicKind::instance_to_dictionary) {
                    return "gdpp::runtime::to_variant(" + emit_expression(argument) + ")";
                }
                if (callee.intrinsic == IntrinsicKind::range) {
                    return emit_conversion({TypeKind::integer, "int"}, emitted_type(argument),
                                           emit_expression(argument), &argument.span);
                }
                if (callee.intrinsic != IntrinsicKind::is_instance_of || index != 2)
                    return emit_expression(argument);
                std::string type_name;
                if (argument.resolution == typed::ResolutionKind::godot_type ||
                    argument.resolution == typed::ResolutionKind::external_type ||
                    argument.resolution == typed::ResolutionKind::script_type) {
                    type_name = argument.resolved_owner;
                } else if (argument.resolution == typed::ResolutionKind::inner_type) {
                    type_name = inner_cpp_type(argument.resolved_owner);
                }
                return type_name.empty()
                           ? emit_expression(argument)
                           : "gdpp::runtime::to_variant(" + godot_string_name(type_name) + ")";
            };
            const bool intrinsic_has_location =
                is_intrinsic && (callee.intrinsic == IntrinsicKind::dictionary_to_instance ||
                                 callee.intrinsic == IntrinsicKind::instance_to_dictionary ||
                                 callee.intrinsic == IntrinsicKind::length);
            const auto finish_intrinsic_call = [&](std::string call) {
                if (callee.intrinsic == IntrinsicKind::dictionary_to_instance) {
                    return emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                           std::move(call), &expression.span);
                }
                return call;
            };
            const bool zero_arity_vararg = utility_function && utility_function->is_vararg &&
                                           utility_function->required_arguments == 0 &&
                                           expression.operands.size() == 1;
            // godot-cpp exposes a mandatory placeholder parameter for vararg wrappers even when
            // Godot accepts zero arguments. An empty String preserves the observable output of
            // print-family calls; str() is exactly an empty String and needs no engine call.
            if (zero_arity_vararg && callee.resolved_owner == "str")
                return "godot::String()";
            if (!in_function_body_) {
                std::string direct = invocation + "(";
                for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                    if (index > 1)
                        direct += ", ";
                    auto argument = is_intrinsic ? emit_intrinsic_argument(index)
                                                 : emit_expression(*expression.operands[index]);
                    if (!is_intrinsic) {
                        if (const auto* function =
                                api_.find_utility_function(callee.resolved_owner)) {
                            if (const auto* record = api_.argument(*function, index - 1)) {
                                argument = emit_api_argument(
                                    record->type, record->meta,
                                    emitted_type(*expression.operands[index]), std::move(argument));
                            } else if (function->is_vararg) {
                                argument =
                                    "gdpp::runtime::variant_constructor_argument(" + argument + ")";
                            }
                        }
                    }
                    direct += argument;
                }
                if (zero_arity_vararg)
                    direct += "godot::String()";
                if (intrinsic_has_location) {
                    if (expression.operands.size() > 1)
                        direct += ", ";
                    direct += script_location(expression.span);
                }
                return is_intrinsic ? finish_intrinsic_call(direct + ")")
                                    : emit_api_return(expression.type, direct + ")");
            }
            std::string result = "([&]()";
            if (expression.type.kind != TypeKind::void_type)
                result += " -> " + cpp_type(expression.type);
            result += " { ";
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                result += "const auto _gdpp_utility_argument_" + suffix + "_" +
                          std::to_string(index - 1) + " = " +
                          (is_intrinsic ? emit_intrinsic_argument(index)
                                        : emit_expression(*expression.operands[index])) +
                          "; " + expression_failure_return;
            }
            std::string call = invocation + "(";
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                if (index > 1)
                    call += ", ";
                auto argument =
                    "_gdpp_utility_argument_" + suffix + "_" + std::to_string(index - 1);
                if (!is_intrinsic) {
                    if (const auto* function = api_.find_utility_function(callee.resolved_owner)) {
                        if (const auto* record = api_.argument(*function, index - 1)) {
                            argument = emit_api_argument(record->type, record->meta,
                                                         emitted_type(*expression.operands[index]),
                                                         std::move(argument));
                        } else if (function->is_vararg) {
                            argument =
                                "gdpp::runtime::variant_constructor_argument(" + argument + ")";
                        }
                    }
                }
                call += argument;
            }
            if (zero_arity_vararg)
                call += "godot::String()";
            if (intrinsic_has_location) {
                if (expression.operands.size() > 1)
                    call += ", ";
                call += script_location(expression.span);
            }
            call += ")";
            if (expression.type.kind != TypeKind::void_type) {
                const auto value = "_gdpp_utility_result_" + suffix;
                result += "const auto " + value + " = " +
                          (is_intrinsic ? finish_intrinsic_call(std::move(call))
                                        : emit_api_return(expression.type, std::move(call))) +
                          "; " + expression_failure_return + "return " + value;
            } else {
                result += call + "; " + expression_failure_return;
            }
            return result + "; }())";
        }
        if (callee.resolution == typed::ResolutionKind::script_free)
            return "gdpp::runtime::free_object_at(gdpp::runtime::to_variant(" +
                   emit_expression(*callee.operands.at(0)) + "), " +
                   script_location(expression.span) + ")";
        if (godot_method && callee.value == "get_script" && expression.operands.size() == 1) {
            std::string object = self_object_expression();
            if (callee.kind == typed::ExpressionKind::member) {
                object = emit_expression(*callee.operands.at(0));
                const bool ref_counted = is_ref_counted_object(callee.operands.at(0)->type);
                if (ref_counted)
                    object = "(" + object + ").ptr()";
            }
            return "gdpp::runtime::script_identity(" + object + ", _gdpp_source_path)";
        }
        // A local declared/inherited signal is already owned by this native object. Constructing
        // a temporary godot::Signal for every `pulse.emit(...)` iteration performs a redundant
        // owner/name lookup and is measurably expensive on Windows. Emit through Object directly,
        // cache the immutable StringName per call site, and still materialize arguments in source
        // order before entering Godot. Keep arbitrary `other.signal.emit()` on the general Signal
        // path because a null external receiver must retain Godot's ordinary error semantics.
        if (callee.kind == typed::ExpressionKind::member && callee.value == "emit" &&
            !callee.operands.empty()) {
            const auto& signal = *callee.operands.at(0);
            const bool local_signal =
                signal.resolution == typed::ResolutionKind::script_signal &&
                (signal.kind == typed::ExpressionKind::identifier ||
                 (signal.kind == typed::ExpressionKind::member && !signal.operands.empty() &&
                  signal.operands.at(0)->kind == typed::ExpressionKind::identifier &&
                  signal.operands.at(0)->value == "self"));
            if (local_signal) {
                const auto suffix = std::to_string(temporary_counter_++);
                const auto signal_name = "_gdpp_signal_name_" + suffix;
                std::string result =
                    "([&]() { const auto& " + signal_name +
                    " = *gdpp::runtime::engine_lifetime_static_ptr([] { return godot::Variant{" +
                    godot_string_name(signal.value) + "}; }); ";
                for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                    result += "const auto _gdpp_signal_argument_" + suffix + "_" +
                              std::to_string(index - 1) + " = " +
                              emit_expression(*expression.operands[index]) + "; " +
                              expression_failure_return;
                }
                result += "gdpp::runtime::emit_local_signal_at(" +
                          script_location(expression.span) + ", " + self_object_expression() +
                          ", " + signal_name;
                for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                    result += ", _gdpp_signal_argument_" + suffix + "_" + std::to_string(index - 1);
                }
                return result + "); " + expression_failure_return + "}())";
            }
        }
        if (callee.kind == typed::ExpressionKind::member && callee.value == "call" &&
            callee.operands.at(0)->type.kind == TypeKind::builtin &&
            callee.operands.at(0)->type.name == "Callable") {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto callable_name = "_gdpp_callable_" + suffix;
            std::string result = "([&]() -> godot::Variant { auto &&" + callable_name + " = " +
                                 emit_expression(*callee.operands.at(0)) +
                                 "; if (script_function_failed()) return {}; ";
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                result += "const auto _gdpp_callable_argument_" + suffix + "_" +
                          std::to_string(index - 1) + " = " +
                          emit_expression(*expression.operands[index]) +
                          "; if (script_function_failed()) return {}; ";
            }
            result += "const godot::Variant _gdpp_callable_result_" + suffix +
                      " = gdpp::runtime::call_callable_at(" + script_location(expression.span) +
                      ", " + callable_name;
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                result += ", gdpp::runtime::variant_constructor_argument("
                          "_gdpp_callable_argument_" +
                          suffix + "_" + std::to_string(index - 1) + ")";
            }
            return result +
                   "); if (script_function_failed()) return {}; return "
                   "_gdpp_callable_result_" +
                   suffix + "; }())";
        }
        const bool attached_self_dynamic_call =
            attached_script_ &&
            (callee.kind == typed::ExpressionKind::identifier ||
             (callee.kind == typed::ExpressionKind::member && !callee.operands.empty() &&
              callee.operands.at(0)->kind == typed::ExpressionKind::identifier &&
              callee.operands.at(0)->value == "self"));
        // Generated script methods live on the attached behavior, but inherited ClassDB methods
        // still live on the provider-owned Godot object. External dynamic resolutions carry their
        // provider owner; only owner-less script dispatch may target the behavior itself.
        const bool attached_behavior_dynamic_call =
            attached_self_dynamic_call && callee.resolved_owner.empty();
        const bool explicit_self_script_call =
            callee.kind == typed::ExpressionKind::member && !callee.operands.empty() &&
            callee.operands.at(0)->kind == typed::ExpressionKind::identifier &&
            callee.operands.at(0)->value == "self";
        const bool attached_instance_script_call =
            script_method && callee.kind == typed::ExpressionKind::member &&
            !script_method->is_static && !callee.operands.empty() && !explicit_self_script_call &&
            callee.resolution != typed::ResolutionKind::script_super &&
            callee.operands.at(0)->resolution != typed::ResolutionKind::script_type &&
            callee.operands.at(0)->resolution != typed::ResolutionKind::inner_type &&
            !attached_script_source_path(callee.operands.at(0)->type, callee.resolved_owner)
                 .empty();
        const bool gdscript_resource_constructor =
            callee.kind == typed::ExpressionKind::member && callee.value == "new" &&
            !callee.operands.empty() &&
            callee.operands.at(0)->type == Type{TypeKind::object, "GDScript"};
        if (callee.resolution == typed::ResolutionKind::dynamic_method ||
            attached_instance_script_call || gdscript_resource_constructor) {
            const auto identity = temporary_counter_++;
            const auto suffix = std::to_string(identity);
            const auto target_name = "_gdpp_dynamic_target_" + suffix;
            const auto method_name = "_gdpp_dynamic_method_" + suffix;
            const auto capture = in_function_body_ ? "[&]" : "[]";
            std::string result = "(" + std::string{capture} +
                                 "() -> godot::Variant { godot::Variant " + target_name +
                                 " = gdpp::runtime::to_variant(" +
                                 (attached_behavior_dynamic_call ? "this"
                                  : callee.kind == typed::ExpressionKind::identifier
                                      ? self_object_expression()
                                      : emit_expression(*callee.operands.at(0))) +
                                 "); if (script_function_failed()) return {}; const auto& " +
                                 method_name +
                                 " = *gdpp::runtime::engine_lifetime_static_ptr([] { return " +
                                 godot_string_name(callee.value) + "; }); ";
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                result += "const godot::Variant _gdpp_dynamic_argument_" + suffix + "_" +
                          std::to_string(index - 1) + " = gdpp::runtime::to_variant(" +
                          emit_expression(*expression.operands[index]) +
                          "); if (script_function_failed()) return {}; ";
            }
            result += "const godot::Variant _gdpp_dynamic_result_" + suffix +
                      " = gdpp::runtime::call_dynamic_at(" + script_location(expression.span) +
                      ", " + target_name + ", " + method_name;
            for (std::size_t index = 1; index < expression.operands.size(); ++index) {
                result += ", _gdpp_dynamic_argument_" + suffix + "_" + std::to_string(index - 1);
            }
            const auto invocation = result +
                                    "); if (script_function_failed()) return {}; "
                                    "return _gdpp_dynamic_result_" +
                                    suffix + "; }())";
            const auto& result_storage = emitted_type(expression);
            return expression.coroutine_call || result_storage.is_dynamic()
                       ? invocation
                       : emit_conversion(result_storage, {TypeKind::variant, "Variant"},
                                         invocation);
        }
        std::string invocation;
        std::string receiver_setup;
        std::string receiver_name;
        const auto suffix = std::to_string(temporary_counter_++);
        if (callee.resolution == typed::ResolutionKind::script_constructor) {
            const auto receiver = "_gdpp_script_resource_" + suffix;
            const auto resource =
                callee.operands.at(0)->type.kind == TypeKind::script_resource
                    ? emit_expression(*callee.operands.at(0))
                    : "gdpp::runtime::ScriptResource<" + callee.resolved_owner + ">{}";
            receiver_setup = "auto " + receiver + " = " + resource + "; ";
            invocation = receiver + ".instantiate";
        } else if (callee.resolution == typed::ResolutionKind::inner_constructor) {
            invocation = detail_namespace_ + "::InternalClassResource<" +
                         inner_cpp_type(callee.resolved_owner) + ">{}.instantiate";
        } else if (callee.resolution == typed::ResolutionKind::script_super) {
            invocation = native_super_owner(callee.resolved_owner) + "::" + script_native_name;
        } else if (typed_inner_method && typed_inner_method->is_static) {
            invocation = inner_cpp_type(typed_inner_method_owner) + "::" + script_native_name;
        } else if (callee.resolution == typed::ResolutionKind::script_static_callable &&
                   !callee.resolved_owner.empty()) {
            invocation = callee.resolved_owner + "::" + script_native_name;
        } else if (callee.kind == typed::ExpressionKind::member && !callee.operands.empty() &&
                   callee.operands.at(0)->resolution == typed::ResolutionKind::inner_type &&
                   callee.resolution != typed::ResolutionKind::godot_method) {
            invocation = emit_expression(*callee.operands.at(0)) + "::" + script_native_name;
        } else if (explicit_self_script_call && script_method) {
            invocation = "this->" + script_native_name;
        } else if (local_function ||
                   (script_method && callee.kind == typed::ExpressionKind::identifier)) {
            invocation = script_native_name;
        } else if (callee.kind == typed::ExpressionKind::member &&
                   callee.resolution == typed::ResolutionKind::godot_method &&
                   callee.operands.at(0)->type.kind == TypeKind::script_resource) {
            const auto receiver = "_gdpp_call_receiver_" + suffix;
            receiver_name = receiver;
            receiver_setup = "auto " + receiver + " = " + emit_expression(*callee.operands.at(0)) +
                             ".resource(); ";
            invocation = receiver + "->" + script_native_name;
        } else if (callee.kind == typed::ExpressionKind::member &&
                   callee.resolution != typed::ResolutionKind::script_super &&
                   callee.operands.at(0)->resolution != typed::ResolutionKind::godot_type &&
                   callee.operands.at(0)->resolution != typed::ResolutionKind::script_type &&
                   callee.operands.at(0)->resolution != typed::ResolutionKind::inner_type) {
            const auto receiver = "_gdpp_call_receiver_" + suffix;
            receiver_name = receiver;
            receiver_setup =
                "auto &&" + receiver + " = " + emit_expression(*callee.operands.at(0)) + "; ";
            if (callee.operands.at(0)->type.is_packed_array())
                invocation = receiver + ".native()." + script_native_name;
            else {
                const auto connector =
                    callee.operands.at(0)->type.kind == TypeKind::object ? "->" : ".";
                invocation = receiver + connector + script_native_name;
            }
            if (node_get_node_method) {
                invocation += "<godot::Node>";
            }
        } else {
            invocation = emit_expression(callee);
            if (node_get_node_method) {
                invocation += "<godot::Node>";
            }
        }
        if (expression.operands.size() == 1 && receiver_setup.empty() && !direct_vararg) {
            const auto call = invocation + "()";
            return godot_method ? emit_api_return(expression.type, call) : call;
        }
        const auto capture = in_function_body_ ? "[&]" : "[]";
        std::string result = "(" + std::string{capture} + "()";
        if (expression.coroutine_call || expression.type.kind != TypeKind::void_type)
            result += " -> " + (expression.coroutine_call ? std::string{"godot::Variant"}
                                                          : cpp_type(expression.type));
        result += " { " + receiver_setup;
        if (!receiver_setup.empty() && expression_may_fail(*callee.operands.at(0)))
            result += expression_failure_return;
        if (!receiver_name.empty() &&
            callee.operands.at(0)->type.kind == TypeKind::script_resource) {
            const auto message =
                godot_string("Cannot call method '" + callee.value + "' on a missing script.");
            result += "if (" + receiver_name +
                      ".is_null()) { gdpp::runtime::report_script_failure(" + message +
                      ", _gdpp_source_path, " + std::to_string(callee.span.begin.line) + ", " +
                      std::to_string(callee.span.begin.column) + "); " +
                      (!expression.coroutine_call && expression.type.kind == TypeKind::void_type
                           ? "return; "
                           : "return {}; ") +
                      "} ";
        } else if (!receiver_name.empty() && callee.operands.at(0)->type.kind == TypeKind::object) {
            const auto message =
                godot_string("Cannot call method '" + callee.value + "' on a null or freed value.");
            result += "if (!gdpp::runtime::is_instance_valid(gdpp::runtime::to_variant(" +
                      receiver_name + "))) { gdpp::runtime::report_script_failure(" + message +
                      ", _gdpp_source_path, " + std::to_string(callee.span.begin.line) + ", " +
                      std::to_string(callee.span.begin.column) + "); " +
                      (!expression.coroutine_call && expression.type.kind == TypeKind::void_type
                           ? "return; "
                           : "return {}; ") +
                      "} ";
        }
        for (std::size_t index = 1; index < expression.operands.size(); ++index) {
            const auto& argument = *expression.operands[index];
            const auto temporary =
                "_gdpp_call_argument_" + suffix + "_" + std::to_string(index - 1);
            const auto native_type = cpp_type(argument.type);
            if (argument.kind == typed::ExpressionKind::identifier && argument.value == "self" &&
                native_type.rfind("godot::Ref<", 0) == 0) {
                const auto object_type =
                    native_type.substr(std::string{"godot::Ref<"}.size(),
                                       native_type.size() - std::string{"godot::Ref<"}.size() - 1);
                result += "const " + native_type + " " + temporary + " = " + native_type +
                          "(godot::Object::cast_to<" + object_type + ">(" +
                          self_object_expression() + ")); ";
            } else {
                result += "const auto " + temporary + " = " + emit_expression(argument) + "; ";
            }
            if (expression_may_fail(argument))
                result += expression_failure_return;
        }
        std::string rest_name;
        if (direct_vararg) {
            const auto provided = expression.operands.size() - 1;
            const auto rest_count = provided > vararg_positional_count
                                        ? provided - vararg_positional_count
                                        : std::size_t{0};
            rest_name = "_gdpp_call_rest_" + suffix;
            result += "godot::Array " + rest_name + "; " + rest_name + ".resize(" +
                      std::to_string(rest_count) + "); ";
            for (std::size_t index = 0; index < rest_count; ++index) {
                result += rest_name + "[" + std::to_string(index) +
                          "] = gdpp::runtime::to_variant("
                          "_gdpp_call_argument_" +
                          suffix + "_" + std::to_string(vararg_positional_count + index) + "); ";
            }
        }
        std::string call = invocation + "(";
        const auto provided = expression.operands.size() - 1;
        const auto emitted_arguments = direct_vararg ? vararg_positional_count : provided;
        for (std::size_t index = 0; index < emitted_arguments; ++index) {
            if (index != 0)
                call += ", ";
            call += index < provided
                        ? emit_call_argument(index + 1, "_gdpp_call_argument_" + suffix + "_" +
                                                            std::to_string(index))
                        : "gdpp::runtime::default_argument()";
        }
        if (direct_vararg) {
            if (emitted_arguments != 0)
                call += ", ";
            call += rest_name;
        }
        if (!direct_vararg)
            append_required_variant_defaults(call, provided);
        call += ")";
        if (expression.coroutine_call || expression.type.kind != TypeKind::void_type) {
            const auto result_name = "_gdpp_call_result_" + suffix;
            result += "const auto " + result_name + " = " +
                      (godot_method ? emit_api_return(expression.type, std::move(call)) : call) +
                      "; " + call_failure_return + "return " + result_name;
        } else {
            result += call + "; " + call_failure_return;
        }
        return result + "; }())";
    }
    case typed::ExpressionKind::member: {
        if (expression.resolution == typed::ResolutionKind::inner_type)
            return inner_cpp_type(expression.resolved_owner);
        if (expression.resolution == typed::ResolutionKind::godot_method &&
            !expression.operands.empty() &&
            expression.operands.at(0)->type.kind == TypeKind::script_resource) {
            return emit_expression(*expression.operands.at(0)) + ".resource()->" +
                   godot_cpp_method_identifier(expression.value);
        }
        if (expression.resolution == typed::ResolutionKind::enum_member &&
            !expression.resolved_owner.empty()) {
            return sanitize_qualified_identifier(expression.resolved_owner) +
                   "::" + enum_identifier(expression.value);
        }
        if (expression.resolution == typed::ResolutionKind::external_callable) {
            return "gdpp::runtime::external_callable_at(gdpp::runtime::to_variant(" +
                   emit_expression(*expression.operands.at(0)) + "), " +
                   godot_string_name(expression.value) + ", " + script_location(expression.span) +
                   ")";
        }
        if (expression.resolution == typed::ResolutionKind::external_signal) {
            return "gdpp::runtime::external_signal_at(gdpp::runtime::to_variant(" +
                   emit_expression(*expression.operands.at(0)) + "), " +
                   godot_string_name(expression.value) + ", " + script_location(expression.span) +
                   ")";
        }
        if (expression.resolution == typed::ResolutionKind::script_enum_type)
            return sanitize_qualified_identifier(expression.resolved_owner) +
                   "::_gdpp_dictionary()";
        if (expression.resolution == typed::ResolutionKind::script_constant &&
            !expression.resolved_owner.empty()) {
            const auto inner_owner = inner_cpp_type(expression.resolved_owner);
            return (inner_owner.empty() ? expression.resolved_owner : inner_owner) +
                   "::" + sanitize_identifier(expression.value) +
                   (managed_static_constant(expression.type) ? "()" : "");
        }
        if (expression.resolution == typed::ResolutionKind::script_super)
            return native_super_owner(expression.resolved_owner) +
                   "::" + sanitize_identifier(expression.value);
        if (expression.resolution == typed::ResolutionKind::script_signal) {
            auto object = emit_expression(*expression.operands.at(0));
            if (expression.operands.at(0)->type.kind == TypeKind::script_resource)
                object = "(" + object + ").resource().ptr()";
            const bool ref_counted = is_ref_counted_object(expression.operands.at(0)->type);
            if (ref_counted && object != self_object_expression())
                object = "(" + object + ").ptr()";
            return "godot::Signal(" + object + ", " + godot_string_name(expression.value) + ")";
        }
        if (expression.resolution == typed::ResolutionKind::script_callable) {
            auto object = emit_expression(*expression.operands.at(0));
            const bool ref_counted = is_ref_counted_object(expression.operands.at(0)->type);
            if (ref_counted && object != self_object_expression())
                object = "(" + object + ").ptr()";
            return "godot::Callable(" + object + ", " + godot_string_name(expression.value) + ")";
        }
        if (expression.resolution == typed::ResolutionKind::godot_callable)
            return emit_godot_callable(expression);
        if (expression.resolution == typed::ResolutionKind::script_static_callable)
            return emit_script_static_callable(expression);
        if (expression.resolution == typed::ResolutionKind::global_constant ||
            expression.resolution == typed::ResolutionKind::global_enum_value)
            return expression.resolved_owner;
        if (expression.resolution == typed::ResolutionKind::builtin_constant)
            return builtin_constant_expression(expression.resolved_owner);
        if (expression.resolution == typed::ResolutionKind::script_runtime_static_field) {
            const auto access = expression.resolved_owner + "::" + expression.getter + "()";
            return "(gdpp::runtime::is_editor_hint() ? godot::Variant() : "
                   "gdpp::runtime::to_variant(" +
                   access + "))";
        }
        const auto object =
            expression.operands.at(0)->resolution == typed::ResolutionKind::script_type
                ? expression.operands.at(0)->resolved_owner
            : expression.operands.at(0)->type.kind == TypeKind::script_resource
                ? "(" + emit_expression(*expression.operands.at(0)) + ").resource()"
                : emit_expression(*expression.operands.at(0));
        if (expression.resolution == typed::ResolutionKind::dynamic_property) {
            if (expression.operands.at(0)->type.kind == TypeKind::dictionary) {
                const auto suffix = std::to_string(temporary_counter_++);
                const auto& dictionary_receiver = *expression.operands.at(0);
                const auto proof =
                    dictionary_receiver.kind == typed::ExpressionKind::identifier &&
                            dictionary_receiver.symbol_identity != 0
                        ? proven_local_dictionary_slots_.find(dictionary_receiver.symbol_identity)
                        : proven_local_dictionary_slots_.end();
                const bool proven = proof != proven_local_dictionary_slots_.end() &&
                                    proof->second.find(expression.value) != proof->second.end();
                if (proven) {
                    const auto key = "_gdpp_proven_dictionary_read_key_" + suffix;
                    const auto value =
                        "([&]() -> const godot::Variant& { const auto& " + key +
                        " = *gdpp::runtime::engine_lifetime_static_ptr([] { return godot::Variant{" +
                        godot_string_name(expression.value) + "}; }); return " + object + "[" +
                        key + "]; }())";
                    return emit_conversion(expression.type, {TypeKind::variant, "Variant"}, value,
                                           &expression.span);
                }
                const auto key = "_gdpp_dictionary_read_key_" + suffix;
                const auto lookup = [&](const std::string& receiver) {
                    return "gdpp::runtime::checked_dictionary_get_named(" + receiver +
                           ", *gdpp::runtime::engine_lifetime_static_ptr([] { return " +
                           godot_string_name(expression.value) + "; }), " +
                           script_location(expression.span) + ")";
                };
                std::string value;
                if (!expression_may_fail(*expression.operands.at(0))) {
                    value = lookup(object);
                } else {
                    const auto receiver = "_gdpp_dictionary_read_receiver_" + suffix;
                    value = "([&]() -> godot::Variant { auto &&" + receiver + " = " + object +
                            "; if (script_function_failed()) return {}; return " +
                            lookup(receiver) + "; }())";
                }
                return emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                       std::move(value), &expression.span);
            }
            auto value = "gdpp::runtime::get_named(" + object + ", " +
                         godot_string_name(expression.value) + ", " +
                         script_location(expression.span) + ")";
            return emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                   std::move(value));
        }
        const auto& receiver = *expression.operands.at(0);
        const bool checked_object_access =
            (receiver.type.kind == TypeKind::object ||
             receiver.type.kind == TypeKind::script_resource) &&
            object != "this" && receiver.resolution != typed::ResolutionKind::godot_type &&
            receiver.resolution != typed::ResolutionKind::script_type &&
            receiver.resolution != typed::ResolutionKind::inner_type;
        const auto receiver_name = checked_object_access ? "_gdpp_property_receiver_" +
                                                               std::to_string(temporary_counter_++)
                                                         : object;
        const auto finish_object_access = [&](std::string access) {
            if (!checked_object_access)
                return access;
            const auto message = godot_string("Cannot access member '" + expression.value +
                                              "' on a null or freed value.");
            return "([&]() -> " + cpp_type(expression.type) + " { auto &&" + receiver_name + " = " +
                   object + "; if (!gdpp::runtime::is_instance_valid(gdpp::runtime::to_variant(" +
                   receiver_name + "))) { gdpp::runtime::report_script_failure(" + message +
                   ", _gdpp_source_path, " + std::to_string(expression.span.begin.line) + ", " +
                   std::to_string(expression.span.begin.column) +
                   "); return {}; } const auto "
                   "_gdpp_member_result = " +
                   access +
                   "; if (script_function_failed()) return {}; return "
                   "_gdpp_member_result; }())";
        };
        const auto connector =
            expression.resolution == typed::ResolutionKind::enum_member ||
                    expression.resolution == typed::ResolutionKind::builtin_constant
                ? "::"
            : expression.operands.at(0)->resolution == typed::ResolutionKind::godot_type  ? "::"
            : expression.operands.at(0)->resolution == typed::ResolutionKind::script_type ? "::"
            : expression.operands.at(0)->resolution == typed::ResolutionKind::inner_type  ? "::"
            : receiver_name == "this" || expression.operands.at(0)->type.kind == TypeKind::object ||
                    expression.operands.at(0)->type.kind == TypeKind::script_resource
                ? "->"
                : ".";
        if (expression.resolution == typed::ResolutionKind::godot_property &&
            !expression.direct_access && !expression.getter.empty()) {
            std::string index;
            if (expression.indexed_argument >= 0) {
                index = std::to_string(expression.indexed_argument);
                if (const auto* getter =
                        api_.find_method(expression.resolved_owner, expression.getter)) {
                    if (const auto* argument = api_.argument(*getter, 0))
                        index = emit_api_argument(argument->type, argument->meta,
                                                  {TypeKind::integer, "int"}, std::move(index));
                }
            }
            return finish_object_access(
                emit_api_return(expression.type,
                                receiver_name + connector + expression.getter + "(" + index + ")"));
        }
        if (expression.resolution == typed::ResolutionKind::script_property &&
            !expression.getter.empty()) {
            if (expression.operands.at(0)->type.kind == TypeKind::script_resource &&
                !expression.resolved_owner.empty()) {
                return expression.resolved_owner + "::" + expression.getter + "()";
            }
            if (expression.operands.at(0)->resolution == typed::ResolutionKind::script_type) {
                return expression.resolved_owner + "::" + expression.getter + "()";
            }
            if (expression.operands.at(0)->resolution == typed::ResolutionKind::inner_type) {
                return emit_expression(*expression.operands.at(0)) + "::" + expression.getter +
                       "()";
            }
            const bool explicit_self =
                attached_script_ &&
                expression.operands.at(0)->kind == typed::ExpressionKind::identifier &&
                expression.operands.at(0)->value == "self";
            if (explicit_self)
                return expression.getter + "()";
            if (!attached_script_source_path(expression.operands.at(0)->type,
                                             expression.resolved_owner)
                     .empty()) {
                auto value = "gdpp::runtime::get_named(gdpp::runtime::to_variant(" + object +
                             "), " + godot_string_name(expression.value) + ", " +
                             script_location(expression.span) + ")";
                return emit_conversion(expression.type, {TypeKind::variant, "Variant"},
                                       std::move(value));
            }
            return finish_object_access(receiver_name + connector + expression.getter + "()");
        }
        if (expression.resolution == typed::ResolutionKind::godot_property &&
            expression.direct_access && expression.operands.at(0)->type.kind == TypeKind::builtin) {
            return emit_direct_builtin_member(expression.resolved_owner, object, expression.value,
                                              &expression.span);
        }
        return finish_object_access(
            receiver_name + connector +
            (expression.resolution == typed::ResolutionKind::enum_member
                 ? enum_identifier(expression.value)
                 : sanitize_identifier(expression.value)) +
            (expression.resolution == typed::ResolutionKind::script_constant &&
                     managed_static_constant(expression.type)
                 ? "()"
                 : ""));
    }
    case typed::ExpressionKind::subscript: {
        if (expression.operands.at(0)->type.is_dynamic() ||
            expression.operands.at(0)->type.kind == TypeKind::object) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto target_name = "_gdpp_dynamic_target_" + suffix;
            const auto key_name = "_gdpp_dynamic_key_" + suffix;
            const auto value_name = "_gdpp_dynamic_value_" + suffix;
            return "([&]() -> godot::Variant { godot::Variant " + target_name + " = " +
                   "gdpp::runtime::to_variant(" + emit_expression(*expression.operands.at(0)) +
                   "); if (script_function_failed()) return {}; const "
                   "godot::Variant " +
                   key_name + " = gdpp::runtime::to_variant(" +
                   emit_expression(*expression.operands.at(1)) +
                   "); if (script_function_failed()) return {}; const "
                   "godot::Variant " +
                   value_name + " = gdpp::runtime::get_key(" + target_name + ", " + key_name +
                   ", " + script_location(expression.span) +
                   "); if (script_function_failed()) return {}; return " + value_name + "; }())";
        }
        const auto suffix = std::to_string(temporary_counter_++);
        const auto target_name = "_gdpp_subscript_target_" + suffix;
        const auto index_name = "_gdpp_subscript_index_" + suffix;
        const auto value_name = "_gdpp_subscript_value_" + suffix;
        return "([&]() -> " + cpp_type(expression.type) + " { auto &&" + target_name + " = " +
               emit_expression(*expression.operands.at(0)) +
               "; if (script_function_failed()) return {}; const auto " + index_name + " = " +
               emit_expression(*expression.operands.at(1)) +
               "; if (script_function_failed()) return {}; const auto " + value_name + " = " +
               emit_subscript_read(expression.operands.at(0)->type, expression.type, target_name,
                                   index_name, expression.span) +
               "; if (script_function_failed()) return {}; return " + value_name + "; }())";
    }
    case typed::ExpressionKind::array_literal: {
        const auto descriptor = describe_container_type(expression.type);
        const auto runtime_typed = descriptor && descriptor->has_runtime_constraint();
        const auto native_type = runtime_typed ? cpp_type(expression.type) : "godot::Array";
        if (expression.operands.empty())
            return native_type + "()";
        const auto suffix = std::to_string(temporary_counter_++);
        const auto array = "_gdpp_array_" + suffix;
        const auto capture = in_function_body_ ? "[&]" : "[]";
        std::string result = std::string{"("} + capture + "() -> " + native_type + " { " +
                             native_type + " " + array + "; " + array + ".resize(" +
                             std::to_string(expression.operands.size()) + "); ";
        for (std::size_t index = 0; index < expression.operands.size(); ++index) {
            const auto value = "_gdpp_array_value_" + suffix + "_" + std::to_string(index);
            auto emitted = emit_expression(*expression.operands[index]);
            if (runtime_typed) {
                emitted = emit_conversion(container_argument_type(descriptor->arguments.front()),
                                          expression.operands[index]->type, std::move(emitted));
            } else {
                emitted = "gdpp::runtime::to_variant(" + emitted + ")";
            }
            result += "{ const auto " + value + " = " + emitted + "; ";
            if (expression_may_fail(*expression.operands[index]) ||
                (runtime_typed &&
                 conversion_may_fail(container_argument_type(descriptor->arguments.front()),
                                     expression.operands[index]->type))) {
                result += "if (script_function_failed()) return {}; ";
            }
            result += array + "[" + std::to_string(index) + "] = " + value + "; } ";
        }
        return result + "return " + array + "; }())";
    }
    case typed::ExpressionKind::dictionary_literal: {
        const auto descriptor = describe_container_type(expression.type);
        const auto runtime_typed = descriptor && descriptor->has_runtime_constraint();
        const auto native_type = runtime_typed ? cpp_type(expression.type) : "godot::Dictionary";
        const auto suffix = std::to_string(temporary_counter_++);
        const auto dictionary = "_gdpp_dictionary_" + suffix;
        const auto capture = in_function_body_ ? "[&]" : "[]";
        std::string result = std::string{"("} + capture + "() -> " + native_type + " { " +
                             native_type + " " + dictionary + "; ";
        for (std::size_t index = 0; index + 1 < expression.operands.size(); index += 2) {
            const auto entry = suffix + "_" + std::to_string(index / 2);
            const auto key = "_gdpp_dictionary_key_" + entry;
            const auto value = "_gdpp_dictionary_value_" + entry;
            auto emitted_key = emit_expression(*expression.operands[index]);
            auto emitted_value = emit_expression(*expression.operands[index + 1]);
            if (runtime_typed) {
                emitted_key =
                    emit_conversion(container_argument_type(descriptor->arguments.at(0)),
                                    expression.operands[index]->type, std::move(emitted_key));
                emitted_value =
                    emit_conversion(container_argument_type(descriptor->arguments.at(1)),
                                    expression.operands[index + 1]->type, std::move(emitted_value));
            } else {
                emitted_key = "gdpp::runtime::to_variant(" + emitted_key + ")";
                emitted_value = "gdpp::runtime::to_variant(" + emitted_value + ")";
            }
            result += "{ const auto " + key + " = " + emitted_key + "; ";
            if (expression_may_fail(*expression.operands[index]) ||
                (runtime_typed &&
                 conversion_may_fail(container_argument_type(descriptor->arguments.at(0)),
                                     expression.operands[index]->type))) {
                result += "if (script_function_failed()) return {}; ";
            }
            result += "const auto " + value + " = " + emitted_value + "; ";
            if (expression_may_fail(*expression.operands[index + 1]) ||
                (runtime_typed &&
                 conversion_may_fail(container_argument_type(descriptor->arguments.at(1)),
                                     expression.operands[index + 1]->type))) {
                result += "if (script_function_failed()) return {}; ";
            }
            result += dictionary + ".set(" + key + ", " + value + "); } ";
        }
        return result + "return " + dictionary + "; }())";
    }
    case typed::ExpressionKind::lambda: {
        if (!expression.lambda) {
            diagnostics_.error("GDS3007", "lambda expression is missing typed IR payload",
                               expression.span);
            return "godot::Callable()";
        }
        const auto& lambda = *expression.lambda;
        const auto required =
            std::count_if(lambda.parameters.begin(), lambda.parameters.end(),
                          [](const auto& parameter) { return !parameter.default_value; });
        const auto identity = temporary_counter_++;
        const auto arguments = "_gdpp_lambda_arguments_" + std::to_string(identity);
        const auto saved_local_overrides = local_expression_overrides_;
        std::map<FlowSymbolId, std::string> ordered_overrides(local_expression_overrides_.begin(),
                                                              local_expression_overrides_.end());
        std::string result;
        if (!ordered_overrides.empty()) {
            result += "([&]() {\n";
            for (const auto& [symbol, source] : ordered_overrides) {
                const auto capture = "_gdpp_lambda_capture_" + std::to_string(identity) + "_" +
                                     std::to_string(symbol);
                result += "    [[maybe_unused]] auto " + capture + " = " + source + ";\n";
                local_expression_overrides_[symbol] = capture;
            }
            result += "    return ";
        }
        result += "gdpp::runtime::make_local_callable<" + std::to_string(required) + ", " +
                  std::to_string(lambda.parameters.size()) + ", " +
                  (lambda.rest_parameter ? "true" : "false") + ">(";
        result += lambda.owner_bound ? self_object_expression() : "nullptr";
        // GDScript captures the value visible when the Callable is created, then copies that
        // snapshot into a fresh invocation frame for every call. Scalar writes inside one call
        // therefore do not persist into the next call, while Array/Dictionary/Object copies keep
        // their normal shared identity. The outer closure owns the creation snapshot and must be
        // mutable: its call operator is also instantiated by the erased Godot Callable bridge, and
        // a const operator would make every value copied into the inner per-call frame const on
        // GCC. The inner mutable closure is the fresh invocation frame and also becomes the root
        // captured by asynchronous continuations and nested lambdas.
        result += ", [=](const auto &" + arguments + ") mutable -> godot::Variant {\n";
        result += "    return [=]() mutable -> godot::Variant {\n";

        const auto saved_return = current_return_type_;
        const auto saved_callable = in_callable_lambda_;
        const auto saved_function = in_function_body_;
        const auto saved_coroutine_abi = current_coroutine_abi_;
        const auto saved_coroutine_state = current_coroutine_state_;
        const auto saved_debug_function = current_debug_function_;
        const auto saved_debug_instance = current_debug_instance_;
        const auto saved_async_continuation = in_async_continuation_;
        const auto saved_static_context = current_static_context_;
        current_return_type_ = lambda.return_type;
        in_callable_lambda_ = true;
        in_function_body_ = true;
        in_async_continuation_ = false;
        current_static_context_ = !lambda.owner_bound;
        current_coroutine_abi_ = lambda.is_coroutine;
        current_coroutine_state_ = lambda.is_coroutine
                                       ? "_gdpp_lambda_coroutine_state_" + std::to_string(identity)
                                       : std::string{};
        current_debug_function_ = lambda.name.empty() ? "<lambda>" : lambda.name;
        current_debug_instance_ = lambda.owner_bound ? saved_debug_instance : "nullptr";
        if (lambda.is_coroutine) {
            result += "    const auto " + current_coroutine_state_ +
                      " = gdpp::runtime::begin_coroutine(" +
                      (lambda.owner_bound ? self_object_expression() : std::string{"nullptr"}) +
                      ");\n";
        }
        // Parameter conversions and default expressions execute inside the called lambda, not in
        // the caller's fault frame. Install the callee frame before evaluating any of them so a
        // conversion/default failure terminates only this invocation and coroutine continuations
        // retain the same failure state.
        result += emit_script_function_scope(1);
        result += emit_debug_frame(lambda.span.begin.line, 1);
        const auto saved_dictionary_slots = proven_local_dictionary_slots_;
        const auto saved_dictionary_scope_depth = dictionary_proof_scope_depth_;
        proven_local_dictionary_slots_.clear();
        dictionary_proof_scope_depth_ = 0;
        if (has_parameter_control_flow(lambda.parameters)) {
            result += emit_parameter_cells(lambda.parameters, 1);
            result += emit_parameter_initialization_chain(lambda.parameters, lambda.rest_parameter,
                                                          lambda.body, arguments, 1, 0, false);
        } else {
            for (std::size_t index = 0; index < lambda.parameters.size(); ++index)
                result += emit_parameter_initializer(lambda.parameters[index], index, arguments, 1,
                                                     false);
            if (lambda.rest_parameter) {
                const auto rest_name = sanitize_identifier(lambda.rest_parameter->name);
                result += "    [[maybe_unused]] godot::Array " + rest_name + ";\n";
                result += "    " + rest_name + ".resize(" + arguments + ".size() > " +
                          std::to_string(lambda.parameters.size()) + " ? " + arguments +
                          ".size() - " + std::to_string(lambda.parameters.size()) + " : 0);\n";
                result += "    for (std::int64_t _gdpp_rest_index = " +
                          std::to_string(lambda.parameters.size()) + "; _gdpp_rest_index < " +
                          arguments + ".size(); ++_gdpp_rest_index) " + rest_name +
                          "[_gdpp_rest_index - " + std::to_string(lambda.parameters.size()) +
                          "] = " + arguments + "[static_cast<std::size_t>(_gdpp_rest_index)];\n";
            }
            result += emit_statements(lambda.body, 1);
        }
        proven_local_dictionary_slots_ = saved_dictionary_slots;
        dictionary_proof_scope_depth_ = saved_dictionary_scope_depth;
        current_return_type_ = saved_return;
        in_callable_lambda_ = saved_callable;
        in_function_body_ = saved_function;
        current_coroutine_abi_ = saved_coroutine_abi;
        current_coroutine_state_ = saved_coroutine_state;
        current_debug_function_ = saved_debug_function;
        current_debug_instance_ = saved_debug_instance;
        in_async_continuation_ = saved_async_continuation;
        current_static_context_ = saved_static_context;
        // Typed non-void lambdas have already passed semantic all-path return
        // analysis. Adding a defensive return here creates unreachable C++ and
        // fails warning-clean MSVC builds. A void GDScript lambda still needs a
        // Variant result for Callable's native bridge.
        if (!lambda.is_coroutine &&
            (lambda.return_type.kind == TypeKind::void_type ||
             requires_native_fallback(lambda.body) ||
             (lambda.return_type.is_dynamic() && native_statements_fall_through(lambda.body))))
            result += "    return godot::Variant();\n";
        result += "    }();\n})";
        if (!ordered_overrides.empty())
            result += ";\n}())";
        local_expression_overrides_ = saved_local_overrides;
        return result;
    }
    }
    return "godot::Variant()";
}

std::string CodeGenerator::emit_statements(const std::vector<typed::Statement>& statements,
                                           const std::size_t indentation,
                                           const std::size_t begin) const {
    const auto saved_overrides = local_expression_overrides_;
    const bool owns_dictionary_proof = dictionary_proof_scope_depth_ == 0;
    DictionarySlotProofs saved_dictionary_slots;
    if (owns_dictionary_proof) {
        saved_dictionary_slots = proven_local_dictionary_slots_;
        proven_local_dictionary_slots_ = prove_local_dictionary_slots(statements);
    }
    ++dictionary_proof_scope_depth_;
    auto result = emit_async_statements(statements, indentation, begin, {}, {}, false);
    --dictionary_proof_scope_depth_;
    local_expression_overrides_ = saved_overrides;
    if (owns_dictionary_proof)
        proven_local_dictionary_slots_ = std::move(saved_dictionary_slots);
    return result;
}

std::string CodeGenerator::lift_async_loop_locals(const typed::Statement& statement,
                                                  const std::size_t indentation) const {
    std::unordered_map<FlowSymbolId, AsyncLocalSymbol> assigned;
    std::unordered_set<FlowSymbolId> declared;
    collect_assigned_symbols(statement.body, assigned);
    collect_declared_symbols(statement.body, declared);
    std::map<FlowSymbolId, AsyncLocalSymbol> ordered(assigned.begin(), assigned.end());
    std::string result;
    for (const auto& [symbol, local] : ordered) {
        if (declared.find(symbol) != declared.end() ||
            local_expression_overrides_.find(symbol) != local_expression_overrides_.end()) {
            continue;
        }
        const auto cell = "_gdpp_async_cell_" + std::to_string(temporary_counter_++);
        result += indent(indentation) + "const auto " + cell + " = std::make_shared<" +
                  cpp_type(local.type) + ">(" + sanitize_identifier(local.name) + ");\n";
        local_expression_overrides_[symbol] = "(*" + cell + ")";
    }
    return result;
}

bool CodeGenerator::statement_contains_await(const typed::Statement& statement) const noexcept {
    if (statement.kind == typed::StatementKind::await_statement ||
        statement.kind == typed::StatementKind::await_variable)
        return await_can_suspend(statement);
    const auto contains = [this](const std::vector<typed::Statement>& statements) {
        return std::any_of(statements.begin(), statements.end(),
                           [this](const auto& child) { return statement_contains_await(child); });
    };
    return contains(statement.body) || contains(statement.else_body) ||
           contains(statement.guard_prefix) || contains(statement.assert_condition_prefix) ||
           contains(statement.assert_message_prefix);
}

bool CodeGenerator::await_can_suspend(const typed::Statement& statement) noexcept {
    if (!statement.expression)
        return false;
    const auto& type = statement.expression->type;
    return statement.expression->coroutine_call || type.is_dynamic() ||
           (type.kind == TypeKind::builtin && type.name == "Signal");
}

std::string CodeGenerator::async_return(const std::size_t indentation,
                                        const bool continuation_context) const {
    if (continuation_context)
        return indent(indentation) + "return;\n";
    if (current_coroutine_abi_)
        return indent(indentation) + "return gdpp::runtime::coroutine_result(" +
               current_coroutine_state_ + ");\n";
    if (current_return_type_.kind == TypeKind::void_type)
        return indent(indentation) + "return;\n";
    return indent(indentation) + "return {};\n";
}

std::string CodeGenerator::coroutine_return(const std::size_t indentation, std::string value,
                                            const bool continuation_context) const {
    const auto prefix = indent(indentation);
    std::string result = prefix + "gdpp::runtime::complete_coroutine(" + current_coroutine_state_ +
                         ", gdpp::runtime::to_variant(" + std::move(value) + "));\n";
    if (continuation_context)
        return result + prefix + "return;\n";
    return result + prefix + "return gdpp::runtime::coroutine_result(" + current_coroutine_state_ +
           ");\n";
}

std::string CodeGenerator::emit_script_function_scope(const std::size_t indentation,
                                                      const bool inherit_existing) const {
    std::string constructor;
    if (current_coroutine_abi_)
        constructor = "(" + current_coroutine_state_ + ")";
    else if (inherit_existing)
        constructor = "(gdpp::runtime::ScriptFaultPolicy::inherit_existing)";
    auto result = indent(indentation) +
                  "gdpp::runtime::ScriptFunctionScope _gdpp_script_function_scope" + constructor +
                  ";\n";
    // A synchronous invocation owns its fault state for the entire native call. Read that state
    // directly so Release hot loops do not perform a TLS lookup after every fault boundary.
    // Coroutine continuations can outlive this C++ scope. Give them a capture-free local accessor
    // that resolves through the thread-local state installed for each initial call/resumption.
    // The explicit declaration also shadows any stack-capturing accessor in the enclosing
    // synchronous function when that function creates a coroutine lambda.
    if (!current_coroutine_abi_) {
        result += indent(indentation) + "[[maybe_unused]] const auto script_function_failed = "
                                        "[&_gdpp_script_function_scope]() noexcept { "
                                        "return _gdpp_script_function_scope.failed(); };\n";
    } else {
        result += indent(indentation) + "[[maybe_unused]] const auto script_function_failed = "
                                        "[]() noexcept { return "
                                        "gdpp::runtime::script_function_failed(); };\n";
    }
    return result;
}

std::string CodeGenerator::emit_script_failure_return(const std::size_t indentation,
                                                      const bool continuation_context) const {
    const auto prefix = indent(indentation);
    std::string result = prefix + "if (script_function_failed()) {\n";
    if (current_coroutine_abi_) {
        result += coroutine_return(indentation + 1, "godot::Variant{}", continuation_context);
    } else if (in_callable_lambda_) {
        result += indent(indentation + 1);
        if (current_return_type_.kind == TypeKind::void_type || current_return_type_.is_dynamic() ||
            current_return_type_.kind == TypeKind::nil) {
            result += "return godot::Variant{};\n";
        } else {
            result += "return gdpp::runtime::to_variant(" +
                      native_default_value(current_return_type_) + ");\n";
        }
    } else if (continuation_context || current_return_type_.kind == TypeKind::void_type) {
        result += indent(indentation + 1) + "return;\n";
    } else {
        result += indent(indentation + 1) + "return {};\n";
    }
    return result + prefix + "}\n";
}

std::string CodeGenerator::emit_suspension_lifetime(const typed::Statement& statement,
                                                    const std::size_t indentation) const {
    std::string result;
    for (const auto& variable : statement.suspension_variables) {
        const auto override = local_expression_overrides_.find(variable.symbol_identity);
        const auto expression = override == local_expression_overrides_.end()
                                    ? sanitize_identifier(variable.name)
                                    : override->second;
        result += indent(indentation) + "static_cast<void>(" + expression + ");\n";
    }
    return result;
}

bool CodeGenerator::can_emit_flat_async(const typed::Function& function,
                                        const mir::ControlFlowFunction& mir_function) const {
    // A long source-level callback chain produces recursively nested lambda types. MSVC can use
    // more than 10 GiB compiling such a function before failing with C1060. Flat MIR dispatch is
    // safe whenever the function has no locals that must be lifted across suspension and no loop
    // or match state that still requires a dedicated coroutine frame. Shorter/complex functions
    // keep the structured lowering until general local liveness is represented in MIR.
    const auto suspensions = std::count_if(
        mir_function.blocks.begin(), mir_function.blocks.end(),
        [](const auto& block) { return block.terminator.kind == mir::TerminatorKind::suspend; });
    const MirSourceIndex source_index{function.body};
    if (source_index.statement_count() != mir_function.source_statement_count ||
        source_index.expression_count() != mir_function.source_expression_count) {
        return false;
    }
    for (const auto& block : mir_function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (!source_index.statement(instruction.source_statement))
                return false;
        }
        if (block.terminator.condition_value != mir::invalid_value) {
            if (block.terminator.condition_value >= mir_function.values.size())
                return false;
            const auto source_expression =
                mir_function.values[block.terminator.condition_value].source_expression;
            if (!source_index.expression(source_expression))
                return false;
        }
    }
    std::unordered_set<FlowSymbolId> declared;
    collect_declared_symbols(function.body, declared);
    return mir_function.suspends && suspensions >= 8 && declared.empty() &&
           std::all_of(function.body.begin(), function.body.end(), flat_async_statement_supported);
}

std::string CodeGenerator::emit_flat_async(const typed::Function& source,
                                           const mir::ControlFlowFunction& function,
                                           const std::size_t indentation) const {
    const MirSourceIndex source_index{source.body};
    const auto terminator_expression =
        [&](const mir::Terminator& terminator) -> const typed::Expression* {
        if (terminator.condition_value >= function.values.size())
            return nullptr;
        return source_index.expression(
            function.values[terminator.condition_value].source_expression);
    };
    const auto prefix = indent(indentation);
    const auto identity = std::to_string(temporary_counter_++);
    const auto step_type = "_gdpp_async_step_type_" + identity;
    const auto step = "_gdpp_async_step_" + identity;
    const auto weak_step = "_gdpp_async_weak_step_" + identity;
    const auto keep_alive = "_gdpp_async_keep_alive_" + identity;
    const auto behavior_lifetime = "_gdpp_attached_behavior_lifetime_" + identity;
    const auto pc = "_gdpp_async_pc_" + identity;
    const auto values = "_gdpp_async_values_" + identity;

    std::string result;
    if (attached_script_ && !current_static_context_) {
        result += prefix +
                  "const godot::Ref<gdpp::runtime::AttachedScriptBehavior> " +
                  behavior_lifetime + "{this};\n";
    }
    result += prefix + "using " + step_type +
              " = std::function<void(std::size_t, const godot::Array &)>;\n";
    result += prefix + "const auto " + step + " = std::make_shared<" + step_type + ">();\n";
    result += prefix + "const std::weak_ptr<" + step_type + "> " + weak_step + " = " + step + ";\n";
    result += prefix + "*" + step + " = [=](std::size_t " + pc + ", const godot::Array &" + values +
              ") mutable {\n";
    if (attached_script_ && !current_static_context_)
        result += indent(indentation + 1) + "static_cast<void>(" + behavior_lifetime + ");\n";
    for (const auto& parameter : source.parameters) {
        result += indent(indentation + 1) + "static_cast<void>(" +
                  parameter_native_name(parameter) + ");\n";
    }
    if (source.rest_parameter) {
        result += indent(indentation + 1) + "static_cast<void>(" +
                  sanitize_identifier(source.rest_parameter->name) + ");\n";
    }
    result += emit_script_function_scope(indentation + 1);
    result += indent(indentation + 1) + "static_cast<void>(" + values + ");\n";
    result +=
        indent(indentation + 1) + "const auto " + keep_alive + " = " + weak_step + ".lock();\n";
    result += indent(indentation + 1) + "if (!" + keep_alive + ") return;\n";
    result += indent(indentation + 1) + "while (true) {\n";
    result += indent(indentation + 2) + "switch (" + pc + ") {\n";

    const auto saved_async_continuation = in_async_continuation_;
    in_async_continuation_ = true;
    for (const auto& block : function.blocks) {
        result += indent(indentation + 2) + "case " + std::to_string(block.id) + ": {\n";
        for (const auto& instruction : block.instructions) {
            const auto* statement = source_index.statement(instruction.source_statement);
            if (!statement || instruction.kind == mir::InstructionKind::suspend_value)
                continue;
            result += emit_statement(*statement, indentation + 3);
        }
        const auto& terminator = block.terminator;
        const auto* expression = terminator_expression(terminator);
        switch (terminator.kind) {
        case mir::TerminatorKind::jump:
            result += indent(indentation + 3) + pc + " = " +
                      std::to_string(terminator.targets.front()) + ";\n";
            result += indent(indentation + 3) + "continue;\n";
            break;
        case mir::TerminatorKind::branch:
            result += indent(indentation + 3) +
                      "const bool _gdpp_flat_condition = " + emit_truthy(*expression) + ";\n";
            result += emit_script_failure_return(indentation + 3, true);
            result += indent(indentation + 3) + pc + " = _gdpp_flat_condition ? " +
                      std::to_string(terminator.targets[0]) + " : " +
                      std::to_string(terminator.targets[1]) + ";\n";
            result += indent(indentation + 3) + "continue;\n";
            break;
        case mir::TerminatorKind::suspend: {
            const auto target = std::to_string(terminator.targets.front());
            const auto awaitable = "_gdpp_flat_awaitable_" + std::to_string(temporary_counter_++);
            result += indent(indentation + 3) + "const godot::Variant " + awaitable +
                      " = gdpp::runtime::to_variant(" + emit_expression(*expression) + ");\n";
            result += emit_script_failure_return(indentation + 3, true);
            result += indent(indentation + 3) + "if (!gdpp::runtime::is_awaitable(" + awaitable +
                      ")) {\n";
            result += indent(indentation + 4) + pc + " = " + target + ";\n";
            result += indent(indentation + 4) + "continue;\n";
            result += indent(indentation + 3) + "}\n";
            result += indent(indentation + 3) + "if (!gdpp::runtime::await_signal(" + awaitable +
                      ", " + await_owner_expression() + ", " +
                      (current_coroutine_state_.empty() ? "gdpp::runtime::CoroutineStatePtr{}"
                                                        : current_coroutine_state_) +
                      ", [" + keep_alive + "](const godot::Array &resume_values) { (*" +
                      keep_alive + ")(" + target + ", resume_values); })) {\n";
            result += current_coroutine_abi_
                          ? coroutine_return(indentation + 4, "godot::Variant{}", true)
                          : indent(indentation + 4) + "return;\n";
            result += indent(indentation + 3) + "}\n";
            result += indent(indentation + 3) + "return;\n";
            break;
        }
        case mir::TerminatorKind::return_value:
            if (current_coroutine_abi_) {
                if (expression) {
                    result += indent(indentation + 3) + "const auto _gdpp_flat_return_value = " +
                              emit_expression(*expression) + ";\n";
                    result += emit_script_failure_return(indentation + 3, true);
                    result += coroutine_return(indentation + 3, "_gdpp_flat_return_value", true);
                } else {
                    result += coroutine_return(indentation + 3, "godot::Variant{}", true);
                }
            } else {
                result += indent(indentation + 3) + "return;\n";
            }
            break;
        case mir::TerminatorKind::stop:
            result += current_coroutine_abi_
                          ? coroutine_return(indentation + 3, "godot::Variant{}", true)
                          : indent(indentation + 3) + "return;\n";
            break;
        case mir::TerminatorKind::invalid:
            result += indent(indentation + 3) + "return;\n";
            break;
        }
        result += indent(indentation + 2) + "}\n";
    }
    in_async_continuation_ = saved_async_continuation;
    result += indent(indentation + 2) + "default: return;\n";
    result += indent(indentation + 2) + "}\n";
    result += indent(indentation + 1) + "}\n";
    result += prefix + "};\n";
    result += prefix + "(*" + step + ")(" + std::to_string(function.entry) + ", godot::Array{});\n";
    if (current_coroutine_abi_)
        result += async_return(indentation, false);
    return result;
}

std::string CodeGenerator::emit_async_match_branch(
    const typed::Statement& branch, const std::size_t next_branch, const std::size_t after_branch,
    const std::string& value_name, const std::string& keep_alive, const std::size_t indentation,
    std::shared_ptr<const AsyncLoopControl> loop_control) const {
    std::string condition;
    std::vector<MatchBinding> bindings;
    for (const auto& pattern : branch.patterns) {
        collect_match_bindings(pattern, bindings);
        if (!condition.empty())
            condition += " || ";
        condition += "(" + emit_match_pattern(pattern, value_name, bindings) + ")";
    }
    if (condition.empty())
        condition = "false";

    const auto prefix = indent(indentation);
    std::string result;
    for (const auto& binding : bindings)
        result += prefix + "godot::Variant " + binding.slot + ";\n";
    result += prefix + "if (" + condition + ") {\n";
    const auto content_indent = indentation + 1;
    for (const auto& binding : bindings) {
        result += indent(content_indent) + "[[maybe_unused]] " + cpp_type(binding.type) + " " +
                  sanitize_identifier(binding.name) + " = " +
                  emit_conversion(binding.type, {TypeKind::variant, "Variant"}, binding.slot,
                                  &branch.span) +
                  ";\n";
    }

    const auto dispatch = [&](const std::size_t target, const std::size_t target_indent) {
        return indent(target_indent) + "(*" + keep_alive + ")(" + std::to_string(target) + ");\n";
    };
    const auto emit_branch_body = [&](const std::size_t body_indent) -> std::string {
        return emit_async_statements(branch.body, body_indent, 0, {}, dispatch(after_branch, 0),
                                     true, loop_control);
    };
    const auto emit_fallback = [&](const std::size_t fallback_indent) -> std::string {
        return dispatch(next_branch, fallback_indent);
    };

    if (!branch.expression) {
        result += emit_branch_body(content_indent);
    } else if (branch.guard_prefix.empty()) {
        result += indent(content_indent) + "if (" + emit_truthy(*branch.expression) + ") {\n";
        result += emit_branch_body(content_indent + 1);
        result += indent(content_indent) + "} else {\n";
        result += emit_fallback(content_indent + 1);
        result += indent(content_indent) + "}\n";
    } else {
        std::string completion = "if (" + emit_truthy(*branch.expression) + ") {\n";
        completion += emit_branch_body(1);
        completion += "} else {\n";
        completion += emit_fallback(1);
        completion += "}\n";
        result += emit_async_statements(branch.guard_prefix, content_indent, 0, {}, completion,
                                        true, loop_control);
    }
    result += prefix + "} else {\n";
    result += emit_fallback(indentation + 1);
    result += prefix + "}\n";
    return result;
}

std::string CodeGenerator::emit_assert_failure(const typed::Statement& statement,
                                               const std::size_t indentation,
                                               const bool continuation_context) const {
    const auto location = current_source_path_ + ":" + std::to_string(statement.span.begin.line);
    std::string message = godot_string("Assertion failed at " + location);
    if (statement.expression) {
        message += " + godot::String(\": \") + godot::String(" +
                   emit_expression(*statement.expression) + ")";
    }
    const auto prefix = indent(indentation);
    if (current_coroutine_abi_) {
        auto result = prefix + "gdpp::runtime::complete_coroutine(" + current_coroutine_state_ +
                      ", godot::Variant{});\n";
        if (continuation_context)
            return result + prefix + "ERR_FAIL_EDMSG(" + message + ");\n";
        return result + prefix + "ERR_FAIL_V_EDMSG(gdpp::runtime::coroutine_result(" +
               current_coroutine_state_ + "), " + message + ");\n";
    }
    if (continuation_context)
        return prefix + "ERR_FAIL_EDMSG(" + message + ");\n";
    if (in_callable_lambda_)
        return prefix + "ERR_FAIL_V_EDMSG(godot::Variant{}, " + message + ");\n";
    if (current_return_type_.kind == TypeKind::void_type)
        return prefix + "ERR_FAIL_EDMSG(" + message + ");\n";
    return prefix + "ERR_FAIL_V_EDMSG({}, " + message + ");\n";
}

std::string CodeGenerator::emit_async_statements(
    const std::vector<typed::Statement>& statements, const std::size_t indentation,
    const std::size_t begin, std::vector<StatementSlice> tails, const std::string& terminal,
    const bool continuation_context, std::shared_ptr<const AsyncLoopControl> loop_control) const {
    std::string result;
    for (std::size_t index = begin; index < statements.size(); ++index) {
        const auto& statement = statements[index];
        if (loop_control && statement.kind == typed::StatementKind::break_statement) {
            result += emit_async_statements({}, indentation, 0, loop_control->break_tails,
                                            loop_control->break_terminal, continuation_context,
                                            loop_control->parent);
            result += async_return(indentation, continuation_context);
            return result;
        }
        if (loop_control && statement.kind == typed::StatementKind::continue_statement) {
            result += indent(indentation) + loop_control->continue_terminal + "\n";
            result += async_return(indentation, continuation_context);
            return result;
        }
        const bool requires_loop_control = loop_control && contains_current_loop_control(statement);
        if (!statement_contains_await(statement) && !requires_loop_control) {
            if (statement.kind == typed::StatementKind::return_statement) {
                if (current_coroutine_abi_ || !continuation_context) {
                    const auto saved_async_continuation = in_async_continuation_;
                    in_async_continuation_ = in_async_continuation_ || continuation_context;
                    result += emit_statement(statement, indentation);
                    in_async_continuation_ = saved_async_continuation;
                } else {
                    result += async_return(indentation, true);
                }
                return result;
            }
            {
                const auto saved_async_continuation = in_async_continuation_;
                in_async_continuation_ = in_async_continuation_ || continuation_context;
                result += emit_statement(statement, indentation);
                in_async_continuation_ = saved_async_continuation;
            }
            continue;
        }

        auto continuation = tails;
        if (index + 1 < statements.size())
            continuation.insert(continuation.begin(), StatementSlice{&statements, index + 1});
        const auto prefix = indent(indentation);
        if (statement.kind == typed::StatementKind::await_statement ||
            statement.kind == typed::StatementKind::await_variable) {
            const auto identity = std::to_string(temporary_counter_++);
            const auto awaitable_name = "_gdpp_awaitable_" + identity;
            const auto result_name = "_gdpp_await_values_" + identity;
            const auto resume_name = "_gdpp_resume_" + identity;
            const auto immediate_name = "_gdpp_immediate_" + identity;
            const auto behavior_lifetime = "_gdpp_attached_behavior_lifetime_" + identity;
            result += prefix + "const godot::Variant " + awaitable_name +
                      " = gdpp::runtime::to_variant(" + emit_expression(*statement.expression) +
                      ");\n";
            result += emit_script_failure_return(indentation, continuation_context);
            if (attached_script_ && !current_static_context_) {
                result += prefix +
                          "const godot::Ref<gdpp::runtime::AttachedScriptBehavior> " +
                          behavior_lifetime + "{this};\n";
            }
            result += prefix + "auto " + resume_name + " = [=](const godot::Array &" + result_name +
                      ") mutable {\n";
            result += emit_script_function_scope(indentation + 1);
            result += indent(indentation + 1) + "static_cast<void>(" + result_name + ");\n";
            if (attached_script_ && !current_static_context_) {
                result +=
                    indent(indentation + 1) + "static_cast<void>(" + behavior_lifetime + ");\n";
            }
            result += emit_suspension_lifetime(statement, indentation + 1);
            if (statement.kind == typed::StatementKind::await_variable) {
                result += indent(indentation + 1) + "[[maybe_unused]] " +
                          cpp_type(statement.declared_type) + " " +
                          sanitize_identifier(statement.name) + " = " +
                          emit_conversion(statement.declared_type, {TypeKind::variant, "Variant"},
                                          "gdpp::runtime::await_result(" + result_name + ")",
                                          &statement.span) +
                          ";\n";
            }
            result += emit_async_statements(statements, indentation + 1, index + 1, tails, terminal,
                                            true, loop_control);
            result += prefix + "};\n";
            result += prefix + "if (!gdpp::runtime::is_awaitable(" + awaitable_name + ")) {\n";
            result += indent(indentation + 1) + "godot::Array " + immediate_name + ";\n";
            result +=
                indent(indentation + 1) + immediate_name + ".push_back(" + awaitable_name + ");\n";
            result += indent(indentation + 1) + resume_name + "(" + immediate_name + ");\n";
            result += async_return(indentation + 1, continuation_context);
            result += prefix + "}\n";
            result += prefix + "if (!gdpp::runtime::await_signal(" + awaitable_name + ", " +
                      await_owner_expression() + ", " +
                      (current_coroutine_state_.empty() ? "gdpp::runtime::CoroutineStatePtr{}"
                                                        : current_coroutine_state_) +
                      ", " + resume_name + ")) {\n";
            result += current_coroutine_abi_ ? coroutine_return(indentation + 1, "godot::Variant{}",
                                                                continuation_context)
                                             : async_return(indentation + 1, continuation_context);
            result += prefix + "}\n" + async_return(indentation, continuation_context);
            return result;
        }

        if (statement.kind == typed::StatementKind::if_statement) {
            const auto condition_name =
                "_gdpp_async_if_condition_" + std::to_string(temporary_counter_++);
            result += prefix + "const bool " + condition_name + " = " +
                      emit_truthy(*statement.condition) + ";\n";
            result += emit_script_failure_return(indentation, continuation_context);
            result += prefix + "if (" + condition_name + ") {\n";
            result += emit_async_statements(statement.body, indentation + 1, 0, continuation,
                                            terminal, continuation_context, loop_control);
            result += prefix + "} else {\n";
            result += emit_async_statements(statement.else_body, indentation + 1, 0, continuation,
                                            terminal, continuation_context, loop_control);
            result += prefix + "}\n";
            return result;
        }

        if (statement.kind == typed::StatementKind::assert_statement) {
            const auto identity = std::to_string(temporary_counter_++);
            const auto after_assert = "_gdpp_after_assert_" + identity;
            result += prefix + "{\n" + indent(indentation + 1) + "auto " + after_assert +
                      " = [=]() mutable {\n";
            result += emit_async_statements({}, indentation + 2, 0, continuation, terminal, true,
                                            loop_control);
            result += indent(indentation + 1) + "};\n";

            const auto prefix_suspends =
                [this](const std::vector<typed::Statement>& prefix_statements) {
                    return std::any_of(
                        prefix_statements.begin(), prefix_statements.end(),
                        [this](const auto& child) { return statement_contains_await(child); });
                };
            const auto condition_continuation =
                continuation_context || prefix_suspends(statement.assert_condition_prefix);
            const auto failure = emit_assert_failure(statement, 0, true);
            const auto message_path =
                emit_async_statements(statement.assert_message_prefix, 1, 0, {}, failure,
                                      condition_continuation, loop_control);
            std::string condition_terminal = "if (" + emit_truthy(*statement.condition) + ") {\n";
            condition_terminal += indent(1) + after_assert + "();\n";
            condition_terminal += "} else {\n" + message_path + "}\n";

            result += indent(indentation + 1) + "#ifdef GDPP_SCRIPT_DEBUG_ENABLED\n";
            result +=
                emit_async_statements(statement.assert_condition_prefix, indentation + 1, 0, {},
                                      condition_terminal, continuation_context, loop_control);
            result += indent(indentation + 1) + "#else\n" + indent(indentation + 1) + after_assert +
                      "();\n" + indent(indentation + 1) + "#endif\n";
            result += async_return(indentation + 1, continuation_context) + prefix + "}\n";
            return result;
        }

        if (statement.kind == typed::StatementKind::match_statement) {
            const auto identity = std::to_string(match_counter_++);
            const auto value_name = "_gdpp_async_match_value_" + identity;
            const auto dispatch_type = "_gdpp_async_match_dispatch_type_" + identity;
            const auto dispatch = "_gdpp_async_match_dispatch_" + identity;
            const auto weak_dispatch = "_gdpp_async_match_weak_dispatch_" + identity;
            const auto keep_alive = "_gdpp_async_match_keep_alive_" + identity;
            const auto branch_index = "_gdpp_async_match_branch_" + identity;
            const auto after_branch = statement.body.size();
            result += prefix + "{\n" + indent(indentation + 1) + "const auto " + value_name +
                      " = " + emit_expression(*statement.condition) + ";\n" +
                      emit_script_failure_return(indentation + 1, continuation_context) +
                      indent(indentation + 1) + "using " + dispatch_type +
                      " = std::function<void(std::size_t)>;\n" + indent(indentation + 1) +
                      "const auto " + dispatch + " = std::make_shared<" + dispatch_type + ">();\n" +
                      indent(indentation + 1) + "const std::weak_ptr<" + dispatch_type + "> " +
                      weak_dispatch + " = " + dispatch + ";\n" + indent(indentation + 1) + "*" +
                      dispatch + " = [=](std::size_t " + branch_index + ") mutable {\n" +
                      indent(indentation + 2) + "const auto " + keep_alive + " = " + weak_dispatch +
                      ".lock();\n" + indent(indentation + 2) + "if (!" + keep_alive +
                      ") return;\n" + indent(indentation + 2) + "switch (" + branch_index + ") {\n";
            for (std::size_t branch = 0; branch < statement.body.size(); ++branch) {
                result += indent(indentation + 2) + "case " + std::to_string(branch) + ": {\n";
                result +=
                    emit_async_match_branch(statement.body[branch], branch + 1, after_branch,
                                            value_name, keep_alive, indentation + 3, loop_control);
                result += indent(indentation + 3) + "return;\n" + indent(indentation + 2) + "}\n";
            }
            result += indent(indentation + 2) + "case " + std::to_string(after_branch) + ": {\n";
            result += emit_async_statements({}, indentation + 3, 0, continuation, terminal, true,
                                            loop_control);
            result += indent(indentation + 3) + "return;\n" + indent(indentation + 2) + "}\n" +
                      indent(indentation + 2) + "default: return;\n" + indent(indentation + 2) +
                      "}\n" + indent(indentation + 1) + "};\n" + indent(indentation + 1) + "(*" +
                      dispatch + ")(0);\n" + async_return(indentation + 1, continuation_context) +
                      prefix + "}\n";
            return result;
        }

        if (statement.kind == typed::StatementKind::for_statement) {
            const auto saved_overrides = local_expression_overrides_;
            const auto identity = temporary_counter_++;
            const auto suffix = std::to_string(identity);
            const auto iterable = "_gdpp_async_iterable_" + suffix;
            const auto iterator = "_gdpp_async_iterator_" + suffix;
            const auto available = "_gdpp_async_available_" + suffix;
            const auto loop = "_gdpp_async_loop_" + suffix;
            const auto weak_loop = "_gdpp_async_weak_loop_" + suffix;
            const auto keep_alive = "_gdpp_async_keep_loop_" + suffix;
            // Install loop-carried storage overrides before emitting any expression that can
            // reference those locals. Keeping the mutating lift call in an operator+ chain leaves
            // its evaluation order relative to emit_expression unspecified, which can make the
            // iterator and body capture different storage.
            result += prefix + "{\n";
            result += lift_async_loop_locals(statement, indentation + 1);
            result +=
                indent(indentation + 1) + "const auto " + iterable +
                " = std::make_shared<godot::Variant>(" + emit_expression(*statement.condition) +
                ");\n" + indent(indentation + 1) + "if (script_function_failed()) {\n" +
                async_return(indentation + 2, continuation_context) + indent(indentation + 1) +
                "}\n" + indent(indentation + 1) + "const auto " + iterator +
                " = std::make_shared<godot::Variant>();\n" + indent(indentation + 1) +
                "const auto " + available + " = std::make_shared<bool>(gdpp::runtime::iter_init(*" +
                iterable + ", *" + iterator + ", " + script_location(statement.condition->span) +
                "));\n" + indent(indentation + 1) + "if (script_function_failed()) {\n" +
                async_return(indentation + 2, continuation_context) + indent(indentation + 1) +
                "}\n" + indent(indentation + 1) + "const auto " + loop +
                " = std::make_shared<std::function<void()>>();\n" + indent(indentation + 1) +
                "const std::weak_ptr<std::function<void()>> " + weak_loop + " = " + loop + ";\n" +
                indent(indentation + 1) + "*" + loop + " = [=]() mutable {\n" +
                indent(indentation + 2) + "const auto " + keep_alive + " = " + weak_loop +
                ".lock();\n" + indent(indentation + 2) + "if (!" + keep_alive + ") return;\n" +
                indent(indentation + 2) + "if (!*" + available + ") {\n";
            result += emit_async_statements({}, indentation + 3, 0, continuation, terminal, true,
                                            loop_control);
            result += async_return(indentation + 3, true) + indent(indentation + 2) + "}\n" +
                      indent(indentation + 2) + "[[maybe_unused]] " +
                      (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                            : cpp_type(statement.declared_type)) +
                      " " + sanitize_identifier(statement.name) + " = " +
                      emit_conversion(statement.declared_type, {TypeKind::variant, "Variant"},
                                      "gdpp::runtime::iter_get(*" + iterable + ", *" + iterator +
                                          ", " + script_location(statement.condition->span) + ")",
                                      &statement.span) +
                      ";\n" + indent(indentation + 2) + "if (script_function_failed()) return;\n";
            const auto advance =
                "*" + available + " = gdpp::runtime::iter_next(*" + iterable + ", *" + iterator +
                ", " + script_location(statement.condition->span) +
                "); if (script_function_failed()) return; (*" + keep_alive + ")();";
            auto nested_loop = std::make_shared<AsyncLoopControl>();
            nested_loop->break_tails = continuation;
            nested_loop->break_terminal = terminal;
            nested_loop->continue_terminal = advance;
            nested_loop->parent = loop_control;
            result += emit_async_statements(statement.body, indentation + 2, 0, {},
                                            advance + " return;", true, nested_loop);
            result += indent(indentation + 1) + "};\n" + indent(indentation + 1) + "(*" + loop +
                      ")();\n" + async_return(indentation + 1, continuation_context) + prefix +
                      "}\n";
            local_expression_overrides_ = saved_overrides;
            return result;
        }

        if (statement.kind == typed::StatementKind::while_statement) {
            const auto saved_overrides = local_expression_overrides_;
            const auto suffix = std::to_string(temporary_counter_++);
            const auto loop = "_gdpp_async_loop_" + suffix;
            const auto weak_loop = "_gdpp_async_weak_loop_" + suffix;
            const auto keep_alive = "_gdpp_async_keep_loop_" + suffix;
            // The condition is reevaluated by the loop callback after every suspension, so it
            // must read the same shared cells that assignments in the body update.
            result += prefix + "{\n";
            result += lift_async_loop_locals(statement, indentation + 1);
            result += indent(indentation + 1) + "const auto " + loop +
                      " = std::make_shared<std::function<void()>>();\n" + indent(indentation + 1) +
                      "const std::weak_ptr<std::function<void()>> " + weak_loop + " = " + loop +
                      ";\n" + indent(indentation + 1) + "*" + loop + " = [=]() mutable {\n" +
                      indent(indentation + 2) + "const auto " + keep_alive + " = " + weak_loop +
                      ".lock();\n" + indent(indentation + 2) + "if (!" + keep_alive +
                      ") return;\n" + indent(indentation + 2) + "if (!(" +
                      emit_truthy(*statement.condition) + ")) {\n";
            result += emit_async_statements({}, indentation + 3, 0, continuation, terminal, true,
                                            loop_control);
            result += async_return(indentation + 3, true) + indent(indentation + 2) + "}\n";
            auto nested_loop = std::make_shared<AsyncLoopControl>();
            nested_loop->break_tails = continuation;
            nested_loop->break_terminal = terminal;
            nested_loop->continue_terminal = "(*" + keep_alive + ")();";
            nested_loop->parent = loop_control;
            result += emit_async_statements(statement.body, indentation + 2, 0, {},
                                            "(*" + keep_alive + ")(); return;", true, nested_loop);
            result += indent(indentation + 1) + "};\n" + indent(indentation + 1) + "(*" + loop +
                      ")();\n" + async_return(indentation + 1, continuation_context) + prefix +
                      "}\n";
            local_expression_overrides_ = saved_overrides;
            return result;
        }

        diagnostics_.error("GDS3006", "unsupported structured await reached code generation",
                           statement.span);
        return result;
    }

    if (!tails.empty()) {
        const auto next = tails.front();
        tails.erase(tails.begin());
        if (next.statements) {
            result +=
                emit_async_statements(*next.statements, indentation, next.begin, std::move(tails),
                                      terminal, continuation_context, std::move(loop_control));
        }
    } else if (!terminal.empty()) {
        result += indent_block(indentation, terminal);
    } else if (current_coroutine_abi_) {
        result += coroutine_return(indentation, "godot::Variant{}", continuation_context);
    }
    return result;
}

void CodeGenerator::collect_match_bindings(const typed::MatchPattern& pattern,
                                           std::vector<MatchBinding>& bindings) const {
    if (pattern.kind == typed::MatchPatternKind::binding) {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.name == pattern.name;
        });
        if (found == bindings.end()) {
            bindings.push_back({pattern.name,
                                "_gdpp_match_bind_" + std::to_string(temporary_counter_++),
                                pattern.type});
        }
    }
    for (const auto& element : pattern.elements)
        collect_match_bindings(element, bindings);
}

std::string CodeGenerator::emit_match_pattern(const typed::MatchPattern& pattern,
                                              const std::string& candidate,
                                              const std::vector<MatchBinding>& bindings) const {
    switch (pattern.kind) {
    case typed::MatchPatternKind::value: {
        const auto suffix = std::to_string(temporary_counter_++);
        const auto actual = "_gdpp_pattern_actual_" + suffix;
        const auto expected = "_gdpp_pattern_expected_" + suffix;
        return "([&]() -> bool { const godot::Variant " + actual + " = gdpp::runtime::to_variant(" +
               candidate + "); const godot::Variant " + expected + " = gdpp::runtime::to_variant(" +
               emit_expression(*pattern.expression) +
               "); return static_cast<bool>(gdpp::runtime::binary(" + "godot::Variant::OP_EQUAL, " +
               actual + ", " + expected + ", " + script_location(pattern.span) + ")); }())";
    }
    case typed::MatchPatternKind::wildcard:
    case typed::MatchPatternKind::rest:
        return "true";
    case typed::MatchPatternKind::binding: {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.name == pattern.name;
        });
        if (found == bindings.end()) {
            diagnostics_.error("GDS3005", "match binding has no native storage slot", pattern.span);
            return "false";
        }
        return "([&]() -> bool { " + found->slot + " = gdpp::runtime::to_variant(" + candidate +
               "); return true; }())";
    }
    case typed::MatchPatternKind::array: {
        const auto suffix = std::to_string(temporary_counter_++);
        const auto actual = "_gdpp_pattern_array_value_" + suffix;
        const auto array = "_gdpp_pattern_array_" + suffix;
        const bool has_rest = !pattern.elements.empty() &&
                              pattern.elements.back().kind == typed::MatchPatternKind::rest;
        const auto fixed_size = pattern.elements.size() - (has_rest ? 1U : 0U);
        std::string result = "([&]() -> bool { const godot::Variant " + actual +
                             " = gdpp::runtime::to_variant(" + candidate + "); if (" + actual +
                             ".get_type() != godot::Variant::ARRAY) return false; " +
                             "const godot::Array " + array + " = " + actual + "; if (" + array +
                             ".size() " + (has_rest ? "< " : "!= ") + std::to_string(fixed_size) +
                             ") return false; ";
        for (std::size_t index = 0; index < fixed_size; ++index) {
            result += "if (!(" +
                      emit_match_pattern(pattern.elements[index],
                                         array + "[" + std::to_string(index) + "]", bindings) +
                      ")) return false; ";
        }
        return result + "return true; }())";
    }
    case typed::MatchPatternKind::dictionary: {
        const auto suffix = std::to_string(temporary_counter_++);
        const auto actual = "_gdpp_pattern_dictionary_value_" + suffix;
        const auto dictionary = "_gdpp_pattern_dictionary_" + suffix;
        const bool has_rest = !pattern.elements.empty() && !pattern.keys.empty() &&
                              !pattern.keys.back() &&
                              pattern.elements.back().kind == typed::MatchPatternKind::rest;
        const auto fixed_size = pattern.elements.size() - (has_rest ? 1U : 0U);
        std::string result = "([&]() -> bool { const godot::Variant " + actual +
                             " = gdpp::runtime::to_variant(" + candidate + "); if (" + actual +
                             ".get_type() != godot::Variant::DICTIONARY) return false; " +
                             "const godot::Dictionary " + dictionary + " = " + actual + "; if (" +
                             dictionary + ".size() " + (has_rest ? "< " : "!= ") +
                             std::to_string(fixed_size) + ") return false; ";
        for (std::size_t index = 0; index < fixed_size; ++index) {
            const auto key = "_gdpp_pattern_key_" + std::to_string(temporary_counter_++);
            result += "const godot::Variant " + key + " = gdpp::runtime::to_variant(" +
                      emit_expression(*pattern.keys[index]) + "); if (!" + dictionary + ".has(" +
                      key + ")) return false; ";
            if (pattern.elements[index].kind != typed::MatchPatternKind::wildcard) {
                result += "if (!(" +
                          emit_match_pattern(pattern.elements[index], dictionary + "[" + key + "]",
                                             bindings) +
                          ")) return false; ";
            }
        }
        return result + "return true; }())";
    }
    }
    diagnostics_.error("GDS3005", "unknown match pattern reached code generation", pattern.span);
    return "false";
}

std::string CodeGenerator::emit_debug_frame(const std::size_t line,
                                            const std::size_t indentation) const {
    if (current_debug_source_.empty() || current_debug_function_.empty() ||
        current_debug_instance_.empty()) {
        return {};
    }
    const auto prefix = indent(indentation);
    return prefix + "#ifdef GDPP_SCRIPT_DEBUG_ENABLED\n" + prefix +
           "[[maybe_unused]] gdpp::runtime::ScriptDebugFrame _gdpp_debug_frame(" +
           godot_string(current_debug_source_) + ", " + godot_string_name(current_debug_function_) +
           ", " + std::to_string(line) + ", " + current_debug_instance_ + ");\n" + prefix +
           "#endif\n";
}

std::string CodeGenerator::emit_debug_line(const std::size_t line,
                                           const std::size_t indentation) const {
    if (current_debug_source_.empty() || current_debug_function_.empty() ||
        current_debug_instance_.empty() || in_async_continuation_) {
        return {};
    }
    const auto prefix = indent(indentation);
    return prefix + "#ifdef GDPP_SCRIPT_DEBUG_ENABLED\n" + prefix + "_gdpp_debug_frame.set_line(" +
           std::to_string(line) + ");\n" + prefix + "#endif\n";
}

std::string CodeGenerator::emit_debug_breakpoint(const typed::Statement& statement,
                                                 const std::size_t indentation) const {
    const auto prefix = indent(indentation);
    const auto body = indent(indentation + 1);
    const auto suffix = std::to_string(temporary_counter_++);
    const auto local_names = "_gdpp_debug_local_names_" + suffix;
    const auto local_values = "_gdpp_debug_local_values_" + suffix;
    const auto member_names = "_gdpp_debug_member_names_" + suffix;
    const auto member_values = "_gdpp_debug_member_values_" + suffix;
    std::string result = prefix + "#ifdef GDPP_SCRIPT_DEBUG_ENABLED\n" + prefix + "{\n" + body +
                         "godot::PackedStringArray " + local_names + ";\n" + body +
                         "godot::Array " + local_values + ";\n";
    for (const auto& variable : statement.debug_variables) {
        const auto override = local_expression_overrides_.find(variable.symbol_identity);
        const auto expression = override == local_expression_overrides_.end()
                                    ? sanitize_identifier(variable.name)
                                    : override->second;
        result += body + local_names + ".push_back(" + godot_string(variable.name) + ");\n" + body +
                  local_values + ".push_back(gdpp::runtime::to_variant(" + expression + "));\n";
    }
    result += body + "godot::PackedStringArray " + member_names + ";\n" + body + "godot::Array " +
              member_values + ";\n";
    if (current_debug_instance_ != "nullptr") {
        for (const auto& [name, expression] : current_debug_members_) {
            result += body + member_names + ".push_back(" + godot_string(name) + ");\n" + body +
                      member_values + ".push_back(gdpp::runtime::to_variant(" + expression +
                      "));\n";
        }
    }
    result += body + "gdpp::runtime::debug_breakpoint(" + godot_string(current_debug_source_) +
              ", " + godot_string_name(current_debug_function_) + ", " +
              std::to_string(statement.span.begin.line) + ", " + current_debug_instance_ + ", " +
              local_names + ", " + local_values + ", " + member_names + ", " + member_values +
              ");\n" + prefix + "}\n" + prefix + "#endif\n";
    return result;
}

std::string CodeGenerator::emit_statement(const typed::Statement& statement,
                                          const std::size_t indentation) const {
    auto result = emit_debug_line(statement.span.begin.line, indentation) +
                  emit_statement_body(statement, indentation);
    if (statement.kind == typed::StatementKind::assignment && assignment_may_fail(statement))
        result += emit_script_failure_return(indentation, in_async_continuation_);
    return result;
}

std::string CodeGenerator::emit_statement_body(const typed::Statement& statement,
                                               std::size_t indentation) const {
    const auto prefix = indent(indentation);
    switch (statement.kind) {
    case typed::StatementKind::expression:
        // GDScript permits intentionally ignoring any expression result. Make that intent
        // explicit so nodiscard Godot value types and ordinary conversion expressions remain
        // warning-clean under commercial -Wall/-Wextra builds.
        return prefix + "static_cast<void>(" + emit_expression(*statement.expression) + ");\n" +
               (expression_may_fail(*statement.expression)
                    ? emit_script_failure_return(indentation, in_async_continuation_)
                    : std::string{});
    case typed::StatementKind::return_statement:
        if (statement.expression) {
            const auto value_name = "_gdpp_return_value_" + std::to_string(temporary_counter_++);
            std::string value;
            if (current_coroutine_abi_ || in_callable_lambda_) {
                value =
                    current_return_type_.is_dynamic()
                        ? emit_expression(*statement.expression)
                        : emit_conversion(current_return_type_, emitted_type(*statement.expression),
                                          emit_expression(*statement.expression),
                                          &statement.expression->span);
            } else {
                value = emit_conversion(current_return_type_, emitted_type(*statement.expression),
                                        emit_expression(*statement.expression),
                                        &statement.expression->span);
            }
            const bool value_may_fail =
                expression_may_fail(*statement.expression) ||
                (!current_return_type_.is_dynamic() &&
                 conversion_may_fail(current_return_type_, emitted_type(*statement.expression)));
            std::string result = prefix + "const auto " + value_name + " = " + value + ";\n";
            if (value_may_fail)
                result += emit_script_failure_return(indentation, in_async_continuation_);
            if (current_coroutine_abi_)
                return result + coroutine_return(indentation, value_name, in_async_continuation_);
            if (in_callable_lambda_)
                return result + prefix + "return gdpp::runtime::to_variant(" + value_name + ");\n";
            if (in_async_continuation_)
                return result + prefix + "return;\n";
            return result + prefix + "return " + value_name + ";\n";
        }
        if (current_coroutine_abi_) {
            return coroutine_return(indentation, "godot::Variant{}", in_async_continuation_);
        }
        if (in_callable_lambda_) {
            return prefix + "return godot::Variant();\n";
        }
        if (in_async_continuation_)
            return prefix + "return;\n";
        if (!statement.expression && current_return_type_.kind != TypeKind::void_type)
            return prefix + "return {};\n";
        return prefix + "return;\n";
    case typed::StatementKind::await_statement:
        if (await_can_suspend(statement)) {
            diagnostics_.error("GDS3006", "nested await reached code generation", statement.span);
            return prefix + "/* invalid nested await */;\n";
        }
        return prefix + "static_cast<void>(" + emit_expression(*statement.expression) + ");\n" +
               (expression_may_fail(*statement.expression)
                    ? emit_script_failure_return(indentation, in_async_continuation_)
                    : std::string{});
    case typed::StatementKind::await_variable:
        if (await_can_suspend(statement)) {
            diagnostics_.error("GDS3006", "nested await reached code generation", statement.span);
            return prefix + "/* invalid nested await */;\n";
        }
        {
            std::string result =
                prefix + "[[maybe_unused]] " + (statement.is_constant ? "const " : "") +
                cpp_type(statement.declared_type) + " " + sanitize_identifier(statement.name) +
                " = " +
                emit_conversion(statement.declared_type, emitted_type(*statement.expression),
                                emit_expression(*statement.expression),
                                &statement.expression->span) +
                ";\n";
            if (expression_may_fail(*statement.expression) ||
                conversion_may_fail(statement.declared_type, emitted_type(*statement.expression))) {
                result += emit_script_failure_return(indentation, in_async_continuation_);
            }
            return result;
        }
    case typed::StatementKind::assert_statement: {
        std::string result = prefix + "#ifdef GDPP_SCRIPT_DEBUG_ENABLED\n";
        for (const auto& child : statement.assert_condition_prefix)
            result += emit_statement(child, indentation);
        const auto condition = "_gdpp_assert_condition_" + std::to_string(temporary_counter_++);
        result += prefix + "const bool " + condition + " = " + emit_truthy(*statement.condition) +
                  ";\n" + emit_script_failure_return(indentation, in_async_continuation_) + prefix +
                  "if (!" + condition + ") {\n";
        for (const auto& child : statement.assert_message_prefix)
            result += emit_statement(child, indentation + 1);
        result += emit_assert_failure(statement, indentation + 1, in_async_continuation_);
        result += prefix + "}\n";
        return result + prefix + "#endif\n";
    }
    case typed::StatementKind::variable: {
        std::string result =
            prefix + "[[maybe_unused]] " + (statement.is_constant ? "const " : "") +
            (statement.expression && statement.expression->kind == typed::ExpressionKind::lambda
                 ? std::string{"auto"}
                 : cpp_type(statement.declared_type)) +
            " " + sanitize_identifier(statement.name) +
            (statement.expression ? " = " + emit_conversion(statement.declared_type,
                                                            emitted_type(*statement.expression),
                                                            emit_expression(*statement.expression),
                                                            &statement.expression->span)
                                  : "{}") +
            ";\n";
        if (statement.expression &&
            (expression_may_fail(*statement.expression) ||
             conversion_may_fail(statement.declared_type, emitted_type(*statement.expression)))) {
            result += emit_script_failure_return(indentation, in_async_continuation_);
        }
        return result;
    }
    case typed::StatementKind::assignment: {
        const auto& target = *statement.condition;
        if (!lowering_assignment_) {
            const typed::Expression* receiver = nullptr;
            const typed::Expression* access = &target;
            while ((access->kind == typed::ExpressionKind::member ||
                    access->kind == typed::ExpressionKind::subscript) &&
                   !access->operands.empty()) {
                const auto* parent = access->operands.front().get();
                if ((parent->kind == typed::ExpressionKind::member ||
                     parent->kind == typed::ExpressionKind::subscript) &&
                    !parent->operands.empty()) {
                    access = parent;
                    continue;
                }
                receiver = parent;
                break;
            }

            const auto suffix = std::to_string(temporary_counter_++);
            const auto receiver_name = "_gdpp_assignment_receiver_" + suffix;
            const auto value_name = "_gdpp_assignment_value_" + suffix;
            const auto nested_prefix = indent(indentation + 1);
            std::string result = prefix + "{\n";
            const auto saved_overrides = exact_expression_overrides_;
            const bool receiver_is_type_namespace =
                receiver && (receiver->resolution == typed::ResolutionKind::godot_type ||
                             receiver->resolution == typed::ResolutionKind::external_type ||
                             receiver->resolution == typed::ResolutionKind::script_type ||
                             receiver->resolution == typed::ResolutionKind::inner_type ||
                             receiver->resolution == typed::ResolutionKind::script_enum_type ||
                             receiver->resolution == typed::ResolutionKind::global_enum_type);
            if (receiver && !receiver_is_type_namespace) {
                result += nested_prefix + "auto &&" + receiver_name + " = " +
                          emit_expression(*receiver) + ";\n";
                if (expression_may_fail(*receiver)) {
                    result += emit_script_failure_return(indentation + 1, in_async_continuation_);
                }
                exact_expression_overrides_[receiver] = receiver_name;
            }
            // The right-hand side is an owned, single-use snapshot. Keep it mutable so the
            // guarded commit below can transfer reference-backed Godot values without adding
            // reference-counted copies. Receiver-before-RHS evaluation order remains unchanged.
            result += nested_prefix + "auto " + value_name + " = " +
                      emit_expression(*statement.expression) + ";\n";
            if (expression_may_fail(*statement.expression)) {
                result += emit_script_failure_return(indentation + 1, in_async_continuation_);
            }
            exact_expression_overrides_[statement.expression.get()] = value_name;
            lowering_assignment_ = true;
            result += emit_statement_body(statement, indentation + 1);
            lowering_assignment_ = false;
            exact_expression_overrides_ = saved_overrides;
            return result + prefix + "}\n";
        }
        const auto checked_assignment_guard = [&](const std::string& receiver,
                                                  const std::string& member,
                                                  const std::size_t guard_indentation) {
            const auto location =
                current_source_path_ + ":" + std::to_string(target.span.begin.line);
            const auto message = godot_string("Cannot assign member '" + member +
                                              "' on a null or freed object at " + location);
            const auto failure =
                in_async_continuation_ || (!in_callable_lambda_ && !current_coroutine_abi_ &&
                                           current_return_type_.kind == TypeKind::void_type)
                    ? "ERR_FAIL_EDMSG(" + message + ");"
                    : "ERR_FAIL_V_EDMSG({}, " + message + ");";
            return indent(guard_indentation) +
                   "if (!gdpp::runtime::is_instance_valid(gdpp::runtime::to_variant(" + receiver +
                   "))) { " + failure + " }\n";
        };
        if (target.resolution == typed::ResolutionKind::dynamic_property &&
            target.kind == typed::ExpressionKind::member && !target.operands.empty() &&
            target.operands.at(0)->type.kind == TypeKind::dictionary) {
            return emit_dictionary_member_assignment(statement, indentation);
        }
        if (target.resolution == typed::ResolutionKind::dynamic_property)
            return emit_dynamic_assignment(statement, indentation);
        if (target.kind == typed::ExpressionKind::subscript &&
            (target.operands.at(0)->type.is_dynamic() ||
             target.operands.at(0)->type.kind == TypeKind::object ||
             (target.operands.at(0)->type.kind == TypeKind::builtin &&
              !target.operands.at(0)->type.is_packed_array()) ||
             target.operands.at(0)->type.kind == TypeKind::string)) {
            return emit_dynamic_assignment(statement, indentation);
        }
        if (target.kind == typed::ExpressionKind::subscript) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto container_name = "_gdpp_subscript_container_" + suffix;
            const auto index_name = "_gdpp_subscript_index_" + suffix;
            const auto value_name = "_gdpp_subscript_value_" + suffix;
            const auto nested_prefix = indent(indentation + 1);
            std::string result =
                prefix + "{\n" + nested_prefix + "auto &&" + container_name + " = " +
                emit_expression(*target.operands.at(0)) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "const auto " + index_name + " = " +
                emit_expression(*target.operands.at(1)) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_);
            std::string assigned;
            if (statement.operation != "=") {
                const auto current_name = "_gdpp_subscript_current_" + suffix;
                result += nested_prefix + "const auto " + current_name + " = " +
                          emit_subscript_read(target.operands.at(0)->type, target.type,
                                              container_name, index_name, target.span) +
                          ";\n" +
                          emit_script_failure_return(indentation + 1, in_async_continuation_);
                result += nested_prefix + "const auto " + value_name + " = " +
                          emit_expression(*statement.expression) + ";\n" +
                          emit_script_failure_return(indentation + 1, in_async_continuation_);
                const auto operation =
                    statement.operation.substr(0, statement.operation.size() - 1);
                bool static_runtime_result = false;
                if (target.type.is_dynamic() || statement.expression->type.is_dynamic()) {
                    assigned =
                        "gdpp::runtime::binary(godot::Variant::" + variant_operator(operation) +
                        ", " + current_name + ", " + value_name + ", " +
                        script_location(target.span) + ")";
                } else if ((target.type.kind == TypeKind::integer ||
                            target.type.kind == TypeKind::enumeration) &&
                           (statement.expression->type.kind == TypeKind::integer ||
                            statement.expression->type.kind == TypeKind::enumeration)) {
                    assigned = emit_integer_operation(operation, current_name, value_name,
                                                      target.type, target.span, false, false);
                } else if (has_direct_cpp_binary_operator(operation, target.type,
                                                          statement.expression->type)) {
                    assigned = "(" + current_name + " " + operation + " " + value_name + ")";
                } else {
                    assigned =
                        "gdpp::runtime::binary(godot::Variant::" + variant_operator(operation) +
                        ", " + current_name + ", " + value_name + ", " +
                        script_location(target.span) + ")";
                    static_runtime_result = true;
                }
                if (static_runtime_result)
                    assigned = emit_conversion(target.type, {TypeKind::variant, "Variant"},
                                               std::move(assigned), &target.span);
            } else {
                result += nested_prefix + "const auto " + value_name + " = " +
                          emit_expression(*statement.expression) + ";\n" +
                          emit_script_failure_return(indentation + 1, in_async_continuation_);
                assigned = value_name;
            }
            const auto assigned_source_type =
                statement.operation == "=" ? emitted_type(*statement.expression)
                : target.type.is_dynamic() || statement.expression->type.is_dynamic()
                    ? Type{TypeKind::variant, "Variant"}
                    : target.type;
            const auto& container_type = target.operands.at(0)->type;
            // Godot's typed Array/Dictionary subscript operators validate a dynamic Variant at
            // the container boundary. A failed Array set and a failed compound Dictionary set
            // leave the container unchanged and continue the script, while a failed direct
            // Dictionary set becomes a script error. Converting into native element storage
            // first would incorrectly turn all three cases into the same fatal failure.
            if (!assigned_source_type.is_dynamic() ||
                (container_type.kind != TypeKind::array &&
                 container_type.kind != TypeKind::dictionary)) {
                assigned = emit_conversion(target.type, assigned_source_type, std::move(assigned),
                                           &statement.expression->span);
            }
            assigned = emit_subscript_store(target.operands.at(0)->type, std::move(assigned));
            const auto final_name = "_gdpp_subscript_assigned_" + suffix;
            result += nested_prefix + "const auto " + final_name + " = " + assigned + ";\n";
            result += emit_script_failure_return(indentation + 1, in_async_continuation_);
            assigned = final_name;
            if (target.operands.at(0)->type.kind == TypeKind::array) {
                result += nested_prefix + "gdpp::runtime::checked_array_set(" + container_name +
                          ", " + index_name + ", gdpp::runtime::to_variant(" + assigned +
                          "), _gdpp_source_path, " + std::to_string(target.span.begin.line) + ", " +
                          std::to_string(target.span.begin.column) + ");\n";
            } else if (target.operands.at(0)->type.is_packed_array()) {
                result += nested_prefix + "gdpp::runtime::checked_packed_array_set(" +
                          container_name + ", " + index_name + ", " + assigned +
                          ", _gdpp_source_path, " + std::to_string(target.span.begin.line) + ", " +
                          std::to_string(target.span.begin.column) + ");\n";
            } else if (statement.operation == "=") {
                result += nested_prefix + "gdpp::runtime::checked_dictionary_set(" +
                          container_name + ", gdpp::runtime::to_variant(" + index_name +
                          "), gdpp::runtime::to_variant(" + assigned + "), " +
                          script_location(target.span) + ");\n";
            } else {
                result += nested_prefix + "gdpp::runtime::unchecked_dictionary_set(" +
                          container_name + ", gdpp::runtime::to_variant(" + index_name +
                          "), gdpp::runtime::to_variant(" + assigned + "), " +
                          script_location(target.span) + ");\n";
            }
            result += prefix + "}\n";
            return result;
        }
        const auto assignment_value = [&](std::string current, const Type& destination) {
            std::string value = emit_expression(*statement.expression);
            Type source = emitted_type(*statement.expression);
            if (statement.operation != "=") {
                const auto suffix = std::to_string(temporary_counter_++);
                const auto current_name = "_gdpp_property_current_" + suffix;
                const auto right_name = "_gdpp_property_right_" + suffix;
                const auto current_expression = std::move(current);
                const auto right_expression = std::move(value);
                const auto operation =
                    statement.operation.substr(0, statement.operation.size() - 1);
                const auto integer_target = target.type.kind == TypeKind::integer ||
                                            target.type.kind == TypeKind::enumeration;
                const auto integer_value = statement.expression->type.kind == TypeKind::integer ||
                                           statement.expression->type.kind == TypeKind::enumeration;
                if (integer_target && statement.expression->type.is_dynamic()) {
                    value = emit_conversion(target.type, statement.expression->type, right_name,
                                            &statement.expression->span);
                    value = emit_integer_operation(operation, current_name, std::move(value),
                                                   target.type, target.span, false, true);
                } else if (integer_target && integer_value) {
                    value = emit_integer_operation(operation, current_name, right_name, target.type,
                                                   target.span, false, false);
                } else if (!target.type.is_dynamic() && target.type.is_numeric() &&
                           statement.expression->type.is_dynamic()) {
                    value = emit_conversion(target.type, statement.expression->type, right_name,
                                            &statement.expression->span);
                    value = "(" + current_name + " " + operation + " " + value + ")";
                } else if (target.type.is_dynamic() || statement.expression->type.is_dynamic()) {
                    value = "gdpp::runtime::binary(godot::Variant::" + variant_operator(operation) +
                            ", " + current_name + ", " + right_name + ", " +
                            script_location(target.span) + ")";
                } else if (!has_direct_cpp_binary_operator(operation, target.type,
                                                           statement.expression->type)) {
                    value = "gdpp::runtime::binary(godot::Variant::" + variant_operator(operation) +
                            ", " + current_name + ", " + right_name + ", " +
                            script_location(target.span) + ")";
                } else {
                    value = "(" + current_name + " " + operation + " " + right_name + ")";
                }
                source = target.type.is_dynamic() || statement.expression->type.is_dynamic() ||
                                 !has_direct_cpp_binary_operator(operation, target.type,
                                                                 statement.expression->type)
                             ? Type{TypeKind::variant, "Variant"}
                             : target.type;
                value = emit_conversion(destination, source, std::move(value),
                                        &statement.expression->span);
                return "([&]() { const auto " + current_name + " = " + current_expression +
                       "; const auto " + right_name + " = " + right_expression + "; return " +
                       value + "; }())";
            }
            return emit_conversion(destination, source, std::move(value),
                                   &statement.expression->span);
        };
        const auto guarded_write = [&](std::string value, const auto& write,
                                       const std::size_t write_indentation) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto assigned = "_gdpp_assignment_result_" + suffix;
            const auto outer = indent(write_indentation);
            const auto inner = indent(write_indentation + 1);
            constexpr std::string_view snapshot_prefix{"_gdpp_assignment_value_"};
            const bool owned_snapshot =
                value.rfind(snapshot_prefix, 0) == 0 && value.size() > snapshot_prefix.size() &&
                std::all_of(value.begin() + static_cast<std::ptrdiff_t>(snapshot_prefix.size()),
                            value.end(), [](const char character) {
                                return std::isdigit(static_cast<unsigned char>(character)) != 0;
                            });
            const auto initialized =
                owned_snapshot ? "std::move(" + std::move(value) + ")" : std::move(value);
            return outer + "{\n" + inner + "auto " + assigned + " = " + initialized + ";\n" +
                   (assignment_may_fail(statement)
                        ? emit_script_failure_return(write_indentation + 1, in_async_continuation_)
                        : std::string{}) +
                   inner + write("std::move(" + assigned + ")") + ";\n" + outer + "}\n";
        };
        if (target.resolution == typed::ResolutionKind::script_runtime_static_field) {
            const auto* owner = script_symbols_
                                    ? script_symbols_->find_native_class(target.resolved_owner)
                                    : nullptr;
            const auto* member =
                owner ? script_symbols_->find_member(*owner, target.value) : nullptr;
            if (!member || member->kind != ScriptMemberKind::field || !member->is_static) {
                diagnostics_.error("GDS3013", "runtime static field metadata is unavailable",
                                   target.span);
                return prefix + "/* unavailable runtime static field */;\n";
            }
            auto value = assignment_value(emit_expression(target), member->type);
            return guarded_write(
                std::move(value),
                [&](const std::string& assigned) {
                    return "if (!gdpp::runtime::is_editor_hint()) " + target.resolved_owner +
                           "::" + target.setter + "(" + assigned + ")";
                },
                indentation);
        }
        if (target.resolution == typed::ResolutionKind::godot_property && target.direct_access &&
            target.kind == typed::ExpressionKind::member) {
            std::vector<const typed::Expression*> direct_chain;
            const typed::Expression* root = &target;
            while (root->resolution == typed::ResolutionKind::godot_property &&
                   root->direct_access && root->kind == typed::ExpressionKind::member &&
                   !root->operands.empty()) {
                direct_chain.push_back(root);
                root = root->operands.front().get();
            }
            if (root->resolution == typed::ResolutionKind::godot_property && !root->direct_access &&
                !root->getter.empty() && !root->setter.empty()) {
                const auto suffix = std::to_string(temporary_counter_++);
                const auto root_name = "_gdpp_property_value_" + suffix;
                const auto nested_prefix = indent(indentation + 1);
                std::string result = prefix + "{\n";
                std::string receiver;
                std::string connector;
                if (root->kind == typed::ExpressionKind::member) {
                    const auto receiver_name = "_gdpp_property_receiver_" + suffix;
                    const auto root_receiver =
                        root->operands.at(0)->type.kind == TypeKind::script_resource
                            ? "(" + emit_expression(*root->operands.at(0)) + ").resource()"
                            : emit_expression(*root->operands.at(0));
                    result +=
                        nested_prefix + "auto &&" + receiver_name + " = " + root_receiver + ";\n";
                    receiver = receiver_name;
                    connector = root->operands.at(0)->type.kind == TypeKind::object ||
                                        root->operands.at(0)->type.kind == TypeKind::script_resource
                                    ? "->"
                                    : ".";
                    if (root->operands.at(0)->type.kind == TypeKind::object ||
                        root->operands.at(0)->type.kind == TypeKind::script_resource) {
                        result +=
                            checked_assignment_guard(receiver_name, root->value, indentation + 1);
                    }
                } else if (attached_script_) {
                    receiver = godot_owner_expression();
                    connector = "->";
                }
                result += nested_prefix + cpp_type(root->type) + " " + root_name + " = " +
                          receiver + connector + root->getter + "(";
                std::string index;
                const auto* getter = api_.find_method(root->resolved_owner, root->getter);
                if (root->indexed_argument >= 0) {
                    index = std::to_string(root->indexed_argument);
                    if (getter) {
                        if (const auto* argument = api_.argument(*getter, 0))
                            index = emit_api_argument(argument->type, argument->meta,
                                                      {TypeKind::integer, "int"}, std::move(index));
                    }
                }
                auto assignment_object = root_name;
                for (std::size_t chain_index = direct_chain.size(); chain_index > 1;
                     --chain_index) {
                    const auto* member = direct_chain[chain_index - 1];
                    assignment_object = emit_direct_builtin_member(member->resolved_owner,
                                                                   std::move(assignment_object),
                                                                   member->value, &member->span);
                }
                auto current_value = root_name;
                for (std::size_t chain_index = direct_chain.size(); chain_index > 0;
                     --chain_index) {
                    const auto* member = direct_chain[chain_index - 1];
                    current_value =
                        emit_direct_builtin_member(member->resolved_owner, std::move(current_value),
                                                   member->value, &member->span);
                }
                auto value = assignment_value(std::move(current_value), target.assignment_type);
                const auto assigned_name = "_gdpp_property_assigned_" + suffix;
                result +=
                    index + ");\n" + nested_prefix + "const auto " + assigned_name + " = " + value +
                    ";\n" + emit_script_failure_return(indentation + 1, in_async_continuation_) +
                    nested_prefix +
                    emit_direct_builtin_assignment(
                        direct_chain.front()->resolved_owner, std::move(assignment_object),
                        direct_chain.front()->value, assigned_name, &direct_chain.front()->span) +
                    ";\n";
                const auto* setter = api_.find_method(root->resolved_owner, root->setter);
                std::string setter_index;
                if (root->indexed_argument >= 0) {
                    setter_index = std::to_string(root->indexed_argument);
                    if (setter) {
                        if (const auto* argument = api_.argument(*setter, 0))
                            setter_index = emit_api_argument(argument->type, argument->meta,
                                                             {TypeKind::integer, "int"},
                                                             std::move(setter_index));
                    }
                    setter_index += ", ";
                }
                auto root_value = root_name;
                if (setter) {
                    const auto value_index = root->indexed_argument >= 0 ? 1U : 0U;
                    if (const auto* argument = api_.argument(*setter, value_index)) {
                        root_value = emit_api_argument(argument->type, argument->meta, root->type,
                                                       std::move(root_value));
                    }
                }
                result += nested_prefix + receiver + connector + root->setter + "(" + setter_index +
                          root_value + ");\n" + prefix + "}\n";
                return result;
            }
            if (direct_chain.size() > 1) {
                // Builtin getters such as Transform2D::get_origin() may return a const reference
                // or a copy. Mutating the leaf directly is either invalid C++ or silently loses
                // the change. The transactional Variant lvalue path already preserves receiver
                // and RHS order, reconstructs every value layer, and commits the root exactly
                // once for locals, properties, subscripts, and object members.
                return emit_dynamic_assignment(statement, indentation);
            }
            const auto& parent = *target.operands.at(0);
            if (parent.type.kind == TypeKind::builtin) {
                const auto suffix = std::to_string(temporary_counter_++);
                const auto receiver = "_gdpp_builtin_receiver_" + suffix;
                const auto nested_prefix = indent(indentation + 1);
                auto value =
                    assignment_value(emit_direct_builtin_member(target.resolved_owner, receiver,
                                                                target.value, &target.span),
                                     target.assignment_type);
                const auto assigned = "_gdpp_builtin_assigned_" + suffix;
                return prefix + "{\n" + nested_prefix + "auto &&" + receiver + " = " +
                       emit_expression(parent) + ";\n" + nested_prefix + "const auto " + assigned +
                       " = " + value + ";\n" +
                       emit_script_failure_return(indentation + 1, in_async_continuation_) +
                       nested_prefix +
                       emit_direct_builtin_assignment(target.resolved_owner, receiver, target.value,
                                                      assigned, &target.span) +
                       ";\n" + prefix + "}\n";
            }
        }
        if (target.resolution == typed::ResolutionKind::script_property) {
            if (target.kind == typed::ExpressionKind::member) {
                const auto& owner = *target.operands.at(0);
                if (owner.type.kind == TypeKind::script_resource &&
                    !target.resolved_owner.empty()) {
                    const auto current = statement.operation == "="
                                             ? std::string{}
                                             : target.resolved_owner + "::" + target.getter + "()";
                    auto value = assignment_value(current, target.assignment_type);
                    return guarded_write(
                        std::move(value),
                        [&](const std::string& assigned) {
                            return target.resolved_owner + "::" + target.setter + "(" + assigned +
                                   ")";
                        },
                        indentation);
                }
                if (owner.resolution == typed::ResolutionKind::script_type ||
                    owner.resolution == typed::ResolutionKind::inner_type) {
                    const auto owner_type = owner.resolution == typed::ResolutionKind::script_type
                                                ? target.resolved_owner
                                                : emit_expression(owner);
                    const auto current = statement.operation == "="
                                             ? std::string{}
                                             : owner_type + "::" + target.getter + "()";
                    auto value = assignment_value(current, target.assignment_type);
                    return guarded_write(
                        std::move(value),
                        [&](const std::string& assigned) {
                            return owner_type + "::" + target.setter + "(" + assigned + ")";
                        },
                        indentation);
                }
                const bool explicit_self = attached_script_ &&
                                           owner.kind == typed::ExpressionKind::identifier &&
                                           owner.value == "self";
                if (explicit_self) {
                    const auto current =
                        statement.operation == "=" ? std::string{} : target.getter + "()";
                    auto value = assignment_value(current, target.assignment_type);
                    return guarded_write(
                        std::move(value),
                        [&](const std::string& assigned) {
                            return target.setter + "(" + assigned + ")";
                        },
                        indentation);
                }
                if (!attached_script_source_path(owner.type, target.resolved_owner).empty()) {
                    const auto suffix = std::to_string(temporary_counter_++);
                    const auto object = "_gdpp_attached_property_target_" + suffix;
                    const auto nested_prefix = indent(indentation + 1);
                    const auto current =
                        statement.operation == "="
                            ? std::string{}
                            : emit_conversion(target.type, {TypeKind::variant, "Variant"},
                                              "gdpp::runtime::get_named(" + object + ", " +
                                                  godot_string_name(target.value) + ", " +
                                                  script_location(target.span) + ")",
                                              &target.span);
                    auto value = assignment_value(current, target.assignment_type);
                    const auto assigned = "_gdpp_attached_property_assigned_" + suffix;
                    return prefix + "{\n" + nested_prefix + "godot::Variant " + object +
                           " = gdpp::runtime::to_variant(" + emit_expression(owner) + ");\n" +
                           emit_script_failure_return(indentation + 1, in_async_continuation_) +
                           nested_prefix + "const auto " + assigned + " = " + value + ";\n" +
                           emit_script_failure_return(indentation + 1, in_async_continuation_) +
                           nested_prefix + "gdpp::runtime::set_named(" + object + ", " +
                           godot_string_name(target.value) + ", gdpp::runtime::to_variant(" +
                           assigned + "), " + script_location(target.span) + ");\n" + prefix +
                           "}\n";
                }
                const auto object = emit_expression(owner);
                const auto connector = owner.type.kind == TypeKind::object ? "->" : ".";
                const auto suffix = std::to_string(temporary_counter_++);
                const auto receiver = "_gdpp_property_receiver_" + suffix;
                const auto nested_prefix = indent(indentation + 1);
                const auto current = statement.operation == "="
                                         ? std::string{}
                                         : receiver + connector + target.getter + "()";
                auto value = assignment_value(current, target.assignment_type);
                std::string result =
                    prefix + "{\n" + nested_prefix + "auto &&" + receiver + " = " + object + ";\n";
                if (owner.type.kind == TypeKind::object)
                    result += checked_assignment_guard(receiver, target.value, indentation + 1);
                const auto assigned = "_gdpp_property_assigned_" + suffix;
                result += nested_prefix + "const auto " + assigned + " = " + value + ";\n" +
                          emit_script_failure_return(indentation + 1, in_async_continuation_) +
                          nested_prefix + receiver + connector + target.setter + "(" + assigned +
                          ");\n" + prefix + "}\n";
                return result;
            }
            const auto current = statement.operation == "=" ? std::string{} : target.getter + "()";
            auto value = assignment_value(current, target.assignment_type);
            return guarded_write(
                std::move(value),
                [&](const std::string& assigned) { return target.setter + "(" + assigned + ")"; },
                indentation);
        }
        if (target.resolution == typed::ResolutionKind::godot_property && !target.direct_access) {
            if (target.setter.empty()) {
                diagnostics_.error("GDS3003", "Godot property is read-only", target.span);
                return prefix + "/* read-only property assignment */;\n";
            }
            if (target.kind == typed::ExpressionKind::identifier) {
                std::string raw_index;
                if (target.indexed_argument >= 0)
                    raw_index = std::to_string(target.indexed_argument);
                auto getter_index = raw_index;
                const auto* setter = api_.find_method(target.resolved_owner, target.setter);
                if (!getter_index.empty()) {
                    if (const auto* getter =
                            api_.find_method(target.resolved_owner, target.getter)) {
                        if (const auto* argument = api_.argument(*getter, 0)) {
                            getter_index = emit_api_argument(argument->type, argument->meta,
                                                             {TypeKind::integer, "int"},
                                                             std::move(getter_index));
                        }
                    }
                }
                const auto receiver =
                    attached_script_ ? godot_owner_expression() + "->" : std::string{};
                const auto current =
                    statement.operation == "="
                        ? std::string{}
                        : emit_api_return(target.type,
                                          receiver + target.getter + "(" + getter_index + ")");
                auto value = assignment_value(current, target.assignment_type);
                auto setter_index = raw_index;
                if (!setter_index.empty()) {
                    if (setter) {
                        if (const auto* argument = api_.argument(*setter, 0)) {
                            setter_index = emit_api_argument(argument->type, argument->meta,
                                                             {TypeKind::integer, "int"},
                                                             std::move(setter_index));
                        }
                    }
                    setter_index += ", ";
                }
                if (setter) {
                    const auto value_index = target.indexed_argument >= 0 ? 1U : 0U;
                    if (const auto* argument = api_.argument(*setter, value_index)) {
                        value = emit_api_argument(argument->type, argument->meta,
                                                  target.assignment_type, std::move(value));
                    }
                }
                return guarded_write(
                    std::move(value),
                    [&](const std::string& assigned) {
                        return receiver + target.setter + "(" + setter_index + assigned + ")";
                    },
                    indentation);
            }
            if (target.kind == typed::ExpressionKind::member) {
                const auto object =
                    target.operands.at(0)->type.kind == TypeKind::script_resource
                        ? "(" + emit_expression(*target.operands.at(0)) + ").resource()"
                        : emit_expression(*target.operands.at(0));
                const bool checked_object =
                    (target.operands.at(0)->type.kind == TypeKind::object ||
                     target.operands.at(0)->type.kind == TypeKind::script_resource) &&
                    target.operands.at(0)->resolution != typed::ResolutionKind::script_type;
                const auto suffix = std::to_string(temporary_counter_++);
                const auto receiver = checked_object ? "_gdpp_property_receiver_" + suffix : object;
                const auto connector =
                    target.operands.at(0)->resolution == typed::ResolutionKind::script_type ? "::"
                    : receiver == "this" || target.operands.at(0)->type.kind == TypeKind::object ||
                            target.operands.at(0)->type.kind == TypeKind::script_resource
                        ? "->"
                        : ".";
                std::string raw_index;
                if (target.indexed_argument >= 0)
                    raw_index = std::to_string(target.indexed_argument);
                auto getter_index = raw_index;
                const auto* setter = api_.find_method(target.resolved_owner, target.setter);
                if (!getter_index.empty()) {
                    if (const auto* getter =
                            api_.find_method(target.resolved_owner, target.getter)) {
                        if (const auto* argument = api_.argument(*getter, 0)) {
                            getter_index = emit_api_argument(argument->type, argument->meta,
                                                             {TypeKind::integer, "int"},
                                                             std::move(getter_index));
                        }
                    }
                }
                const auto current =
                    statement.operation == "="
                        ? std::string{}
                        : emit_api_return(target.type, receiver + connector + target.getter + "(" +
                                                           getter_index + ")");
                auto value = assignment_value(current, target.assignment_type);
                auto setter_index = raw_index;
                if (!setter_index.empty()) {
                    if (setter) {
                        if (const auto* argument = api_.argument(*setter, 0)) {
                            setter_index = emit_api_argument(argument->type, argument->meta,
                                                             {TypeKind::integer, "int"},
                                                             std::move(setter_index));
                        }
                    }
                    setter_index += ", ";
                }
                if (setter) {
                    const auto value_index = target.indexed_argument >= 0 ? 1U : 0U;
                    if (const auto* argument = api_.argument(*setter, value_index)) {
                        value = emit_api_argument(argument->type, argument->meta,
                                                  target.assignment_type, std::move(value));
                    }
                }
                if (!checked_object) {
                    return guarded_write(
                        std::move(value),
                        [&](const std::string& assigned) {
                            return receiver + connector + target.setter + "(" + setter_index +
                                   assigned + ")";
                        },
                        indentation);
                }
                const auto nested_prefix = indent(indentation + 1);
                const auto assigned = "_gdpp_property_assigned_" + suffix;
                return prefix + "{\n" + nested_prefix + "auto &&" + receiver + " = " + object +
                       ";\n" + checked_assignment_guard(receiver, target.value, indentation + 1) +
                       nested_prefix + "const auto " + assigned + " = " + value + ";\n" +
                       emit_script_failure_return(indentation + 1, in_async_continuation_) +
                       nested_prefix + receiver + connector + target.setter + "(" + setter_index +
                       assigned + ");\n" + prefix + "}\n";
            }
        }
        auto value = assignment_value(emit_expression(target), target.assignment_type);
        return guarded_write(
            std::move(value),
            [&](const std::string& assigned) {
                return emit_storage_assignment(target.type, emit_expression(target), assigned);
            },
            indentation);
    }
    case typed::StatementKind::if_statement: {
        const auto condition = "_gdpp_if_condition_" + std::to_string(temporary_counter_++);
        std::string result = prefix + "const bool " + condition + " = " +
                             emit_truthy(*statement.condition) + ";\n" +
                             emit_script_failure_return(indentation, in_async_continuation_) +
                             prefix + "if (" + condition + ") {\n";
        for (const auto& child : statement.body)
            result += emit_statement(child, indentation + 1);
        result += prefix + "}";
        if (!statement.else_body.empty()) {
            result += " else {\n";
            for (const auto& child : statement.else_body) {
                result += emit_statement(child, indentation + 1);
            }
            result += prefix + "}";
        }
        return result + "\n";
    }
    case typed::StatementKind::match_statement: {
        const auto identity = match_counter_++;
        const auto value_name = "_gdpp_match_value_" + std::to_string(identity);
        const auto matched_name = "_gdpp_match_done_" + std::to_string(identity);
        std::string result = prefix + "{\n" + indent(indentation + 1) + "const auto " + value_name +
                             " = " + emit_expression(*statement.condition) + ";\n" +
                             emit_script_failure_return(indentation + 1, in_async_continuation_) +
                             indent(indentation + 1) + "bool " + matched_name + " = false;\n";
        for (const auto& branch : statement.body) {
            std::string condition;
            std::vector<MatchBinding> bindings;
            for (const auto& pattern : branch.patterns) {
                collect_match_bindings(pattern, bindings);
                if (!condition.empty())
                    condition += " || ";
                condition += "(" + emit_match_pattern(pattern, value_name, bindings) + ")";
            }
            const auto branch_prefix = indent(indentation + 1);
            for (const auto& binding : bindings)
                result += branch_prefix + "godot::Variant " + binding.slot + ";\n";
            result += branch_prefix + "if (!" + matched_name;
            if (!condition.empty())
                result += " && (" + condition + ")";
            result += ") {\n";
            const auto content_indent = indentation + 2;
            for (const auto& binding : bindings) {
                result += indent(content_indent) + "[[maybe_unused]] " + cpp_type(binding.type) +
                          " " + sanitize_identifier(binding.name) + " = " +
                          emit_conversion(binding.type, {TypeKind::variant, "Variant"},
                                          binding.slot, &branch.span) +
                          ";\n";
            }
            for (const auto& guard_statement : branch.guard_prefix)
                result += emit_statement(guard_statement, content_indent);
            if (branch.expression) {
                const auto guard = emit_truthy(*branch.expression);
                result += indent(content_indent) + "if (" + guard + ") {\n" +
                          indent(content_indent + 1) + matched_name + " = true;\n";
                for (const auto& child : branch.body)
                    result += emit_statement(child, content_indent + 1);
                result += indent(content_indent) + "}\n";
            } else {
                result += indent(content_indent) + matched_name + " = true;\n";
                for (const auto& child : branch.body)
                    result += emit_statement(child, content_indent);
            }
            result += branch_prefix + "}\n";
        }
        return result + prefix + "}\n";
    }
    case typed::StatementKind::match_branch:
        diagnostics_.error("GDS3004", "orphan match branch reached code generation",
                           statement.span);
        return prefix + "/* invalid match branch */;\n";
    case typed::StatementKind::while_statement: {
        const auto condition = "_gdpp_while_condition_" + std::to_string(temporary_counter_++);
        std::string result = prefix + "while (true) {\n" + indent(indentation + 1) + "const bool " +
                             condition + " = " + emit_truthy(*statement.condition) + ";\n" +
                             emit_script_failure_return(indentation + 1, in_async_continuation_) +
                             indent(indentation + 1) + "if (!" + condition + ") break;\n";
        for (const auto& child : statement.body)
            result += emit_statement(child, indentation + 1);
        return result + prefix + "}\n";
    }
    case typed::StatementKind::for_statement: {
        if (statement.iteration.strategy == IterationStrategy::intrinsic_range) {
            const auto* direct_range = range_call(*statement.condition);
            if (!direct_range) {
                diagnostics_.error("GDS3011", "range iteration plan lost its intrinsic call",
                                   statement.span);
                return prefix + "/* invalid range iteration plan */;\n";
            }
            const auto suffix = std::to_string(temporary_counter_++);
            const auto start = "_gdpp_range_start_" + suffix;
            const auto stop = "_gdpp_range_stop_" + suffix;
            const auto step = "_gdpp_range_step_" + suffix;
            const auto value = "_gdpp_range_value_" + suffix;
            const auto argument_count = direct_range->operands.size() - 1;
            const auto integer_argument = [&](const std::size_t index) {
                const auto& argument = *direct_range->operands.at(index);
                return emit_conversion({TypeKind::integer, "int"}, emitted_type(argument),
                                       emit_expression(argument), &argument.span);
            };
            const auto start_value = argument_count == 1 ? std::string{"0"} : integer_argument(1);
            const auto stop_value = argument_count == 1 ? integer_argument(1) : integer_argument(2);
            const auto step_value = argument_count == 3 ? integer_argument(3) : std::string{"1"};
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "const int64_t " + start + " = " + start_value +
                ";\n" + emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "const int64_t " + stop + " = " + stop_value + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "const int64_t " + step + " = " + step_value + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "if (" + step + " == 0) {\n" + indent(indentation + 2) +
                "gdpp::runtime::report_script_failure(\"Error calling GDScript utility function "
                "\\\"range()\\\": Step argument is zero!\", _gdpp_source_path, " +
                std::to_string(statement.span.begin.line) + ", " +
                std::to_string(statement.span.begin.column) + ");\n" +
                emit_script_failure_return(indentation + 2, in_async_continuation_) +
                nested_prefix + "}\n" + nested_prefix + "for (int64_t " + value + " = " + start +
                "; " + step + " != 0 && (" + step + " > 0 ? " + value + " < " + stop + " : " +
                value + " > " + stop + "); " + value + " = gdpp::integer::range_advance(" + value +
                ", " + step + ", " + stop + ")) {\n" + body_prefix + "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, {TypeKind::integer, "int"}, value,
                                &statement.span) +
                ";\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::integer_count) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto limit = "_gdpp_integer_limit_" + suffix;
            std::string result =
                prefix + "{\n" + indent(indentation + 1) + "const int64_t " + limit + " = " +
                emit_expression(*statement.condition) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                indent(indentation + 1) + "for (" +
                (statement.declared_type.is_dynamic() ? std::string{"int64_t"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = 0; " +
                sanitize_identifier(statement.name) + " < " + limit + "; ++" +
                sanitize_identifier(statement.name) + ") {\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + indent(indentation + 1) + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::floating_count) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto limit = "_gdpp_float_limit_" + suffix;
            const auto value = "_gdpp_float_value_" + suffix;
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "const double " + limit + " = " +
                emit_expression(*statement.condition) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "for (double " + value + " = 0.0; " + value + " < " + limit + "; " +
                value + " += 1.0) {\n" + body_prefix + "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, {TypeKind::floating, "float"}, value,
                                &statement.span) +
                ";\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::vector2_range ||
            statement.iteration.strategy == IterationStrategy::vector2i_range) {
            const bool integral = statement.iteration.strategy == IterationStrategy::vector2i_range;
            const auto suffix = std::to_string(temporary_counter_++);
            const auto bounds = "_gdpp_vector2_bounds_" + suffix;
            const auto value = "_gdpp_vector2_value_" + suffix;
            const auto scalar = integral ? std::string{"int64_t"} : std::string{"double"};
            const auto source_type =
                integral ? Type{TypeKind::integer, "int"} : Type{TypeKind::floating, "float"};
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "const auto " + bounds + " = " +
                emit_expression(*statement.condition) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "for (" + scalar + " " + value + " = " + bounds + ".x; " + value +
                " < " + bounds + ".y; ++" + value + ") {\n" + body_prefix + "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, source_type, value, &statement.span) +
                ";\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::vector3_range ||
            statement.iteration.strategy == IterationStrategy::vector3i_range) {
            const bool integral = statement.iteration.strategy == IterationStrategy::vector3i_range;
            const auto suffix = std::to_string(temporary_counter_++);
            const auto bounds = "_gdpp_vector3_bounds_" + suffix;
            const auto value = "_gdpp_vector3_value_" + suffix;
            const auto stop = "_gdpp_vector3_stop_" + suffix;
            const auto step = "_gdpp_vector3_step_" + suffix;
            const auto scalar = integral ? std::string{"int64_t"} : std::string{"double"};
            const auto zero = integral ? std::string{"0"} : std::string{"0.0"};
            const auto source_type =
                integral ? Type{TypeKind::integer, "int"} : Type{TypeKind::floating, "float"};
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "const auto " + bounds + " = " +
                emit_expression(*statement.condition) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "const " + scalar + " " + stop + " = " + bounds + ".y;\n" +
                nested_prefix + "const " + scalar + " " + step + " = " + bounds + ".z;\n" +
                nested_prefix + "for (" + scalar + " " + value + " = " + bounds + ".x; " + step +
                " != " + zero + " && (" + step + " > " + zero + " ? " + value + " < " + stop +
                " : " + value + " > " + stop + "); " + value +
                (integral
                     ? " = gdpp::integer::range_advance(" + value + ", " + step + ", " + stop + ")"
                     : " += " + step) +
                ") {\n" + body_prefix + "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, source_type, value, &statement.span) +
                ";\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::indexed_packed_array) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto iterable_name = "_gdpp_packed_iterable_" + suffix;
            const auto index_name = "_gdpp_packed_index_" + suffix;
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "auto &&" + iterable_name + " = " +
                emit_expression(*statement.condition) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "for (int64_t " + index_name + " = 0; " + index_name + " < " +
                iterable_name + ".native().size(); ++" + index_name + ") {\n" + body_prefix +
                "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, statement.iteration.element_type,
                                iterable_name + ".native()[" + index_name + "]", &statement.span) +
                ";\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::indexed_string) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto iterable_name = "_gdpp_string_iterable_" + suffix;
            const auto index_name = "_gdpp_string_index_" + suffix;
            const auto size_name = "_gdpp_string_size_" + suffix;
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "const godot::String " + iterable_name + " = " +
                emit_expression(*statement.condition) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "const int64_t " + size_name + " = " + iterable_name +
                ".length();\n" + nested_prefix + "for (int64_t " + index_name + " = 0; " +
                index_name + " < " + size_name + "; ++" + index_name + ") {\n" + body_prefix +
                "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, statement.iteration.element_type,
                                iterable_name + ".substr(" + index_name + ", 1)", &statement.span) +
                ";\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::indexed_array) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto iterable_name = "_gdpp_array_iterable_" + suffix;
            const auto index_name = "_gdpp_array_index_" + suffix;
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "auto &&" + iterable_name + " = " +
                emit_expression(*statement.condition) + ";\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "for (int64_t " + index_name + " = 0; " + index_name + " < " +
                iterable_name + ".size(); ++" + index_name + ") {\n" + body_prefix +
                "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, {TypeKind::variant, "Variant"},
                                iterable_name + "[" + index_name + "]", &statement.span) +
                ";\n";
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" + prefix + "}\n";
        }
        if (statement.iteration.strategy == IterationStrategy::dynamic_protocol ||
            statement.iteration.strategy == IterationStrategy::dictionary_protocol ||
            statement.iteration.strategy == IterationStrategy::object_protocol) {
            const auto suffix = std::to_string(temporary_counter_++);
            const auto iterable_name = "_gdpp_dynamic_iterable_" + suffix;
            const auto iterator_name = "_gdpp_dynamic_iterator_" + suffix;
            const auto available_name = "_gdpp_dynamic_available_" + suffix;
            const auto nested_prefix = indent(indentation + 1);
            const auto body_prefix = indent(indentation + 2);
            std::string result =
                prefix + "{\n" + nested_prefix + "const godot::Variant " + iterable_name + " = " +
                "gdpp::runtime::to_variant(" + emit_expression(*statement.condition) + ");\n" +
                emit_script_failure_return(indentation + 1, in_async_continuation_) +
                nested_prefix + "godot::Variant " + iterator_name + ";\n" + nested_prefix +
                "for (bool " + available_name + " = gdpp::runtime::iter_init(" + iterable_name +
                ", " + iterator_name + ", " + script_location(statement.condition->span) + "); " +
                available_name + "; " + available_name + " = gdpp::runtime::iter_next(" +
                iterable_name + ", " + iterator_name + ", " +
                script_location(statement.condition->span) + ")) {\n" + body_prefix +
                "[[maybe_unused]] " +
                (statement.declared_type.is_dynamic() ? std::string{"godot::Variant"}
                                                      : cpp_type(statement.declared_type)) +
                " " + sanitize_identifier(statement.name) + " = " +
                emit_conversion(statement.declared_type, {TypeKind::variant, "Variant"},
                                "gdpp::runtime::iter_get(" + iterable_name + ", " + iterator_name +
                                    ", " + script_location(statement.condition->span) + ")",
                                &statement.span) +
                ";\n" + emit_script_failure_return(indentation + 2, in_async_continuation_);
            for (const auto& child : statement.body)
                result += emit_statement(child, indentation + 2);
            return result + nested_prefix + "}\n" +
                   emit_script_failure_return(indentation + 1, in_async_continuation_) + prefix +
                   "}\n";
        }
        diagnostics_.error("GDS3005",
                           "unsupported statically typed iterable reached code generation",
                           statement.span);
        return prefix + "/* unsupported iterable */;\n";
    }
    case typed::StatementKind::pass_statement:
        return prefix + ";\n";
    case typed::StatementKind::break_statement:
        return prefix + "break;\n";
    case typed::StatementKind::continue_statement:
        return prefix + "continue;\n";
    case typed::StatementKind::breakpoint_statement:
        return emit_debug_breakpoint(statement, indentation);
    }
    return {};
}

void CodeGenerator::emit_attached_descriptor_definition(
    std::ostringstream& source, const std::string& native_name, const std::string& source_path,
    const std::string& global_name, const std::string& native_base_type,
    const std::string& base_script_path, const std::string& contract_hash, const bool tool_mode,
    const bool is_abstract, const std::vector<typed::Field>& fields,
    const std::vector<typed::Function>& functions, const std::vector<typed::Signal>& signals,
    const std::vector<typed::Enum>& enums, const std::vector<typed::Class>& classes) const {
    source << "gdpp::runtime::AttachedScriptDescriptor " << native_name
           << "::_gdpp_descriptor() {\n"
           << "    gdpp::runtime::AttachedScriptDescriptor descriptor;\n"
           << "    descriptor.source_path = " << godot_string(source_path) << ";\n"
           << "    descriptor.global_name = " << godot_string_name(global_name) << ";\n"
           << "    descriptor.native_base_type = " << godot_string_name(native_base_type) << ";\n"
           << "    descriptor.contract_hash = " << godot_string(contract_hash) << ";\n"
           << "    descriptor.base_script_path = " << godot_string(base_script_path) << ";\n"
           << "    descriptor.behavior_class = " << godot_string_name(native_name) << ";\n"
           << "    descriptor.tool = " << (tool_mode ? "true" : "false") << ";\n"
           << "    descriptor.abstract = " << (is_abstract ? "true" : "false") << ";\n";
    if (!is_abstract)
        source << "    descriptor.factory = []() -> "
                  "godot::Ref<gdpp::runtime::AttachedScriptBehavior> { return memnew("
               << native_name << "); };\n";
    for (const auto& variable : fields) {
        if (!is_bound_property(variable))
            continue;
        source << "    {\n"
               << "        gdpp::runtime::AttachedScriptProperty property;\n"
               << "        property.info = "
               << property_info(variable, api_, script_symbols_, cpp_type(variable.type)) << ";\n";
        const auto name = sanitize_identifier(variable.name);
        source << "        property.getter = [](gdpp::runtime::AttachedScriptBehavior *behavior) "
                  "-> godot::Variant {\n"
               << "            if (!behavior || !behavior->is_class(" << native_name
               << "::get_class_static())) return {};\n"
               << "            auto *typed = static_cast<" << native_name << " *>(behavior);\n"
               << "            return gdpp::runtime::to_variant(typed->_gdpp_get_" << name
               << "());\n"
               << "        };\n"
               << "        property.setter = [](gdpp::runtime::AttachedScriptBehavior *behavior, "
                  "const godot::Variant &value) -> bool {\n"
               << "            if (!behavior || !behavior->is_class(" << native_name
               << "::get_class_static())) return false;\n"
               << "            gdpp::runtime::ScriptFunctionScope storage_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
               << "            auto *typed = static_cast<" << native_name << " *>(behavior);\n"
               << "            const auto converted = "
               << emit_conversion(variable.type, {TypeKind::variant, "Variant"}, "value",
                                  &variable.span)
               << ";\n"
               << "            if (storage_scope.failed()) return false;\n"
               << "            typed->_gdpp_set_" << name << "(converted);\n"
               << "            return !storage_scope.failed();\n"
               << "        };\n"
               << "        property.storage_getter = []("
                  "gdpp::runtime::AttachedScriptBehavior *behavior) -> godot::Variant {\n"
               << "            if (!behavior || !behavior->is_class(" << native_name
               << "::get_class_static())) return {};\n"
               << "            auto *typed = static_cast<" << native_name << " *>(behavior);\n"
               << "            return gdpp::runtime::to_variant(typed->" << name << ");\n"
               << "        };\n"
               << "        property.storage_setter = []("
                  "gdpp::runtime::AttachedScriptBehavior *behavior, "
                  "const godot::Variant &value) -> bool {\n"
               << "            if (!behavior || !behavior->is_class(" << native_name
               << "::get_class_static())) return false;\n"
               << "            gdpp::runtime::ScriptFunctionScope storage_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
               << "            auto *typed = static_cast<" << native_name << " *>(behavior);\n"
               << "            const auto converted = "
               << emit_conversion(variable.type, {TypeKind::variant, "Variant"}, "value",
                                  &variable.span)
               << ";\n"
               << "            if (storage_scope.failed()) return false;\n"
               << "            "
               << emit_storage_assignment(variable.type, "typed->" + name, "converted") << ";\n"
               << "            return !storage_scope.failed();\n"
               << "        };\n";
        if (!variable.initializer || editor_safe_initializer(*variable.initializer)) {
            source << "        property.has_default = true;\n"
                   << "        property.default_value = gdpp::runtime::to_variant(";
            if (variable.initializer) {
                source << emit_conversion(variable.type, emitted_type(*variable.initializer),
                                          emit_expression(*variable.initializer),
                                          &variable.initializer->span);
            } else {
                const auto native_type = cpp_type(variable.type);
                source << (!native_type.empty() && native_type.back() == '*'
                               ? "static_cast<" + native_type + ">(nullptr)"
                               : native_type + "{}");
            }
            source << ");\n";
        }
        source << "        descriptor.properties.push_back(std::move(property));\n"
               << "    }\n";
    }
    for (const auto& variable : fields) {
        if (variable.is_constant || !variable.is_static)
            continue;
        const auto name = sanitize_identifier(variable.name);
        source << "    {\n"
               << "        gdpp::runtime::AttachedScriptStaticProperty property;\n"
               << "        property.name = " << godot_string_name(variable.name) << ";\n"
               << "        property.getter = []() -> godot::Variant {\n"
               << "            gdpp::runtime::ScriptFunctionScope storage_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
               << "            const auto value = " << native_name << "::_gdpp_get_" << name
               << "();\n"
               << "            if (storage_scope.failed()) return {};\n"
               << "            return gdpp::runtime::to_variant(value);\n"
               << "        };\n"
               << "        property.setter = [](const godot::Variant &value) -> bool {\n"
               << "            gdpp::runtime::ScriptFunctionScope storage_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
               << "            const auto converted = "
               << emit_conversion(variable.type, {TypeKind::variant, "Variant"}, "value",
                                  &variable.span)
               << ";\n"
               << "            if (storage_scope.failed()) return false;\n"
               << "            " << native_name << "::_gdpp_set_" << name << "(converted);\n"
               << "            return !storage_scope.failed();\n"
               << "        };\n"
               << "        descriptor.static_properties.push_back(std::move(property));\n"
               << "    }\n";
    }
    for (const auto& function : functions) {
        if (function.name == "_static_init")
            continue;
        source << "    {\n"
               << "        godot::MethodInfo method("
               << native_property_info(function.return_type, "") << ", "
               << godot_string_name(function.name);
        for (const auto& parameter : function.parameters)
            source << ", " << native_property_info(parameter.type, parameter.name);
        source << ");\n";
        if (function.is_static)
            source << "        method.flags |= GDEXTENSION_METHOD_FLAG_STATIC;\n";
        if (function.rest_parameter)
            source << "        method.flags |= GDEXTENSION_METHOD_FLAG_VARARG;\n";
        for (const auto& parameter : function.parameters) {
            if (parameter.default_value)
                source << "        method.default_arguments.push_back("
                          "gdpp::runtime::default_argument());\n";
        }
        source << "        descriptor.methods.push_back(std::move(method));\n"
               << "        descriptor.method_dispatches.push_back({"
               << godot_string_name(function.name) << ", &" << native_name
               << "::" << method_callback_name(function) << "});\n"
               << "    }\n";
    }
    for (const auto& signal : signals) {
        source << "    descriptor.signals.emplace_back(" << godot_string_name(signal.name);
        for (const auto& parameter : signal.parameters)
            source << ", " << native_property_info(parameter.type, parameter.name);
        source << ");\n";
    }
    for (const auto& variable : fields) {
        if (!variable.is_constant)
            continue;
        const auto name = sanitize_identifier(variable.name);
        source << "    descriptor.deferred_constants.push_back({"
               << godot_string_name(variable.name)
               << ", []() -> godot::Variant { return gdpp::runtime::to_variant("
               << (managed_constant_field(variable) ? name + "()" : name) << "); }});\n";
    }
    for (const auto& enumeration : enums) {
        if (enumeration.name.empty()) {
            for (const auto& entry : enumeration.entries) {
                source << "    descriptor.constants[" << godot_string_name(entry.name)
                       << "] = int64_t{" << enum_identifier(entry.name) << "};\n";
            }
            continue;
        }
        source << "    {\n"
               << "        godot::Dictionary values;\n";
        for (const auto& entry : enumeration.entries) {
            source << "        values[" << godot_string(entry.name) << "] = int64_t{"
                   << sanitize_identifier(enumeration.name) << "::"
                   << enum_identifier(entry.name) << "};\n";
        }
        source << "        values.make_read_only();\n"
               << "        descriptor.constants[" << godot_string_name(enumeration.name)
               << "] = values;\n"
               << "    }\n";
    }
    for (const auto& inner_class : classes) {
        const auto inner_source_path =
            source_path + (source_path.find("::") == std::string::npos ? "::" : ".") +
            inner_class.name;
        source << "    descriptor.deferred_constants.push_back({"
               << godot_string_name(inner_class.name)
               << ", []() -> godot::Variant { return gdpp::runtime::to_variant("
                  "gdpp::runtime::attached_script_resource("
               << godot_string(inner_source_path) << ")); }});\n";
    }
    if (has_rpc_configuration(functions)) {
        source << "    {\n        godot::Dictionary rpc;\n";
        for (const auto& function : functions) {
            if (!function.rpc)
                continue;
            const auto& rpc = *function.rpc;
            source << "        {\n            godot::Dictionary config;\n"
                   << "            config[\"rpc_mode\"] = godot::MultiplayerAPI::"
                   << (rpc.permission == RpcPermission::any_peer ? "RPC_MODE_ANY_PEER"
                                                                 : "RPC_MODE_AUTHORITY")
                   << ";\n";
            if (rpc.transfer_mode_explicit) {
                source << "            config[\"transfer_mode\"] = "
                          "godot::MultiplayerPeer::";
                switch (rpc.transfer_mode) {
                case RpcTransferMode::unreliable:
                    source << "TRANSFER_MODE_UNRELIABLE";
                    break;
                case RpcTransferMode::unreliable_ordered:
                    source << "TRANSFER_MODE_UNRELIABLE_ORDERED";
                    break;
                case RpcTransferMode::reliable:
                    source << "TRANSFER_MODE_RELIABLE";
                    break;
                }
                source << ";\n";
            }
            if (rpc.call_local_explicit)
                source << "            config[\"call_local\"] = "
                       << (rpc.call_local ? "true" : "false") << ";\n";
            if (rpc.channel_explicit)
                source << "            config[\"channel\"] = int64_t{" << rpc.channel << "};\n";
            source << "            rpc[" << godot_string_name(function.name)
                   << "] = config;\n        }\n";
        }
        source << "        descriptor.rpc_config = rpc;\n    }\n";
    }
    source << "    return descriptor;\n}\n\n";
}

void CodeGenerator::emit_inner_class_declaration(const typed::Class& declaration,
                                                 std::ostringstream& header,
                                                 const std::string& native_name,
                                                 const std::string& source_name,
                                                 const bool tool_mode) const {
    const auto* previous_inner_script = current_inner_script_;
    const auto previous_function_parameters = local_function_parameters_;
    const auto previous_functions = local_functions_;
    const auto previous_function_native_names = local_function_native_names_;
    const auto previous_native_class_name = current_native_class_name_;
    const auto previous_godot_base_type = current_godot_base_type_;
    const auto previous_attached_godot_base_type = attached_godot_base_type_;
    local_function_parameters_.clear();
    local_functions_.clear();
    local_function_native_names_.clear();
    current_native_class_name_ = native_name;
    for (const auto& function : declaration.functions) {
        local_functions_.emplace(function.name, &function);
        auto& parameters = local_function_parameters_[function.name];
        for (const auto& parameter : function.parameters)
            parameters.push_back(parameter.type);
    }
    current_inner_script_ = script_symbols_ && current_script_
                                ? script_symbols_->find_inner(*current_script_, source_name)
                                : nullptr;
    const auto resolved_base = inner_base_names_.find(source_name);
    const auto source_base =
        resolved_base == inner_base_names_.end() ? declaration.base_type : resolved_base->second;
    const auto native_inner_base = inner_cpp_type(source_base);
    const auto* native_script_base = current_inner_script_ && script_symbols_
                                         ? script_symbols_->base_of(*current_inner_script_)
                                         : nullptr;
    const auto base_cpp = !native_inner_base.empty() ? native_inner_base
                          : native_script_base       ? native_script_base->native_class_name
                                                     : "gdpp::runtime::AttachedScriptBehavior";
    const auto godot_base = inner_godot_base_type(source_name);
    current_godot_base_type_ = godot_base;
    attached_godot_base_type_ = godot_base;
    const auto engine_virtual_for = [&](const typed::Function& function) {
        static_cast<void>(function);
        return static_cast<const GodotMethodRecord*>(nullptr);
    };
    const auto coroutine_abi_for = [&](const typed::Function& function) {
        return function.is_coroutine && !engine_virtual_for(function);
    };
    const auto function_return_type = [&](const typed::Function& function) {
        return coroutine_abi_for(function) ? std::string{"godot::Variant"}
                                           : cpp_type(function.return_type);
    };
    const auto function_symbol = [&](const typed::Function& function) {
        if (!current_inner_script_)
            return static_cast<const ScriptMemberSymbol*>(nullptr);
        const auto member =
            std::find_if(current_inner_script_->members.begin(),
                         current_inner_script_->members.end(), [&](const auto& candidate) {
                             return candidate.kind == ScriptMemberKind::function &&
                                    candidate.name == function.name;
                         });
        return member == current_inner_script_->members.end() ? nullptr : &*member;
    };
    const auto function_native_name = [&](const typed::Function& function) {
        const auto* symbol = function_symbol(function);
        if (symbol)
            return inner_method_implementation_name(*current_inner_script_, *symbol);
        return typed_inner_method_implementation_name(source_name, function);
    };
    for (const auto& function : declaration.functions)
        local_function_native_names_.insert_or_assign(function.name,
                                                      function_native_name(function));
    const bool has_static_initialization =
        requires_static_initialization(declaration.fields, declaration.functions);
    header << "class " << native_name << " : public " << base_cpp << " {\n"
           << "    GDCLASS(" << native_name << ", " << base_cpp << ")\n\n"
           << "public:\n"
           << "    inline static constexpr bool _gdpp_tool_mode = "
           << (tool_mode ? "true" : "false") << ";\n"
           << "    inline static constexpr bool _gdpp_attached = true;\n"
           << "    inline static constexpr bool _gdpp_attached_ref_counted = "
           << (api_.inherits(godot_base, "RefCounted") ? "true" : "false") << ";\n"
           << "    inline static constexpr const char *_gdpp_source_path = "
           << escaped_string("res://" + current_source_path_ + "::" + source_name) << ";\n"
           << "    " << native_name << "();\n"
           << "    void _gdpp_initialize_instance() override;\n"
           << "    void _gdpp_initialize_onready() override;\n"
           << "    void _gdpp_dispatch_notification(std::int32_t what, bool reversed) override;\n";
    for (const auto& enumeration : declaration.enums) {
        if (enumeration.name.empty()) {
            for (const auto& entry : enumeration.entries)
                header << "    inline static constexpr int64_t " << enum_identifier(entry.name)
                       << " = " << entry.value << ";\n";
        } else {
            emit_named_enum_declaration(enumeration, header, 4);
        }
    }
    for (const auto& field : declaration.fields) {
        if (!field.is_constant)
            continue;
        header << "    static const " << cpp_type(field.type);
        if (managed_constant_field(field))
            header << "& " << sanitize_identifier(field.name) << "();\n";
        else
            header << ' ' << sanitize_identifier(field.name) << ";\n";
    }
    header << "\nprotected:\n    static void _bind_methods();\n";
    for (const auto& function : declaration.functions) {
        if (function.name != "_static_init" && !engine_virtual_for(function)) {
            header << emit_method_callback_declaration(function);
        }
    }
    bool fields = false;
    for (const auto& field : declaration.fields) {
        if (field.is_constant)
            continue;
        fields = true;
        header << "    ";
        if (field.is_static) {
            const auto name = sanitize_identifier(field.name);
            const auto type = cpp_type(field.type);
            header << "static std::atomic<" << type << "*>& _gdpp_static_" << name
                   << "_pointer();\n"
                   << "    static std::mutex& _gdpp_static_" << name << "_mutex();\n"
                   << "    static " << type << "& _gdpp_static_" << name << "_storage();\n"
                   << "    static void _gdpp_static_" << name << "_release();\n";
        } else {
            header << cpp_type(field.type) << ' ' << sanitize_identifier(field.name) << "{};\n";
        }
    }
    for (const auto& field : declaration.fields) {
        if (cached_preload_field(field) && !field.is_static) {
            const auto name = sanitize_identifier(field.name);
            const auto type = cpp_type(field.type);
            header << "    static std::atomic<" << type << "*>& _gdpp_preloaded_" << name
                   << "_pointer();\n"
                   << "    static std::mutex& _gdpp_preloaded_" << name << "_mutex();\n"
                   << "    static " << type << "& _gdpp_preloaded_" << name << "();\n"
                   << "    static void _gdpp_preloaded_" << name << "_release();\n";
        }
    }
    for (const auto& field : declaration.fields) {
        if (!managed_constant_field(field))
            continue;
        const auto name = sanitize_identifier(field.name);
        const auto type = cpp_type(field.type);
        header << "    static std::atomic<" << type << "*>& _gdpp_constant_" << name
               << "_pointer();\n"
               << "    static " << type << "& _gdpp_constant_" << name
               << "_storage();\n"
               << "    static bool& _gdpp_constant_" << name << "_ready();\n"
               << "    static std::mutex& _gdpp_constant_" << name << "_mutex();\n"
               << "    static void _gdpp_constant_" << name << "_release();\n";
    }
    if (has_static_initialization) {
        header << "    static gdpp::runtime::ScriptInitializationState& "
                  "_gdpp_static_initialization();\n"
               << "    static bool _gdpp_ensure_static_initialized();\n";
    }
    if (std::any_of(declaration.fields.begin(), declaration.fields.end(), cached_preload_field)) {
        header << "    static gdpp::runtime::ScriptInitializationState& "
                  "_gdpp_preload_initialization();\n";
    }
    if (!fields)
        header << "    // No internal class fields.\n";
    header << "\npublic:\n"
           << "    static void _gdpp_preload_resources();\n"
           << "    static void _gdpp_release_preloaded_resources();\n"
           << "    static gdpp::runtime::AttachedScriptDescriptor _gdpp_descriptor();\n";
    for (const auto& field : declaration.fields) {
        if (field.is_constant)
            continue;
        const auto name = sanitize_identifier(field.name);
        const auto setter_parameter = field.setter && field.setter->method.empty()
                                          ? sanitize_identifier(field.setter->parameter)
                                          : "value";
        const auto getter_type = field.getter && field.getter->is_coroutine
                                     ? std::string{"godot::Variant"}
                                     : cpp_type(field.type);
        header << "    " << (field.is_static ? "static " : "") << getter_type << " _gdpp_get_"
               << name << "();\n"
               << "    " << (field.is_static ? "static " : "") << "void _gdpp_set_" << name << '('
               << cpp_type(field.type) << ' ' << setter_parameter << ");\n";
    }
    for (const auto& function : declaration.functions) {
        if (const auto* method = engine_virtual_for(function)) {
            header << "    virtual " << virtual_return_type(*method) << ' '
                   << sanitize_identifier(function.name) << '(';
            for (std::size_t index = 0; index < method->maximum_arguments; ++index) {
                if (index != 0)
                    header << ", ";
                header << virtual_parameter_type(*method, index) << " _gdpp_engine_argument_"
                       << index;
            }
            header << ')';
            if (method->is_const)
                header << " const";
            header << " override;\n";
        }
        header << "    "
               << (function.is_static         ? "static "
                   : function.name == "_init" ? ""
                                              : "virtual ")
               << function_return_type(function) << ' ' << function_native_name(function) << '(';
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0)
                header << ", ";
            const auto& parameter = function.parameters[index];
            header << parameter_native_type(parameter) << ' ' << parameter_native_name(parameter);
            if (parameter.default_value && !function.rest_parameter)
                header << " = " << emit_parameter_default(parameter);
        }
        if (function.rest_parameter) {
            if (!function.parameters.empty())
                header << ", ";
            header << "godot::Array " << sanitize_identifier(function.rest_parameter->name);
        }
        header << ')';
        if (const auto* symbol = function_symbol(function);
            symbol && inner_overrides_method(*current_inner_script_, *symbol)) {
            header << " override";
        } else if (!symbol) {
            std::string base_owner;
            const auto* inherited =
                find_inherited_inner_function(source_base, function.name, &base_owner);
            if (inherited && !function.is_static &&
                same_native_function_abi(function, *inherited)) {
                header << " override";
            }
        }
        header << ";\n";
    }
    header << "};\n\n";
    local_function_parameters_ = previous_function_parameters;
    local_functions_ = previous_functions;
    local_function_native_names_ = previous_function_native_names;
    current_native_class_name_ = previous_native_class_name;
    current_godot_base_type_ = previous_godot_base_type;
    attached_godot_base_type_ = previous_attached_godot_base_type;
    current_inner_script_ = previous_inner_script;
}

void CodeGenerator::emit_inner_class_definition(const typed::Class& declaration,
                                                std::ostringstream& source,
                                                const std::string& native_name,
                                                const std::string& source_name,
                                                const bool tool_mode) const {
    const auto* previous_inner_script = current_inner_script_;
    const auto previous_function_parameters = local_function_parameters_;
    const auto previous_functions = local_functions_;
    const auto previous_function_native_names = local_function_native_names_;
    const auto previous_native_class_name = current_native_class_name_;
    const auto previous_godot_base_type = current_godot_base_type_;
    const auto previous_attached_godot_base_type = attached_godot_base_type_;
    const auto previous_debug_source = current_debug_source_;
    const auto previous_debug_function = current_debug_function_;
    const auto previous_debug_instance = current_debug_instance_;
    const auto previous_debug_members = current_debug_members_;
    local_function_parameters_.clear();
    local_functions_.clear();
    local_function_native_names_.clear();
    current_native_class_name_ = native_name;
    current_debug_source_ = "res://" + current_source_path_ + "::" + source_name;
    current_debug_function_.clear();
    current_debug_instance_.clear();
    current_debug_members_.clear();
    for (const auto& field : declaration.fields) {
        if (!field.is_constant && !field.is_static) {
            current_debug_members_.emplace_back(field.name,
                                                "this->" + sanitize_identifier(field.name));
        }
    }
    for (const auto& function : declaration.functions) {
        local_functions_.emplace(function.name, &function);
        auto& parameters = local_function_parameters_[function.name];
        for (const auto& parameter : function.parameters)
            parameters.push_back(parameter.type);
    }
    current_inner_script_ = script_symbols_ && current_script_
                                ? script_symbols_->find_inner(*current_script_, source_name)
                                : nullptr;
    if (script_symbols_ && current_inner_script_) {
        std::unordered_set<std::string> debug_member_names;
        for (const auto& [name, expression] : current_debug_members_) {
            static_cast<void>(expression);
            debug_member_names.insert(name);
        }
        const auto add_debug_members = [&](const auto& members) {
            for (const auto& member : members) {
                if (member.kind == ScriptMemberKind::field && !member.is_static &&
                    debug_member_names.insert(member.name).second) {
                    current_debug_members_.emplace_back(
                        member.name, "this->" + sanitize_identifier(member.name));
                }
            }
        };
        std::unordered_set<const ScriptInnerClassSymbol*> visited_inner;
        for (auto* base = script_symbols_->inner_base_of(*current_inner_script_);
             base && visited_inner.insert(base).second;
             base = script_symbols_->inner_base_of(*base)) {
            add_debug_members(base->members);
        }
        std::unordered_set<const ScriptClassSymbol*> visited_scripts;
        for (auto* base = script_symbols_->base_of(*current_inner_script_);
             base && visited_scripts.insert(base).second; base = script_symbols_->base_of(*base)) {
            add_debug_members(base->members);
        }
    }
    const auto godot_base = inner_godot_base_type(source_name);
    current_godot_base_type_ = godot_base;
    attached_godot_base_type_ = godot_base;
    const auto resolved_base = inner_base_names_.find(source_name);
    const auto source_base =
        resolved_base == inner_base_names_.end() ? declaration.base_type : resolved_base->second;
    const auto native_inner_base = inner_cpp_type(source_base);
    const auto* native_script_base = current_inner_script_ && script_symbols_
                                         ? script_symbols_->base_of(*current_inner_script_)
                                         : nullptr;
    const auto base_cpp = !native_inner_base.empty() ? native_inner_base
                          : native_script_base       ? native_script_base->native_class_name
                                                     : "gdpp::runtime::AttachedScriptBehavior";
    const auto base_script_path = [&]() {
        // Runtime descriptor identities always use canonical source paths. The C++ inheritance
        // map may contain a generated native class name after project-level semantic resolution;
        // publishing that implementation name after `::` creates an unregistered Script
        // identity and breaks dynamic construction of every derived internal class.
        if (current_inner_script_ && script_symbols_) {
            if (const auto* inner_base = script_symbols_->inner_base_of(*current_inner_script_)) {
                if (const auto* owner = script_symbols_->owner_of(*inner_base)) {
                    return "res://" + owner->path + "::" + inner_base->name;
                }
            }
            if (const auto* script_base = script_symbols_->base_of(*current_inner_script_))
                return "res://" + script_base->path;
        }
        if (!native_inner_base.empty()) {
            if (current_inner_script_ && !current_inner_script_->base_script_path.empty() &&
                !current_inner_script_->base_class_name.empty()) {
                return "res://" + current_inner_script_->base_script_path +
                       "::" + current_inner_script_->base_class_name;
            }
            return "res://" + current_source_path_ + "::" + source_base;
        }
        return native_script_base ? "res://" + native_script_base->path : std::string{};
    }();
    const auto native_base_type = inner_attached_native_base_type(source_name);
    const auto engine_virtual_for = [&](const typed::Function& function) {
        static_cast<void>(function);
        return static_cast<const GodotMethodRecord*>(nullptr);
    };
    const auto coroutine_abi_for = [&](const typed::Function& function) {
        return function.is_coroutine && !engine_virtual_for(function);
    };
    const auto function_return_type = [&](const typed::Function& function) {
        return coroutine_abi_for(function) ? std::string{"godot::Variant"}
                                           : cpp_type(function.return_type);
    };
    const auto function_symbol = [&](const typed::Function& function) {
        if (!current_inner_script_)
            return static_cast<const ScriptMemberSymbol*>(nullptr);
        const auto member =
            std::find_if(current_inner_script_->members.begin(),
                         current_inner_script_->members.end(), [&](const auto& candidate) {
                             return candidate.kind == ScriptMemberKind::function &&
                                    candidate.name == function.name;
                         });
        return member == current_inner_script_->members.end() ? nullptr : &*member;
    };
    const auto function_native_name = [&](const typed::Function& function) {
        const auto* symbol = function_symbol(function);
        if (symbol)
            return inner_method_implementation_name(*current_inner_script_, *symbol);
        return typed_inner_method_implementation_name(source_name, function);
    };
    for (const auto& function : declaration.functions)
        local_function_native_names_.insert_or_assign(function.name,
                                                      function_native_name(function));
    const bool has_static_initialization =
        requires_static_initialization(declaration.fields, declaration.functions);
    const bool has_static_initializer =
        std::any_of(declaration.functions.begin(), declaration.functions.end(),
                    [](const auto& function) { return function.name == "_static_init"; });
    if (has_static_initialization) {
        source << "gdpp::runtime::ScriptInitializationState& " << native_name
               << "::_gdpp_static_initialization() {\n"
               << "    static gdpp::runtime::ScriptInitializationState state;\n"
               << "    return state;\n}\n\n"
               << "bool " << native_name << "::_gdpp_ensure_static_initialized() {\n";
        if (!tool_mode)
            source << "    if (gdpp::runtime::is_editor_hint()) return true;\n";
        source << "    return _gdpp_static_initialization().run(\n"
               << "        \"Static initialization previously failed for " << native_name
               << ".\",\n"
               << "        []() {\n";
        for (const auto& field : declaration.fields) {
            if (!field.is_constant && field.is_static) {
                source << "            (void)_gdpp_static_" << sanitize_identifier(field.name)
                       << "_storage();\n"
                       << "            if (script_function_failed()) return;\n";
            }
        }
        if (has_static_initializer)
            source << "            _gdpp_script_method__static_init();\n";
        source << "        },\n"
               << "        []() {\n";
        for (const auto& field : declaration.fields) {
            if (managed_constant_field(field)) {
                const auto name = sanitize_identifier(field.name);
                source << "            _gdpp_constant_" << name << "_release();\n";
            } else if (!field.is_constant && field.is_static) {
                source << "            _gdpp_static_" << sanitize_identifier(field.name)
                       << "_release();\n";
            }
        }
        source << "        });\n"
               << "}\n\n";
    }
    for (const auto& field : declaration.fields) {
        if (!field.is_constant)
            continue;
        const auto name = sanitize_identifier(field.name);
        if (!managed_constant_field(field)) {
            source << "const " << cpp_type(field.type) << ' ' << native_name << "::" << name
                   << " = " << emit_expression(*field.initializer) << ";\n";
            continue;
        }
        const auto type = cpp_type(field.type);
        const auto storage = "_gdpp_constant_" + name + "_storage()";
        source << "std::atomic<" << type << "*>& " << native_name << "::_gdpp_constant_" << name
               << "_pointer() {\n    static std::atomic<" << type
               << "*> value{nullptr};\n    return value;\n}\n\n"
               << type << "& " << native_name << "::_gdpp_constant_" << name
               << "_storage() {\n"
               << "    auto* value = _gdpp_constant_" << name
               << "_pointer().load(std::memory_order_acquire);\n"
               << "    if (value) return *value;\n"
               << "    std::lock_guard<std::mutex> lock(_gdpp_constant_" << name << "_mutex());\n"
               << "    value = _gdpp_constant_" << name
               << "_pointer().load(std::memory_order_relaxed);\n"
               << "    if (!value) {\n"
               << "        value = new " << type
               << (field.type.kind == TypeKind::script_resource ? "{" + type + "::missing()}"
                                                                : "{}")
               << ";\n"
               << "        _gdpp_constant_" << name
               << "_pointer().store(value, std::memory_order_release);\n"
               << "    }\n"
               << "    return *value;\n}\n\n"
               << "bool& " << native_name << "::_gdpp_constant_" << name
               << "_ready() {\n    static bool value = false;\n    return value;\n}\n\n"
               << "std::mutex& " << native_name << "::_gdpp_constant_" << name
               << "_mutex() {\n    static std::mutex value;\n    return value;\n}\n\n"
               << "void " << native_name << "::_gdpp_constant_" << name << "_release() {\n"
               << "    std::lock_guard<std::mutex> lock(_gdpp_constant_" << name << "_mutex());\n"
               << "    _gdpp_constant_" << name << "_ready() = false;\n"
               << "    delete _gdpp_constant_" << name
               << "_pointer().exchange(nullptr, std::memory_order_acq_rel);\n}\n\n"
               << "const " << type << "& " << native_name << "::" << name << "() {\n"
               << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
               << (has_static_initialization ? "    (void)_gdpp_ensure_static_initialized();\n"
                                               "    if (script_function_failed()) return " +
                                                   storage + ";\n"
                                             : "")
               << "    auto& value = " << storage << ";\n"
               << "    std::lock_guard<std::mutex> lock(_gdpp_constant_" << name << "_mutex());\n"
               << "    if (!_gdpp_constant_" << name << "_ready()) {\n"
               << "        "
               << emit_storage_assignment(field.type, "value",
                                          emit_conversion(field.type,
                                                          emitted_type(*field.initializer),
                                                          emit_expression(*field.initializer),
                                                          &field.initializer->span))
               << ";\n"
               << "        if (script_function_failed()) return value;\n"
               << "        _gdpp_constant_" << name << "_ready() = true;\n"
               << "    }\n"
               << "    return value;\n}\n\n";
    }
    for (const auto& field : declaration.fields) {
        if (!field.is_constant && field.is_static) {
            const auto name = sanitize_identifier(field.name);
            const auto type = cpp_type(field.type);
            source << "std::atomic<" << type << "*>& " << native_name << "::_gdpp_static_" << name
                   << "_pointer() {\n    static std::atomic<" << type
                   << "*> value{nullptr};\n    return value;\n}\n\n"
                   << "std::mutex& " << native_name << "::_gdpp_static_" << name
                   << "_mutex() {\n    static std::mutex value;\n    return value;\n}\n\n"
                   << type << "& " << native_name << "::_gdpp_static_" << name
                   << "_storage() {\n"
                   << "    bool initialize = true;\n";
            if (!tool_mode) {
                source << "    if (gdpp::runtime::is_editor_hint()) initialize = false;\n";
            }
            source << "    if (initialize && !_gdpp_ensure_static_initialized()) initialize = false;\n"
                   << "    auto* value = _gdpp_static_" << name
                   << "_pointer().load(std::memory_order_acquire);\n"
                   << "    if (value) return *value;\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_static_" << name << "_mutex());\n"
                   << "    value = _gdpp_static_" << name
                   << "_pointer().load(std::memory_order_relaxed);\n"
                   << "    if (!value) {\n"
                   << "        value = new " << type << "{};\n"
                   << "        _gdpp_static_" << name
                   << "_pointer().store(value, std::memory_order_release);\n"
                   << "        if (initialize) {\n";
            if (field.initializer && !field.onready) {
                source << "            "
                       << emit_storage_assignment(
                              field.type, "*value",
                              emit_conversion(field.type, emitted_type(*field.initializer),
                                              emit_expression(*field.initializer),
                                              &field.initializer->span))
                       << ";\n";
            }
            source << "        }\n"
                   << "    }\n"
                   << "    return *value;\n}\n\n"
                   << "void " << native_name << "::_gdpp_static_" << name << "_release() {\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_static_" << name << "_mutex());\n"
                   << "    delete _gdpp_static_" << name
                   << "_pointer().exchange(nullptr, std::memory_order_acq_rel);\n}\n\n";
        }
    }
    const auto emit_instance_initializers = [&]() {
        for (const auto& field : declaration.fields) {
            if (!field.is_constant && !field.is_static && !field.onready && field.initializer) {
                const bool editor_safe = editor_safe_initializer(*field.initializer);
                if (!editor_safe && !tool_mode)
                    source << "    if (!gdpp_editor_hint) {\n";
                std::string value;
                if (contains_preload(*field.initializer)) {
                    value = "_gdpp_preloaded_" + sanitize_identifier(field.name) + "()";
                } else {
                    value = emit_conversion(field.type, emitted_type(*field.initializer),
                                            emit_expression(*field.initializer),
                                            &field.initializer->span);
                }
                source << (editor_safe || tool_mode ? "    " : "        ")
                       << emit_storage_assignment(field.type, sanitize_identifier(field.name),
                                                  std::move(value))
                       << ";\n"
                       << (editor_safe || tool_mode ? "    " : "        ")
                       << "if (script_function_failed()) return;\n";
                if (!editor_safe && !tool_mode)
                    source << "    }\n";
            }
        }
    };
    const bool needs_editor_hint =
        !tool_mode &&
        (std::any_of(declaration.fields.begin(), declaration.fields.end(), cached_preload_field) ||
         std::any_of(declaration.fields.begin(), declaration.fields.end(), [](const auto& field) {
             return !field.is_constant && !field.is_static && !field.onready && field.initializer &&
                    !editor_safe_initializer(*field.initializer);
         }));
    in_function_body_ = true;
    source << native_name << "::" << native_name << "() {\n"
           << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
              "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n";
    if (has_static_initialization) {
        source << "    (void)_gdpp_ensure_static_initialized();\n"
               << "    if (script_function_failed()) return;\n";
    }
    if (std::any_of(declaration.fields.begin(), declaration.fields.end(), cached_preload_field)) {
        source << "    _gdpp_preload_resources();\n"
               << "    if (script_function_failed()) return;\n";
    }
    source << "}\n\n"
           << "void " << native_name << "::_gdpp_initialize_instance() {\n"
           << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
              "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
           << "    " << base_cpp << "::_gdpp_initialize_instance();\n"
           << "    if (script_function_failed()) return;\n";
    if (needs_editor_hint)
        source << "    const bool gdpp_editor_hint = gdpp::runtime::is_editor_hint();\n";
    emit_instance_initializers();
    source << "}\n\n"
           << "void " << native_name << "::_gdpp_initialize_onready() {\n"
           << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
              "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
           << "    " << base_cpp << "::_gdpp_initialize_onready();\n"
           << "    if (script_function_failed()) return;\n";
    for (const auto& field : declaration.fields) {
        if (field.onready && field.initializer) {
            source << "    "
                   << emit_storage_assignment(field.type, sanitize_identifier(field.name),
                                              emit_conversion(field.type,
                                                              emitted_type(*field.initializer),
                                                              emit_expression(*field.initializer),
                                                              &field.initializer->span))
                   << ";\n"
                   << "    if (script_function_failed()) return;\n";
        }
    }
    source << "}\n\n";

    const auto notification =
        std::find_if(declaration.functions.begin(), declaration.functions.end(),
                     [](const typed::Function& function) {
                         return !function.is_static && function.name == "_notification";
                     });
    source << "void " << native_name
           << "::_gdpp_dispatch_notification(std::int32_t what, bool reversed) {\n"
           << "    if (reversed) " << base_cpp << "::_gdpp_dispatch_notification(what, true);\n";
    if (notification != declaration.functions.end())
        source << "    " << function_native_name(*notification) << "(what);\n";
    source << "    if (!reversed) " << base_cpp << "::_gdpp_dispatch_notification(what, false);\n"
           << "}\n\n";
    in_function_body_ = false;
    for (const auto& field : declaration.fields) {
        if (cached_preload_field(field) && !field.is_static) {
            const auto name = sanitize_identifier(field.name);
            const auto type = cpp_type(field.type);
            source << "std::atomic<" << type << "*>& " << native_name << "::_gdpp_preloaded_"
                   << name << "_pointer() {\n    static std::atomic<" << type
                   << "*> value{nullptr};\n    return value;\n}\n\n"
                   << "std::mutex& " << native_name << "::_gdpp_preloaded_" << name
                   << "_mutex() {\n    static std::mutex value;\n    return value;\n}\n\n"
                   << type << "& " << native_name << "::_gdpp_preloaded_" << name << "() {\n"
                   << "    auto* value = _gdpp_preloaded_" << name
                   << "_pointer().load(std::memory_order_acquire);\n"
                   << "    if (value) return *value;\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_preloaded_" << name
                   << "_mutex());\n"
                   << "    value = _gdpp_preloaded_" << name
                   << "_pointer().load(std::memory_order_relaxed);\n"
                   << "    if (!value) {\n"
                   << "        value = new " << type << "{};\n"
                   << "        _gdpp_preloaded_" << name
                   << "_pointer().store(value, std::memory_order_release);\n"
                   << "    }\n"
                   << "    return *value;\n}\n\n"
                   << "void " << native_name << "::_gdpp_preloaded_" << name << "_release() {\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_preloaded_" << name
                   << "_mutex());\n"
                   << "    delete _gdpp_preloaded_" << name
                   << "_pointer().exchange(nullptr, std::memory_order_acq_rel);\n}\n\n";
        }
    }
    const bool has_cached_preloads =
        std::any_of(declaration.fields.begin(), declaration.fields.end(), cached_preload_field);
    const auto emit_cached_preload_cleanup = [&](const std::string& prefix) {
        for (const auto& field : declaration.fields) {
            if (!cached_preload_field(field))
                continue;
            if (field.is_static) {
                source << prefix << "_gdpp_static_" << sanitize_identifier(field.name)
                       << "_release();\n";
                continue;
            }
            source << prefix << "_gdpp_preloaded_" << sanitize_identifier(field.name)
                   << "_release();\n";
        }
    };
    if (has_cached_preloads) {
        source << "gdpp::runtime::ScriptInitializationState& " << native_name
               << "::_gdpp_preload_initialization() {\n"
               << "    static gdpp::runtime::ScriptInitializationState state;\n"
               << "    return state;\n}\n\n";
    }
    source << "void " << native_name << "::_gdpp_preload_resources() {\n";
    if (has_cached_preloads) {
        if (!tool_mode)
            source << "    if (gdpp::runtime::is_editor_hint()) return;\n";
        source << "    (void)_gdpp_preload_initialization().run(\n"
               << "        \"Preload initialization previously failed for " << native_name
               << ".\",\n"
               << "        []() {\n";
    }
    for (const auto& field : declaration.fields) {
        if (!cached_preload_field(field))
            continue;
        const auto prefix = has_cached_preloads ? "            " : "    ";
        std::string target;
        if (!field.is_static)
            target = "_gdpp_preloaded_";
        target += sanitize_identifier(field.name);
        if (!field.is_static)
            target += "()";
        source << prefix
               << emit_storage_assignment(field.type, std::move(target),
                                          emit_conversion(field.type,
                                                          emitted_type(*field.initializer),
                                                          emit_expression(*field.initializer),
                                                          &field.initializer->span))
               << ";\n"
               << prefix << "if (script_function_failed()) return;\n";
    }
    if (has_cached_preloads) {
        source << "        },\n"
               << "        []() {\n";
        emit_cached_preload_cleanup("            ");
        source << "        });\n";
    }
    source << "}\n\n";
    source << "void " << native_name << "::_gdpp_release_preloaded_resources() {\n";
    if (has_cached_preloads) {
        source << "    _gdpp_preload_initialization().reset([]() {\n";
        emit_cached_preload_cleanup("        ");
        source << "    });\n";
    }
    if (has_static_initialization) {
        source << "    _gdpp_static_initialization().reset([]() {\n";
        for (const auto& field : declaration.fields) {
            if (managed_constant_field(field)) {
                const auto name = sanitize_identifier(field.name);
                source << "        _gdpp_constant_" << name << "_release();\n";
            } else if (!field.is_constant && field.is_static) {
                source << "        _gdpp_static_" << sanitize_identifier(field.name)
                       << "_release();\n";
            }
        }
        source << "    });\n";
    } else {
        for (const auto& field : declaration.fields) {
            if (!managed_constant_field(field))
                continue;
            const auto name = sanitize_identifier(field.name);
            source << "    _gdpp_constant_" << name << "_release();\n";
        }
    }
    source << "}\n\n";
    source << "void " << native_name << "::_bind_methods() {\n";
    source << "    godot::ClassDB::bind_static_method(get_class_static(), "
              "godot::D_METHOD(\"_gdpp_preload_resources\"), &"
           << native_name << "::_gdpp_preload_resources);\n";
    std::unordered_map<std::string, std::size_t> inner_enum_member_counts;
    for (const auto& enumeration : declaration.enums) {
        for (const auto& entry : enumeration.entries)
            ++inner_enum_member_counts[entry.name];
    }
    for (const auto& enumeration : declaration.enums) {
        for (const auto& entry : enumeration.entries) {
            // GDExtension integer constants occupy a flat namespace per native class even when
            // an enum name is supplied. GDScript permits the same member name in multiple named
            // enums, so omit only the ambiguous reflection entries; generated C++ and exported
            // property hints still retain every enum member and value.
            if (inner_enum_member_counts[entry.name] != 1)
                continue;
            const auto value = enumeration.name.empty() ? enum_identifier(entry.name)
                                                        : sanitize_identifier(enumeration.name) +
                                                              "::" + enum_identifier(entry.name);
            source << "    godot::ClassDB::bind_integer_constant(get_class_static(), "
                   << godot_text_argument(enumeration.name) << ", "
                   << godot_text_argument(entry.name) << ", " << value << ");\n";
        }
    }
    for (const auto& function : declaration.functions) {
        if (function.name == "_static_init" ||
            (!function.is_static && engine_virtual_for(function)))
            continue;
        source << emit_method_registration(function, native_name, function_return_type(function));
    }
    for (const auto& field : declaration.fields) {
        if (field.is_constant || field.is_static)
            continue;
        const auto name = sanitize_identifier(field.name);
        source << "    godot::ClassDB::bind_method(godot::D_METHOD(\"_gdpp_get_" << name << "\"), &"
               << native_name << "::_gdpp_get_" << name << ");\n"
               << "    godot::ClassDB::bind_method(godot::D_METHOD(\"_gdpp_set_" << name
               << "\", \"value\"), &" << native_name << "::_gdpp_set_" << name << ");\n"
               << "    ADD_PROPERTY("
               << property_info(field, api_, script_symbols_, cpp_type(field.type))
               << ", \"_gdpp_set_" << name << "\", \"_gdpp_get_" << name << "\");\n";
    }
    for (const auto& signal : declaration.signals) {
        source << "    ADD_SIGNAL(godot::MethodInfo(" << godot_text_argument(signal.name);
        for (const auto& parameter : signal.parameters)
            source << ", " << native_property_info(parameter.type, parameter.name);
        source << "));\n";
    }
    source << "}\n\n";
    for (const auto& field : declaration.fields) {
        if (field.is_constant)
            continue;
        const auto name = sanitize_identifier(field.name);
        const auto source_owner = source_name + "::" + field.name;
        record_generated_symbol(GeneratedSymbolKind::property_getter, source_owner + ":get",
                                native_name + "::_gdpp_get_" + name,
                                field.getter ? field.getter->span : field.span);
        record_generated_symbol(GeneratedSymbolKind::property_setter, source_owner + ":set",
                                native_name + "::_gdpp_set_" + name,
                                field.setter ? field.setter->span : field.span);
        const bool inline_getter_coroutine =
            field.getter && field.getter->method.empty() && field.getter->is_coroutine;
        current_return_type_ = field.type;
        current_coroutine_abi_ = inline_getter_coroutine;
        current_static_context_ = field.is_static;
        current_coroutine_state_ = inline_getter_coroutine ? "_gdpp_property_coroutine_state" : "";
        source << (field.getter && field.getter->is_coroutine ? "godot::Variant"
                                                              : cpp_type(field.type))
               << ' ' << native_name << "::_gdpp_get_" << name << "() {\n";
        if (inline_getter_coroutine) {
            source << "    const auto " << current_coroutine_state_
                   << " = gdpp::runtime::begin_coroutine("
                   << (field.is_static ? "nullptr" : self_object_expression()) << ");\n";
        }
        if (field.is_static && has_static_initialization) {
            source << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                      "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
                   << "    (void)_gdpp_ensure_static_initialized();\n";
            source << emit_script_failure_return(1, false);
        }
        if (field.getter && !field.getter->method.empty()) {
            source << "    return " << local_function_implementation_name(field.getter->method)
                   << "();\n";
        } else if (field.getter) {
            current_return_type_ = field.type;
            in_function_body_ = true;
            current_debug_function_ = "@" + field.name + "_getter";
            current_debug_instance_ = field.is_static ? "nullptr" : "owner()";
            source << emit_script_function_scope(1);
            source << emit_debug_frame(field.getter->span.begin.line, 1);
            source << emit_statements(field.getter->body, 1);
            if (requires_native_fallback(field.getter->body))
                source << "    return {};\n";
        } else {
            source << "    return "
                   << (field.is_static ? "_gdpp_static_" + name + "_storage()" : name) << ";\n";
        }
        source << "}\n\n";
        current_coroutine_abi_ = false;
        current_coroutine_state_.clear();
        current_static_context_ = field.is_static;
        current_return_type_ = {TypeKind::void_type, "void"};
        source << "void " << native_name << "::_gdpp_set_" << name << '(' << "[[maybe_unused]] "
               << cpp_type(field.type) << ' '
               << (field.setter && field.setter->method.empty()
                       ? sanitize_identifier(field.setter->parameter)
                       : "value")
               << ") {\n";
        if (field.type == Type{TypeKind::object, "GDScript"}) {
            const auto parameter =
                field.setter && field.setter->method.empty()
                    ? sanitize_identifier(field.setter->parameter)
                    : std::string{"value"};
            source << "    gdpp::runtime::ScriptFunctionScope _gdpp_gdscript_storage_scope("
                      "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
                   << "    " << parameter
                   << " = gdpp::runtime::strict_gdscript_storage("
                      "gdpp::runtime::to_variant("
                   << parameter << "), gdpp::runtime::ScriptSourceLocation{_gdpp_source_path, "
                   << field.span.begin.line << ", " << field.span.begin.column << "});\n"
                   << "    if (script_function_failed()) return;\n";
        }
        if (field.is_static && has_static_initialization) {
            source << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                      "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
                   << "    (void)_gdpp_ensure_static_initialized();\n"
                   << "    if (script_function_failed()) return;\n";
        }
        if (field.setter && !field.setter->method.empty()) {
            source << "    " << local_function_implementation_name(field.setter->method)
                   << "(value);\n";
        } else if (field.setter) {
            current_return_type_ = {TypeKind::void_type, "void"};
            in_function_body_ = true;
            current_debug_function_ = "@" + field.name + "_setter";
            current_debug_instance_ = field.is_static ? "nullptr" : "owner()";
            source << emit_script_function_scope(1);
            source << emit_debug_frame(field.setter->span.begin.line, 1);
            source << emit_statements(field.setter->body, 1);
        } else {
            source << "    "
                   << emit_storage_assignment(
                          field.type,
                          field.is_static ? "_gdpp_static_" + name + "_storage()" : name, "value")
                   << ";\n";
        }
        source << "}\n\n";
        in_function_body_ = false;
        current_coroutine_abi_ = false;
        current_coroutine_state_.clear();
        current_static_context_ = false;
    }
    for (const auto& function : declaration.functions) {
        current_return_type_ = function.return_type;
        current_coroutine_abi_ = coroutine_abi_for(function);
        current_static_context_ = function.is_static;
        current_coroutine_state_ = current_coroutine_abi_ ? "_gdpp_coroutine_state" : "";
        in_function_body_ = true;
        current_debug_function_ = function.name;
        current_debug_instance_ = function.is_static ? "nullptr" : "owner()";
        const auto* engine_virtual = engine_virtual_for(function);
        const auto native_function_name = function_native_name(function);
        const auto source_symbol = source_name + "::" + function.name;
        if (engine_virtual) {
            record_generated_symbol(GeneratedSymbolKind::virtual_adapter, source_symbol,
                                    native_name + "::" + sanitize_identifier(function.name),
                                    function.span);
            source << virtual_return_type(*engine_virtual) << ' ' << native_name
                   << "::" << sanitize_identifier(function.name) << '(';
            for (std::size_t index = 0; index < engine_virtual->maximum_arguments; ++index) {
                if (index != 0)
                    source << ", ";
                source << "[[maybe_unused]] " << virtual_parameter_type(*engine_virtual, index)
                       << " _gdpp_engine_argument_" << index;
            }
            source << ')';
            if (engine_virtual->is_const)
                source << " const";
            source << " {\n    ";
            const bool returns_value = !std::string_view{engine_virtual->return_type}.empty() &&
                                       std::string_view{engine_virtual->return_type} != "void";
            if (returns_value)
                source << "return ";
            std::string call =
                engine_virtual->is_const ? "const_cast<" + native_name + "*>(this)->" : "this->";
            call += native_function_name + "(";
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0)
                    call += ", ";
                const auto& parameter = function.parameters[index];
                if (index >= engine_virtual->maximum_arguments) {
                    call += "gdpp::runtime::default_argument()";
                    continue;
                }
                const auto argument_name = "_gdpp_engine_argument_" + std::to_string(index);
                if (parameter.default_value) {
                    call += "gdpp::runtime::to_variant(" + argument_name + ")";
                } else if (const auto* argument = api_.argument(*engine_virtual, index)) {
                    call += emit_conversion(parameter.type, type_from_godot_api(argument->type),
                                            argument_name, &parameter.span);
                }
            }
            if (function.rest_parameter) {
                if (!function.parameters.empty())
                    call += ", ";
                call += "godot::Array()";
            }
            call += ")";
            source << (returns_value ? emit_api_argument(engine_virtual->return_type,
                                                         engine_virtual->return_meta,
                                                         function.return_type, std::move(call))
                                     : call)
                   << ";\n}\n\n";
        }
        if (function.is_abstract) {
            record_generated_symbol(GeneratedSymbolKind::method, source_symbol,
                                    native_name + "::" + native_function_name, function.span);
            source << function_return_type(function) << ' ' << native_name
                   << "::" << native_function_name << '(';
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0)
                    source << ", ";
                const auto& parameter = function.parameters[index];
                source << "[[maybe_unused]] " << parameter_native_type(parameter) << ' '
                       << parameter_native_name(parameter);
            }
            if (function.rest_parameter) {
                if (!function.parameters.empty())
                    source << ", ";
                source << "[[maybe_unused]] godot::Array "
                       << sanitize_identifier(function.rest_parameter->name);
            }
            source << ") {\n"
                   << "    gdpp::runtime::ScriptFunctionScope _gdpp_abstract_scope("
                      "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
                   << "    gdpp::runtime::report_script_failure("
                   << godot_string("Cannot call abstract function '" + function.name + "'.")
                   << ", _gdpp_source_path, " << function.span.begin.line << ", "
                   << function.span.begin.column << ");\n";
            if (function.return_type.kind != TypeKind::void_type)
                source << "    return {};\n";
            source << "}\n\n";
            in_function_body_ = false;
            current_coroutine_abi_ = false;
            current_coroutine_state_.clear();
            current_static_context_ = false;
            continue;
        }
        record_generated_symbol(GeneratedSymbolKind::method, source_symbol,
                                native_name + "::" + native_function_name, function.span);
        source << function_return_type(function) << ' ' << native_name
               << "::" << native_function_name << '(';
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0)
                source << ", ";
            const auto& parameter = function.parameters[index];
            source << "[[maybe_unused]] " << parameter_native_type(parameter) << ' '
                   << parameter_native_name(parameter);
        }
        if (function.rest_parameter) {
            if (!function.parameters.empty())
                source << ", ";
            source << "[[maybe_unused]] godot::Array "
                   << sanitize_identifier(function.rest_parameter->name);
        }
        source << ')';
        source << " {\n";
        if (current_coroutine_abi_) {
            source << "    const auto " << current_coroutine_state_
                   << " = gdpp::runtime::begin_coroutine("
                   << (function.is_static ? "nullptr" : self_object_expression()) << ");\n";
        }
        source << emit_script_function_scope(1, function.name == "_static_init");
        if (function.is_static && function.name != "_static_init" && has_static_initialization) {
            source << "    (void)_gdpp_ensure_static_initialized();\n";
            source << emit_script_failure_return(1, false);
        }
        source << emit_debug_frame(function.span.begin.line, 1);
        if (has_parameter_control_flow(function.parameters)) {
            const auto saved_overrides = local_expression_overrides_;
            source << emit_parameter_cells(function.parameters, 1);
            source << emit_parameter_initialization_chain(
                function.parameters, function.rest_parameter, function.body, {}, 1, 0, false);
            local_expression_overrides_ = saved_overrides;
        } else {
            source << emit_parameter_default_initializers(function.parameters, 1);
            source << emit_statements(function.body, 1);
        }
        if (!current_coroutine_abi_ && function.return_type.kind != TypeKind::void_type &&
            (requires_native_fallback(function.body) ||
             (function.return_type.is_dynamic() && native_statements_fall_through(function.body))))
            source << "    return {};\n";
        source << "}\n\n";
        in_function_body_ = false;
        current_coroutine_abi_ = false;
        current_coroutine_state_.clear();
        current_static_context_ = false;
    }
    for (const auto& function : declaration.functions) {
        if (function.name == "_static_init" || engine_virtual_for(function)) {
            continue;
        }
        record_generated_symbol(GeneratedSymbolKind::variant_callback,
                                source_name + "::" + function.name,
                                native_name + "::" + method_callback_name(function), function.span);
        source << emit_method_callback_definition(
            function, native_name, function_native_name(function), function_return_type(function));
    }
    emit_attached_descriptor_definition(
        source, native_name, "res://" + current_source_path_ + "::" + source_name, {},
        native_base_type, base_script_path, current_script_contract_hash_, tool_mode,
        declaration.is_abstract, declaration.fields, declaration.functions, declaration.signals,
        declaration.enums, declaration.classes);
    local_function_parameters_ = previous_function_parameters;
    local_functions_ = previous_functions;
    local_function_native_names_ = previous_function_native_names;
    current_native_class_name_ = previous_native_class_name;
    current_godot_base_type_ = previous_godot_base_type;
    attached_godot_base_type_ = previous_attached_godot_base_type;
    current_debug_source_ = previous_debug_source;
    current_debug_function_ = previous_debug_function;
    current_debug_instance_ = previous_debug_instance;
    current_debug_members_ = previous_debug_members;
    current_inner_script_ = previous_inner_script;
}

GeneratedUnit CodeGenerator::generate(const mir::Module& mir_module, const std::string& source_path,
                                      const std::string& native_class_suffix,
                                      const std::string& native_base_class,
                                      const std::string& native_base_header,
                                      const bool attached_script,
                                      const std::string& attached_native_base,
                                      const std::string& attached_base_script_path,
                                      const std::string& script_contract_hash) const {
    const auto& module = mir_module.program;
    const std::filesystem::path path{source_path};
    const auto base_name = path.stem().string();
    GeneratedUnit unit;
    generated_symbols_.clear();
    current_static_context_ = false;
    in_async_continuation_ = false;
    unit.icon_path = module.icon_path;
    unit.is_abstract = module.is_abstract;
    unit.is_tool = module.is_tool;
    unit.static_unload = module.static_unload;
    unit.is_attached = attached_script;
    unit.attached_native_base = attached_native_base;
    match_counter_ = 0;
    temporary_counter_ = 0;
    current_source_path_ = source_path;
    current_debug_source_ = "res://" + source_path;
    current_debug_function_.clear();
    current_debug_instance_.clear();
    current_debug_members_.clear();
    for (const auto& field : module.fields) {
        if (!field.is_constant && !field.is_static) {
            current_debug_members_.emplace_back(field.name,
                                                "this->" + sanitize_identifier(field.name));
        }
    }
    current_return_type_ = {TypeKind::void_type, "void"};
    in_function_body_ = false;
    attached_script_ = attached_script;
    attached_godot_base_type_.clear();
    current_script_contract_hash_ = script_contract_hash;
    current_script_ = script_symbols_ ? script_symbols_->find_path(source_path) : nullptr;
    if (script_symbols_ && current_script_) {
        std::unordered_set<std::string> debug_member_names;
        for (const auto& [name, expression] : current_debug_members_) {
            static_cast<void>(expression);
            debug_member_names.insert(name);
        }
        for (const auto* member : script_symbols_->inherited_members(*current_script_)) {
            if (member->kind == ScriptMemberKind::field && !member->is_static &&
                debug_member_names.insert(member->name).second) {
                current_debug_members_.emplace_back(member->name,
                                                    "this->" + sanitize_identifier(member->name));
            }
        }
    }
    unit.script_class_name = current_script_
                                 ? current_script_->script_name
                                 : module.class_name.value_or(to_pascal_case(base_name));
    unit.script_class_name = sanitize_identifier(unit.script_class_name);
    unit.class_name = current_script_
                          ? current_script_->native_class_name
                          : "GDPPNative_" + unit.script_class_name + native_class_suffix;
    current_native_class_name_ = unit.class_name;
    inner_native_names_.clear();
    inner_godot_base_types_.clear();
    inner_attached_native_base_types_.clear();
    inner_base_names_.clear();
    inner_declarations_.clear();
    inner_ref_types_.clear();
    container_enum_types_.clear();
    local_function_parameters_.clear();
    local_functions_.clear();
    local_function_native_names_.clear();
    constructor_functions_.clear();
    for (const auto& function : module.functions) {
        local_functions_.emplace(function.name, &function);
        auto& parameters = local_function_parameters_[function.name];
        for (const auto& parameter : function.parameters)
            parameters.push_back(parameter.type);
    }
    if (const auto initializer =
            std::find_if(module.functions.begin(), module.functions.end(),
                         [](const auto& function) { return function.name == "_init"; });
        initializer != module.functions.end()) {
        constructor_functions_.emplace(unit.class_name, &*initializer);
    }
    for (const auto& enumeration : module.enums) {
        if (!enumeration.name.empty())
            container_enum_types_.insert(enumeration.name);
    }
    std::vector<std::pair<std::string, const typed::Class*>> named_inner_classes;
    const auto collect_inner_classes = [&](const auto& self, const auto& declarations,
                                           const std::string& parent) -> void {
        for (const auto& declaration : declarations) {
            const auto qualified =
                parent.empty() ? declaration.name : parent + "." + declaration.name;
            named_inner_classes.emplace_back(qualified, &declaration);
            self(self, declaration.classes, qualified);
        }
    };
    collect_inner_classes(collect_inner_classes, module.classes, "");
    const auto find_named_inner = [&](std::string_view name) -> const typed::Class* {
        const auto found = std::find_if(named_inner_classes.begin(), named_inner_classes.end(),
                                        [&](const auto& value) { return value.first == name; });
        return found == named_inner_classes.end() ? nullptr : found->second;
    };
    const auto resolve_inner_base = [&](const std::string& owner,
                                        const std::string& base) -> std::string {
        if (find_named_inner(base))
            return base;
        const auto separator = owner.rfind('.');
        if (separator != std::string::npos) {
            const auto lexical = owner.substr(0, separator + 1) + base;
            if (find_named_inner(lexical))
                return lexical;
        }
        std::string unique;
        for (const auto& [qualified, declaration] : named_inner_classes) {
            static_cast<void>(declaration);
            const auto leaf_separator = qualified.rfind('.');
            const auto leaf = leaf_separator == std::string::npos
                                  ? qualified
                                  : qualified.substr(leaf_separator + 1);
            if (leaf != base)
                continue;
            if (!unique.empty())
                return {};
            unique = qualified;
        }
        return unique;
    };
    for (const auto& [qualified, declaration] : named_inner_classes) {
        std::string native_name = unit.class_name;
        std::size_t begin = 0;
        while (begin <= qualified.size()) {
            const auto separator = qualified.find('.', begin);
            const auto end = separator == std::string::npos ? qualified.size() : separator;
            native_name += "__" + sanitize_identifier(qualified.substr(begin, end - begin));
            if (separator == std::string::npos)
                break;
            begin = separator + 1;
        }
        if (script_symbols_ && current_script_) {
            if (const auto* symbol = script_symbols_->find_inner(*current_script_, qualified);
                symbol && !symbol->native_class_name.empty()) {
                native_name = symbol->native_class_name;
            }
        }
        inner_native_names_.emplace(qualified, native_name);
        inner_declarations_.emplace(qualified, declaration);
        if (const auto initializer =
                std::find_if(declaration->functions.begin(), declaration->functions.end(),
                             [](const auto& function) { return function.name == "_init"; });
            initializer != declaration->functions.end()) {
            constructor_functions_.emplace(qualified, &*initializer);
            constructor_functions_.emplace(native_name, &*initializer);
        }
        if (!api_.find_class(declaration->base_type)) {
            std::string resolved;
            if (script_symbols_ && current_script_) {
                if (const auto* symbol = script_symbols_->find_inner(*current_script_, qualified)) {
                    if (const auto* base = script_symbols_->inner_base_of(*symbol))
                        resolved = base->native_class_name;
                }
            }
            if (resolved.empty())
                resolved = resolve_inner_base(qualified, declaration->base_type);
            if (!resolved.empty())
                inner_base_names_.emplace(qualified, std::move(resolved));
        }
        inner_ref_types_.insert(qualified);
    }
    for (const auto& [qualified, declaration] : named_inner_classes) {
        if (script_symbols_ && current_script_) {
            if (const auto* symbol = script_symbols_->find_inner(*current_script_, qualified);
                symbol && !symbol->godot_base_type.empty()) {
                inner_godot_base_types_.emplace(qualified, symbol->godot_base_type);
                inner_attached_native_base_types_.emplace(
                    qualified, symbol->attached_native_base.empty() ? symbol->godot_base_type
                                                                    : symbol->attached_native_base);
                continue;
            }
        }
        const typed::Class* current = declaration;
        std::string current_name = qualified;
        std::unordered_set<std::string> visited{qualified};
        while (!api_.find_class(current->base_type)) {
            const auto base = inner_base_names_.find(current_name);
            if (base == inner_base_names_.end() || !visited.insert(base->second).second)
                break;
            current_name = base->second;
            current = find_named_inner(current_name);
            if (!current)
                break;
        }
        inner_godot_base_types_.emplace(qualified, api_.find_class(current->base_type)
                                                       ? current->base_type
                                                       : std::string{"RefCounted"});
        inner_attached_native_base_types_.emplace(qualified, api_.find_class(current->base_type)
                                                                 ? current->base_type
                                                                 : std::string{"RefCounted"});
    }
    std::vector<std::pair<std::string, const typed::Class*>> ordered_inner_classes;
    std::unordered_set<std::string> ordered_inner_names;
    std::unordered_set<std::string> visiting_inner_names;
    const auto order_inner = [&](const auto& self, const std::string& qualified,
                                 const typed::Class& declaration) -> void {
        if (ordered_inner_names.find(qualified) != ordered_inner_names.end() ||
            !visiting_inner_names.insert(qualified).second) {
            return;
        }
        if (const auto base = inner_base_names_.find(qualified); base != inner_base_names_.end()) {
            if (const auto* base_declaration = find_named_inner(base->second))
                self(self, base->second, *base_declaration);
        }
        visiting_inner_names.erase(qualified);
        if (ordered_inner_names.insert(qualified).second)
            ordered_inner_classes.emplace_back(qualified, &declaration);
    };
    for (const auto& [qualified, declaration] : named_inner_classes)
        order_inner(order_inner, qualified, *declaration);
    for (const auto& [qualified, declaration] : ordered_inner_classes) {
        const auto& native_name = inner_native_names_.at(qualified);
        unit.inner_class_names.push_back(native_name);
        if (declaration->is_abstract)
            unit.abstract_inner_class_names.push_back(native_name);
    }
    const auto file_stem =
        current_script_
            ? std::filesystem::path{current_script_->header_file_name}.stem().stem().string()
            : to_snake_case(unit.script_class_name);
    detail_namespace_ = file_stem + "_gdpp_detail";
    unit.header_file_name =
        current_script_ ? current_script_->header_file_name : file_stem + ".gd.hpp";
    unit.source_file_name =
        current_script_
            ? std::filesystem::path{unit.header_file_name}.replace_extension(".cpp").string()
            : file_stem + ".gd.cpp";
    unit.symbol_file_name =
        std::filesystem::path{unit.source_file_name}.replace_extension(".symbols").string();
    auto base = module.base_type.value_or("RefCounted");
    if (native_base_class.empty() && !is_valid_base_type(base)) {
        diagnostics_.error("GDS3002",
                           "script-path inheritance requires semantic resolution and is not yet "
                           "supported",
                           module.span);
        base = "RefCounted";
    }
    const auto base_cpp = attached_script && native_base_class.empty()
                              ? std::string{"gdpp::runtime::AttachedScriptBehavior"}
                          : native_base_class.empty() ? "godot::" + godot_cpp_class_name(base)
                                                      : native_base_class;
    const auto initializer =
        std::find_if(module.functions.begin(), module.functions.end(),
                     [](const typed::Function& function) { return function.name == "_init"; });
    const bool has_instance_initializers =
        std::any_of(module.fields.begin(), module.fields.end(), [](const typed::Field& field) {
            return !field.is_constant && !field.is_static && !field.onready && field.initializer;
        });
    const bool has_static_initialization =
        requires_static_initialization(module.fields, module.functions);
    const bool is_autoload = current_script_ && !current_script_->autoload_name.empty();
    const auto ready =
        std::find_if(module.functions.begin(), module.functions.end(),
                     [](const typed::Function& function) { return function.name == "_ready"; });
    const bool has_onready_fields =
        std::any_of(module.fields.begin(), module.fields.end(),
                    [](const typed::Field& field) { return field.onready; });
    const auto godot_base_type = current_script_ ? current_script_->godot_base_type : base;
    attached_godot_base_type_ = godot_base_type;
    const bool has_native_rpc =
        has_rpc_configuration(module.functions) &&
        (godot_base_type == "Node" || api_.inherits(godot_base_type, "Node"));
    const auto virtual_method_for = [&](const typed::Function& function) {
        if (function.is_static || attached_script)
            return static_cast<const GodotMethodRecord*>(nullptr);
        const auto* method = api_.find_method(godot_base_type, function.name);
        return method && method->is_virtual ? method : nullptr;
    };
    const auto coroutine_abi_for = [&](const typed::Function& function) {
        return function.is_coroutine;
    };
    const auto function_return_type = [&](const typed::Function& function) {
        return coroutine_abi_for(function) ? std::string{"godot::Variant"}
                                           : cpp_type(function.return_type);
    };
    const auto overrides_script_method = [&](const typed::Function& function) {
        if (function.is_static || function.name == "_init" || !script_symbols_ ||
            !current_script_) {
            return false;
        }
        if (attached_script) {
            const auto local_inner_prefix = "res://" + current_source_path_ + "::";
            if (attached_base_script_path.rfind(local_inner_prefix, 0) == 0) {
                const auto local_base = attached_base_script_path.substr(local_inner_prefix.size());
                std::string declaration_owner;
                const auto* inherited =
                    find_inherited_inner_function(local_base, function.name, &declaration_owner);
                return inherited && same_native_function_abi(function, *inherited);
            }
        }
        const auto declared = std::find_if(current_script_->members.begin(),
                                           current_script_->members.end(), [&](const auto& member) {
                                               return member.kind == ScriptMemberKind::function &&
                                                      member.name == function.name;
                                           });
        if (declared == current_script_->members.end() || declared->is_static)
            return false;
        auto* base_script = script_symbols_->base_of(*current_script_);
        while (base_script) {
            const auto inherited = std::find_if(
                base_script->members.begin(), base_script->members.end(), [&](const auto& member) {
                    return member.kind == ScriptMemberKind::function && !member.is_static &&
                           member.name == function.name;
                });
            if (inherited != base_script->members.end()) {
                return same_native_method_abi(*declared, *inherited) &&
                       script_method_implementation_name(*current_script_, *declared) ==
                           script_method_implementation_name(*base_script, *inherited);
            }
            base_script = script_symbols_->base_of(*base_script);
        }
        return false;
    };
    const auto function_native_name = [&](const typed::Function& function) {
        const auto isolated_source_name =
            "_gdpp_script_method_" + sanitize_identifier(function.name);
        if (!current_script_)
            return virtual_method_for(function)
                       ? "_gdpp_virtual_impl_" + sanitize_identifier(function.name)
                       : isolated_source_name;
        const auto member =
            std::find_if(current_script_->members.begin(), current_script_->members.end(),
                         [&](const auto& candidate) {
                             return candidate.kind == ScriptMemberKind::function &&
                                    candidate.name == function.name;
                         });
        if (member != current_script_->members.end())
            return script_method_implementation_name(*current_script_, *member);
        return virtual_method_for(function)
                   ? "_gdpp_virtual_impl_" + sanitize_identifier(function.name)
                   : isolated_source_name;
    };
    for (const auto& function : module.functions)
        local_function_native_names_.insert_or_assign(function.name,
                                                      function_native_name(function));
    const auto function_parameter_type = [&](const typed::Function& function,
                                             const std::size_t index) {
        return cpp_type(function.parameters[index].type);
    };

    current_godot_base_type_ = godot_base_type;

    std::ostringstream header;
    const auto native_types = collect_native_types(module, api_, script_symbols_);
    std::set<std::string> included_script_headers;
    header << "// Generated by GDPP. Do not edit.\n"
           << "#pragma once\n\n"
           << "#include <gdpp/runtime/attached_script.hpp>\n";
    if (attached_script)
        header << "#include <" << header_for_base(godot_base_type) << ">\n";
    if (!attached_script && native_base_class.empty())
        header << "#include <" << header_for_base(base) << ">\n";
    if (!native_base_header.empty()) {
        header << "#include \"" << native_base_header << "\"\n";
        included_script_headers.insert(native_base_header);
    }
    if (script_symbols_ && current_script_) {
        for (const auto& inner : current_script_->inner_classes) {
            const auto* script_base = script_symbols_->base_of(inner);
            if (!script_base) {
                if (const auto* inner_base = script_symbols_->inner_base_of(inner))
                    script_base = script_symbols_->owner_of(*inner_base);
            }
            if (script_base && script_base != current_script_ &&
                included_script_headers.insert(script_base->header_file_name).second) {
                header << "#include \"" << script_base->header_file_name << "\"\n";
            }
        }
    }
    for (const auto& type : native_types.builtins) {
        header << "#include <godot_cpp/variant/" << to_snake_case(type) << ".hpp>\n";
    }
    for (const auto& type : native_types.objects) {
        header << "#include <" << header_for_base(type) << ">\n";
    }
    if (native_types.typed_array)
        header << "#include <godot_cpp/variant/typed_array.hpp>\n";
    if (native_types.typed_dictionary)
        header << "#include <godot_cpp/variant/typed_dictionary.hpp>\n";
    if (script_symbols_) {
        for (const auto& type : native_types.complete_scripts) {
            const auto* symbol = script_symbols_->find_global(type);
            if (symbol && symbol != current_script_ &&
                included_script_headers.insert(symbol->header_file_name).second)
                header << "#include \"" << symbol->header_file_name << "\"\n";
        }
        for (const auto& resource_path : native_types.complete_script_resources) {
            const auto* symbol = script_symbols_->find_path(resource_path);
            if (symbol && symbol != current_script_ &&
                included_script_headers.insert(symbol->header_file_name).second)
                header << "#include \"" << symbol->header_file_name << "\"\n";
        }
        for (const auto& type : native_types.scripts) {
            const auto* symbol = script_symbols_->find_global(type);
            if (symbol && symbol != current_script_ &&
                included_script_headers.find(symbol->header_file_name) ==
                    included_script_headers.end() &&
                native_types.complete_scripts.find(type) == native_types.complete_scripts.end())
                header << "class " << symbol->native_class_name << ";\n";
        }
        for (const auto& resource_path : native_types.script_resources) {
            const auto* symbol = script_symbols_->find_path(resource_path);
            if (symbol && symbol != current_script_ &&
                included_script_headers.find(symbol->header_file_name) ==
                    included_script_headers.end() &&
                native_types.complete_script_resources.find(resource_path) ==
                    native_types.complete_script_resources.end())
                header << "class " << symbol->native_class_name << ";\n";
        }
    }
    // ScriptResource selects pointer ownership from the actual generated base at compile time,
    // so every generated unit needs the Ref/RefCounted definitions even when its local AST does
    // not directly spell a ref-counted type.
    header << "#include <godot_cpp/classes/ref.hpp>\n"
           << "#include <godot_cpp/classes/ref_counted.hpp>\n"
           << "#include <godot_cpp/classes/script.hpp>\n"
           << "#include <godot_cpp/core/class_db.hpp>\n"
           << "#include <godot_cpp/core/error_macros.hpp>\n"
           << "#include <godot_cpp/core/math_defs.hpp>\n"
           << "#include <godot_cpp/core/memory.hpp>\n"
           << "#include <godot_cpp/core/object.hpp>\n"
           << "#include <godot_cpp/core/type_info.hpp>\n"
           << "#include <godot_cpp/variant/array.hpp>\n"
           << "#include <godot_cpp/variant/dictionary.hpp>\n"
           << "#include <godot_cpp/variant/utility_functions.hpp>\n"
           << "#include <godot_cpp/variant/variant.hpp>\n\n"
           << "#include <gdpp/runtime/variant_ops.hpp>\n"
           << "#include <gdpp/numeric/integer_semantics.hpp>\n\n"
           << "#include <cstdint>\n"
           << "#include <atomic>\n"
           << "#include <functional>\n"
           << "#include <initializer_list>\n"
           << "#include <memory>\n"
           << "#include <mutex>\n"
           << "#include <type_traits>\n"
           << "#include <utility>\n\n";
    // Inner classes are flattened into namespace-scope native classes. Their source ordering is
    // constrained by inheritance, but fields and signatures may refer to any sibling or nested
    // class declared later in the GDScript file. Declare the complete class family before emitting
    // definitions so those references remain valid C++ on every compiler.
    for (const auto& [qualified, declaration] : ordered_inner_classes) {
        static_cast<void>(declaration);
        header << "class " << inner_native_names_.at(qualified) << ";\n";
    }
    if (!ordered_inner_classes.empty())
        header << '\n';
    std::set<std::string> emitted_container_object_tags;
    for (const auto& type_name : native_types.container_objects) {
        const auto identity = container_object_tag_identity(type_name);
        if (!emitted_container_object_tags.insert(identity).second)
            continue;
        const auto identifier = sanitize_identifier(identity);
        header << "#ifndef GDPP_RUNTIME_CONTAINER_OBJECT_TAG_" << identifier << "\n"
               << "#define GDPP_RUNTIME_CONTAINER_OBJECT_TAG_" << identifier << "\n"
               << "namespace gdpp::runtime::container_tags {\n"
               << "struct ContainerObjectTag_" << identifier << " {\n"
               << "    static godot::StringName get_class_static() { return "
               << godot_string_name(container_object_runtime_name(identity)) << "; }\n"
               << "    inline static constexpr const char *_gdpp_attached_script_path = "
               << escaped_string(
                      attached_script_source_path({TypeKind::object, std::string{identity}}))
               << ";\n"
               << "};\n"
               << "} // namespace gdpp::runtime::container_tags\n"
               << "#endif\n";
    }
    if (!emitted_container_object_tags.empty())
        header << '\n';
    header << "namespace " << detail_namespace_ << " {\n"
           << "template <typename T> struct InternalClassResource {\n"
           << "    template <typename... Args>\n"
           << "    static auto instantiate(Args &&...args) {\n"
           << "        godot::Array gdpp_arguments;\n"
           << "        (gdpp_arguments.push_back("
              "gdpp::runtime::to_variant(std::forward<Args>(args))), ...);\n"
           << "        godot::Variant instance = gdpp::runtime::instantiate_attached_script("
              "godot::String(T::_gdpp_source_path), gdpp_arguments);\n"
           << "        if constexpr (T::_gdpp_attached_ref_counted) {\n"
           << "            godot::Object *object = instance;\n"
           << "            return godot::Ref<godot::RefCounted>("
              "godot::Object::cast_to<godot::RefCounted>(object));\n"
           << "        } else {\n"
           << "            return static_cast<godot::Object *>(instance);\n"
           << "        }\n"
           << "    }\n"
           << "};\n"
           << "inline godot::Dictionary make_dictionary(\n"
           << "    std::initializer_list<std::pair<godot::Variant, godot::Variant>> values) {\n"
           << "    godot::Dictionary result;\n"
           << "    for (const auto &entry : values) result[entry.first] = entry.second;\n"
           << "    return result;\n"
           << "}\n"
           << "} // namespace " << detail_namespace_ << "\n\n";
    const auto root_attached_script = attached_script_;
    attached_script_ = true;
    for (const auto& [qualified, declaration] : ordered_inner_classes) {
        emit_inner_class_declaration(*declaration, header, inner_native_names_.at(qualified),
                                     qualified, module.is_tool);
    }
    attached_script_ = root_attached_script;
    header << "class " << unit.class_name << " : public " << base_cpp << " {\n"
           << "    GDCLASS(" << unit.class_name << ", " << base_cpp << ")\n\n"
           << "public:\n"
           << "    inline static constexpr bool _gdpp_tool_mode = "
           << (module.is_tool ? "true" : "false") << ";\n"
           << "    inline static constexpr bool _gdpp_static_unload = "
           << (module.static_unload ? "true" : "false") << ";\n"
           << "    inline static constexpr bool _gdpp_attached = "
           << (attached_script ? "true" : "false") << ";\n"
           << "    inline static constexpr bool _gdpp_attached_ref_counted = "
           << (attached_script && api_.inherits(godot_base_type, "RefCounted") ? "true" : "false")
           << ";\n"
           << "    inline static constexpr const char *_gdpp_source_path = "
           << escaped_string("res://" + current_source_path_) << ";\n";
    for (const auto& enumeration : module.enums) {
        if (enumeration.name.empty()) {
            for (const auto& entry : enumeration.entries) {
                header << "    inline static constexpr int64_t " << enum_identifier(entry.name)
                       << " = " << entry.value << ";\n";
            }
        } else {
            emit_named_enum_declaration(enumeration, header, 4);
        }
    }
    if (!module.enums.empty())
        header << '\n';
    for (const auto& variable : module.fields) {
        if (!variable.is_constant)
            continue;
        header << "    static const " << cpp_type(variable.type);
        if (managed_constant_field(variable))
            header << "& " << sanitize_identifier(variable.name) << "();\n";
        else
            header << ' ' << sanitize_identifier(variable.name) << ";\n";
    }
    if (std::any_of(module.fields.begin(), module.fields.end(),
                    [](const typed::Field& field) { return field.is_constant; }))
        header << '\n';
    if (attached_script) {
        header << "    " << unit.class_name << "();\n"
               << "    void _gdpp_initialize_instance() override;\n"
               << "    void _gdpp_initialize_onready() override;\n"
               << "    void _gdpp_dispatch_notification(std::int32_t what, bool reversed) "
                  "override;\n\n";
    } else if (initializer != module.functions.end()) {
        const auto required = std::count_if(
            initializer->parameters.begin(), initializer->parameters.end(),
            [](const typed::Parameter& parameter) { return !parameter.default_value; });
        if (required != 0 || initializer->rest_parameter)
            header << "    " << unit.class_name
                   << (has_instance_initializers || has_static_initialization || is_autoload ||
                               has_native_rpc || (initializer->rest_parameter && required == 0)
                           ? "();\n"
                           : "() = default;\n");
        header << "    " << unit.class_name << '(';
        for (std::size_t index = 0; index < initializer->parameters.size(); ++index) {
            if (index != 0)
                header << ", ";
            const auto& parameter = initializer->parameters[index];
            header << parameter_native_type(parameter) << ' ' << parameter_native_name(parameter);
            if (parameter.default_value && !initializer->rest_parameter)
                header << " = " << emit_parameter_default(parameter);
        }
        if (initializer->rest_parameter) {
            if (!initializer->parameters.empty())
                header << ", ";
            header << "godot::Array " << sanitize_identifier(initializer->rest_parameter->name);
        }
        header << ");\n\n";
    } else if (has_instance_initializers || has_static_initialization || is_autoload ||
               has_native_rpc) {
        header << "    " << unit.class_name << "();\n\n";
    }
    header << "protected:\n"
           << "    static void _bind_methods();\n\n";
    for (const auto& function : module.functions) {
        if (function.name != "_static_init" && (attached_script || function.name != "_init") &&
            !virtual_method_for(function)) {
            header << emit_method_callback_declaration(function);
        }
    }
    if (std::any_of(module.functions.begin(), module.functions.end(), [&](const auto& function) {
            return function.name != "_static_init" &&
                   (attached_script || function.name != "_init") && !virtual_method_for(function);
        })) {
        header << '\n';
    }
    if (std::none_of(module.fields.begin(), module.fields.end(),
                     [](const typed::Field& field) { return !field.is_constant; }))
        header << "    // No script fields.\n";
    for (const auto& variable : module.fields) {
        if (variable.is_constant)
            continue;
        header << "    ";
        if (variable.is_static) {
            const auto name = sanitize_identifier(variable.name);
            const auto type = cpp_type(variable.type);
            header << "static std::atomic<" << type << "*>& _gdpp_static_" << name
                   << "_pointer();\n"
                   << "    static std::mutex& _gdpp_static_" << name << "_mutex();\n"
                   << "    static " << type << "& _gdpp_static_" << name << "_storage();\n"
                   << "    static void _gdpp_static_" << name << "_release();\n";
        } else {
            header << cpp_type(variable.type) << ' ' << sanitize_identifier(variable.name)
                   << "{};\n";
        }
    }
    for (const auto& variable : module.fields) {
        if (cached_preload_field(variable) && !variable.is_static) {
            const auto name = sanitize_identifier(variable.name);
            const auto type = cpp_type(variable.type);
            header << "    static std::atomic<" << type << "*>& _gdpp_preloaded_" << name
                   << "_pointer();\n"
                   << "    static std::mutex& _gdpp_preloaded_" << name << "_mutex();\n"
                   << "    static " << type << "& _gdpp_preloaded_" << name << "();\n"
                   << "    static void _gdpp_preloaded_" << name << "_release();\n";
        }
    }
    for (const auto& variable : module.fields) {
        if (!managed_constant_field(variable))
            continue;
        const auto name = sanitize_identifier(variable.name);
        const auto type = cpp_type(variable.type);
        header << "    static std::atomic<" << type << "*>& _gdpp_constant_" << name
               << "_pointer();\n"
               << "    static " << type << "& _gdpp_constant_" << name
               << "_storage();\n"
               << "    static bool& _gdpp_constant_" << name << "_ready();\n"
               << "    static std::mutex& _gdpp_constant_" << name << "_mutex();\n"
               << "    static void _gdpp_constant_" << name << "_release();\n";
    }
    if (has_static_initialization) {
        header << "    static gdpp::runtime::ScriptInitializationState& "
                  "_gdpp_static_initialization();\n"
               << "    static bool _gdpp_ensure_static_initialized();\n";
    }
    if (std::any_of(module.fields.begin(), module.fields.end(), cached_preload_field)) {
        header << "    static gdpp::runtime::ScriptInitializationState& "
                  "_gdpp_preload_initialization();\n";
    }
    header << "\npublic:\n"
           << "    static void _gdpp_preload_resources();\n"
           << "    static void _gdpp_release_preloaded_resources();\n";
    if (attached_script)
        header << "    static gdpp::runtime::AttachedScriptDescriptor _gdpp_descriptor();\n";
    for (const auto& variable : module.fields) {
        if (variable.is_constant)
            continue;
        const auto name = sanitize_identifier(variable.name);
        const auto setter_parameter = variable.setter && variable.setter->method.empty()
                                          ? sanitize_identifier(variable.setter->parameter)
                                          : "value";
        const auto getter_type = variable.getter && variable.getter->is_coroutine
                                     ? std::string{"godot::Variant"}
                                     : cpp_type(variable.type);
        header << "    " << (variable.is_static ? "static " : "") << getter_type << " _gdpp_get_"
               << name << "();\n"
               << "    " << (variable.is_static ? "static " : "") << "void _gdpp_set_" << name
               << "(" << cpp_type(variable.type) << ' ' << setter_parameter << ");\n";
    }
    if (std::any_of(module.fields.begin(), module.fields.end(),
                    [](const typed::Field& field) { return !field.is_constant; })) {
        header << '\n';
    }
    for (const auto& function : module.functions) {
        if (const auto* method = virtual_method_for(function)) {
            header << "    virtual " << virtual_return_type(*method) << ' '
                   << sanitize_identifier(function.name) << '(';
            for (std::size_t index = 0; index < method->maximum_arguments; ++index) {
                if (index != 0)
                    header << ", ";
                header << virtual_parameter_type(*method, index) << " _gdpp_engine_argument_"
                       << index;
            }
            header << ')';
            if (method->is_const)
                header << " const";
            header << " override;\n";
        }
        header << "    ";
        if (function.is_static)
            header << "static ";
        else if (function.name != "_init")
            header << "virtual ";
        header << function_return_type(function) << ' ' << function_native_name(function) << '(';
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0)
                header << ", ";
            const auto& parameter = function.parameters[index];
            header << (parameter.default_value ? parameter_native_type(parameter)
                                               : function_parameter_type(function, index))
                   << ' ' << parameter_native_name(parameter);
            if (parameter.default_value && !function.rest_parameter)
                header << " = " << emit_parameter_default(parameter);
        }
        if (function.rest_parameter) {
            if (!function.parameters.empty())
                header << ", ";
            header << "godot::Array " << sanitize_identifier(function.rest_parameter->name);
        }
        header << ')';
        if (overrides_script_method(function))
            header << " override";
        header << ";\n";
    }
    if (!attached_script && has_onready_fields && ready == module.functions.end()) {
        header << "    virtual void _ready()";
        const auto* method = api_.find_method(godot_base_type, "_ready");
        if (method && method->is_virtual)
            header << " override";
        header << ";\n";
    }
    header << "};\n";
    unit.header = header.str();

    std::ostringstream source;
    source << "// Generated by GDPP. Do not edit.\n"
           << "// GDScript permits lexical shadowing. Keep that source-language contract while\n"
           << "// retaining every other warning as an error in generated native builds.\n"
           << "#if defined(__clang__)\n"
           << "#pragma clang diagnostic push\n"
           << "#pragma clang diagnostic ignored \"-Wshadow\"\n"
           << "#elif defined(__GNUC__)\n"
           << "#pragma GCC diagnostic push\n"
           << "#pragma GCC diagnostic ignored \"-Wshadow\"\n"
           << "#elif defined(_MSC_VER)\n"
           << "#pragma warning(push)\n"
           << "#pragma warning(disable : 4456 4457 4458 4459)\n"
           << "#endif\n"
           << "#include \"" << unit.header_file_name << "\"\n";
    const bool has_any_rpc = has_rpc_configuration(module) ||
                             std::any_of(ordered_inner_classes.begin(), ordered_inner_classes.end(),
                                         [](const auto& entry) {
                                             return has_rpc_configuration(entry.second->functions);
                                         });
    if (has_any_rpc) {
        source << "#include <godot_cpp/classes/multiplayer_api.hpp>\n"
               << "#include <godot_cpp/classes/multiplayer_peer.hpp>\n";
    }
    if (script_symbols_) {
        std::set<std::string> dependency_headers;
        for (const auto& native_name : native_types.resolved_native_scripts) {
            const auto* symbol = script_symbols_->find_native_class(native_name);
            if (symbol && symbol != current_script_)
                dependency_headers.insert(symbol->header_file_name);
        }
        for (const auto& type : native_types.scripts) {
            const auto* symbol = script_symbols_->find_global(type);
            if (symbol && symbol != current_script_)
                dependency_headers.insert(symbol->header_file_name);
        }
        for (const auto& resource_path : native_types.script_resources) {
            const auto* symbol = script_symbols_->find_path(resource_path);
            if (symbol && symbol != current_script_)
                dependency_headers.insert(symbol->header_file_name);
        }
        if (current_script_) {
            const auto add_complete_script_type = [&](const Type& type) {
                if (type.kind != TypeKind::object || api_.find_class(type.name))
                    return;
                const auto* symbol = script_symbols_->find_class(type.name);
                if (symbol && symbol != current_script_)
                    dependency_headers.insert(symbol->header_file_name);
            };
            // Super calls and checked downcasts may use a type declared only by an inherited
            // signature. Those types do not necessarily appear in the child AST, but C++
            // dynamic_cast requires a complete target in the generated source.
            for (const auto* member : script_symbols_->inherited_members(*current_script_)) {
                add_complete_script_type(member->type);
                for (const auto& parameter : member->parameters)
                    add_complete_script_type(parameter);
            }
        }
        for (const auto& header_file : dependency_headers)
            source << "#include \"" << header_file << "\"\n";
    }
    source << "\nusing gdpp::runtime::script_function_failed;\n\n";
    attached_script_ = true;
    for (const auto& [qualified, declaration] : ordered_inner_classes) {
        emit_inner_class_definition(*declaration, source, inner_native_names_.at(qualified),
                                    qualified, module.is_tool);
    }
    attached_script_ = root_attached_script;
    const bool has_static_initializer =
        std::any_of(module.functions.begin(), module.functions.end(),
                    [](const auto& function) { return function.name == "_static_init"; });
    if (has_static_initialization) {
        source << "gdpp::runtime::ScriptInitializationState& " << unit.class_name
               << "::_gdpp_static_initialization() {\n"
               << "    static gdpp::runtime::ScriptInitializationState state;\n"
               << "    return state;\n}\n\n"
               << "bool " << unit.class_name << "::_gdpp_ensure_static_initialized() {\n";
        if (!module.is_tool)
            source << "    if (gdpp::runtime::is_editor_hint()) return true;\n";
        source << "    return _gdpp_static_initialization().run(\n"
               << "        \"Static initialization previously failed for " << unit.class_name
               << ".\",\n"
               << "        []() {\n";
        for (const auto& variable : module.fields) {
            if (!variable.is_constant && variable.is_static) {
                source << "            (void)_gdpp_static_" << sanitize_identifier(variable.name)
                       << "_storage();\n"
                       << "            if (script_function_failed()) return;\n";
            }
        }
        if (has_static_initializer)
            source << "            _gdpp_script_method__static_init();\n";
        source << "        },\n"
               << "        []() {\n";
        for (const auto& variable : module.fields) {
            if (managed_constant_field(variable)) {
                const auto name = sanitize_identifier(variable.name);
                source << "            _gdpp_constant_" << name << "_release();\n";
            } else if (!variable.is_constant && variable.is_static) {
                source << "            _gdpp_static_" << sanitize_identifier(variable.name)
                       << "_release();\n";
            }
        }
        source << "        });\n"
               << "}\n\n";
    }
    for (const auto& variable : module.fields) {
        if (!variable.is_constant)
            continue;
        const auto name = sanitize_identifier(variable.name);
        if (!managed_constant_field(variable)) {
            source << "const " << cpp_type(variable.type) << ' ' << unit.class_name << "::" << name
                   << " = ";
            if (variable.initializer && !variable.onready) {
                source << emit_conversion(variable.type, emitted_type(*variable.initializer),
                                          emit_expression(*variable.initializer),
                                          &variable.initializer->span);
            } else {
                source << "{}";
            }
            source << ";\n";
            continue;
        }
        const auto type = cpp_type(variable.type);
        const auto storage = "_gdpp_constant_" + name + "_storage()";
        source << "std::atomic<" << type << "*>& " << unit.class_name << "::_gdpp_constant_" << name
               << "_pointer() {\n    static std::atomic<" << type
               << "*> value{nullptr};\n    return value;\n}\n\n"
               << type << "& " << unit.class_name << "::_gdpp_constant_" << name
               << "_storage() {\n"
               << "    auto* value = _gdpp_constant_" << name
               << "_pointer().load(std::memory_order_acquire);\n"
               << "    if (value) return *value;\n"
               << "    std::lock_guard<std::mutex> lock(_gdpp_constant_" << name << "_mutex());\n"
               << "    value = _gdpp_constant_" << name
               << "_pointer().load(std::memory_order_relaxed);\n"
               << "    if (!value) {\n"
               << "        value = new " << type
               << (variable.type.kind == TypeKind::script_resource ? "{" + type + "::missing()}"
                                                                   : "{}")
               << ";\n"
               << "        _gdpp_constant_" << name
               << "_pointer().store(value, std::memory_order_release);\n"
               << "    }\n"
               << "    return *value;\n}\n\n"
               << "bool& " << unit.class_name << "::_gdpp_constant_" << name
               << "_ready() {\n    static bool value = false;\n    return value;\n}\n\n"
               << "std::mutex& " << unit.class_name << "::_gdpp_constant_" << name
               << "_mutex() {\n    static std::mutex value;\n    return value;\n}\n\n"
               << "void " << unit.class_name << "::_gdpp_constant_" << name << "_release() {\n"
               << "    std::lock_guard<std::mutex> lock(_gdpp_constant_" << name << "_mutex());\n"
               << "    _gdpp_constant_" << name << "_ready() = false;\n"
               << "    delete _gdpp_constant_" << name
               << "_pointer().exchange(nullptr, std::memory_order_acq_rel);\n}\n\n"
               << "const " << type << "& " << unit.class_name << "::" << name << "() {\n"
               << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
               << (has_static_initialization ? "    (void)_gdpp_ensure_static_initialized();\n"
                                               "    if (script_function_failed()) return " +
                                                   storage + ";\n"
                                             : "")
               << "    auto& value = " << storage << ";\n"
               << "    std::lock_guard<std::mutex> lock(_gdpp_constant_" << name << "_mutex());\n"
               << "    if (!_gdpp_constant_" << name << "_ready()) {\n"
               << "        ";
        std::string constant_value;
        if (variable.initializer && !variable.onready) {
            constant_value = emit_conversion(variable.type, emitted_type(*variable.initializer),
                                             emit_expression(*variable.initializer),
                                             &variable.initializer->span);
        } else {
            constant_value = "{}";
        }
        source << emit_storage_assignment(variable.type, "value", std::move(constant_value))
               << ";\n"
               << "        if (script_function_failed()) return value;\n"
               << "        _gdpp_constant_" << name << "_ready() = true;\n"
               << "    }\n"
               << "    return value;\n}\n\n";
    }
    if (std::any_of(module.fields.begin(), module.fields.end(),
                    [](const typed::Field& field) { return field.is_constant; }))
        source << '\n';
    for (const auto& variable : module.fields) {
        if (!variable.is_constant && variable.is_static) {
            const auto name = sanitize_identifier(variable.name);
            const auto type = cpp_type(variable.type);
            source << "std::atomic<" << type << "*>& " << unit.class_name << "::_gdpp_static_"
                   << name << "_pointer() {\n    static std::atomic<" << type
                   << "*> value{nullptr};\n    return value;\n}\n\n"
                   << "std::mutex& " << unit.class_name << "::_gdpp_static_" << name
                   << "_mutex() {\n    static std::mutex value;\n    return value;\n}\n\n"
                   << type << "& " << unit.class_name << "::_gdpp_static_" << name
                   << "_storage() {\n"
                   << "    bool initialize = true;\n";
            if (!module.is_tool) {
                source << "    if (gdpp::runtime::is_editor_hint()) initialize = false;\n";
            }
            source << "    if (initialize && !_gdpp_ensure_static_initialized()) initialize = false;\n"
                   << "    auto* value = _gdpp_static_" << name
                   << "_pointer().load(std::memory_order_acquire);\n"
                   << "    if (value) return *value;\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_static_" << name << "_mutex());\n"
                   << "    value = _gdpp_static_" << name
                   << "_pointer().load(std::memory_order_relaxed);\n"
                   << "    if (!value) {\n"
                   << "        value = new " << type << "{};\n"
                   << "        _gdpp_static_" << name
                   << "_pointer().store(value, std::memory_order_release);\n"
                   << "        if (initialize) {\n";
            if (variable.initializer && !variable.onready) {
                source << "            "
                       << emit_storage_assignment(
                              variable.type, "*value",
                              emit_conversion(variable.type, emitted_type(*variable.initializer),
                                              emit_expression(*variable.initializer),
                                              &variable.initializer->span))
                       << ";\n";
            }
            source << "        }\n"
                   << "    }\n"
                   << "    return *value;\n}\n\n"
                   << "void " << unit.class_name << "::_gdpp_static_" << name << "_release() {\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_static_" << name << "_mutex());\n"
                   << "    delete _gdpp_static_" << name
                   << "_pointer().exchange(nullptr, std::memory_order_acq_rel);\n}\n\n";
        }
    }
    if (std::any_of(module.fields.begin(), module.fields.end(), [](const typed::Field& field) {
            return !field.is_constant && field.is_static;
        })) {
        source << '\n';
    }
    const auto emit_instance_initializers = [&]() {
        for (const auto& variable : module.fields) {
            if (!variable.is_constant && !variable.is_static && !variable.onready &&
                variable.initializer) {
                const bool editor_safe = editor_safe_initializer(*variable.initializer);
                if (!editor_safe && !module.is_tool)
                    source << "    if (!gdpp_editor_hint) {\n";
                std::string value;
                if (contains_preload(*variable.initializer)) {
                    value = "_gdpp_preloaded_" + sanitize_identifier(variable.name) + "()";
                } else {
                    value = emit_conversion(variable.type, emitted_type(*variable.initializer),
                                            emit_expression(*variable.initializer),
                                            &variable.initializer->span);
                }
                source << (editor_safe || module.is_tool ? "    " : "        ")
                       << emit_storage_assignment(variable.type, sanitize_identifier(variable.name),
                                                  std::move(value))
                       << ";\n"
                       << (editor_safe || module.is_tool ? "    " : "        ")
                       << "if (script_function_failed()) return;\n";
                if (!editor_safe && !module.is_tool)
                    source << "    }\n";
            }
        }
    };
    const auto required =
        initializer == module.functions.end()
            ? std::ptrdiff_t{0}
            : std::count_if(initializer->parameters.begin(), initializer->parameters.end(),
                            [](const auto& parameter) { return !parameter.default_value; });
    const bool needs_editor_hint =
        is_autoload ||
        (!module.is_tool &&
         (std::any_of(module.fields.begin(), module.fields.end(), cached_preload_field) ||
          std::any_of(module.fields.begin(), module.fields.end(), [](const auto& field) {
              return !field.is_constant && !field.is_static && !field.onready &&
                     field.initializer && !editor_safe_initializer(*field.initializer);
          })));
    const bool default_calls_initializer = !attached_script &&
                                           initializer != module.functions.end() &&
                                           initializer->rest_parameter && required == 0;
    const auto emit_autoload_registration = [&]() {
        if (is_autoload) {
            source << "    if (!gdpp_editor_hint) gdpp::runtime::register_autoload("
                   << godot_string_name(current_script_->autoload_name) << ", "
                   << self_object_expression() << ");\n";
        }
    };
    if (attached_script) {
        in_function_body_ = true;
        source << unit.class_name << "::" << unit.class_name << "() {\n"
               << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n";
        if (has_static_initialization) {
            source << "    (void)_gdpp_ensure_static_initialized();\n"
                   << "    if (script_function_failed()) return;\n";
        }
        if (std::any_of(module.fields.begin(), module.fields.end(), cached_preload_field)) {
            source << "    _gdpp_preload_resources();\n"
                   << "    if (script_function_failed()) return;\n";
        }
        source << "}\n\nvoid " << unit.class_name << "::_gdpp_initialize_instance() {\n"
               << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n";
        if (!native_base_class.empty()) {
            source << "    " << native_base_class << "::_gdpp_initialize_instance();\n"
                   << "    if (script_function_failed()) return;\n";
        }
        if (needs_editor_hint)
            source << "    const bool gdpp_editor_hint = gdpp::runtime::is_editor_hint();\n";
        emit_autoload_registration();
        emit_instance_initializers();
        source << "}\n\n"
               << "void " << unit.class_name << "::_gdpp_initialize_onready() {\n"
               << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
               << "    " << base_cpp << "::_gdpp_initialize_onready();\n"
               << "    if (script_function_failed()) return;\n";
        for (const auto& field : module.fields) {
            if (field.onready && field.initializer) {
                source << "    "
                       << emit_storage_assignment(
                              field.type, sanitize_identifier(field.name),
                              emit_conversion(field.type, emitted_type(*field.initializer),
                                              emit_expression(*field.initializer),
                                              &field.initializer->span))
                       << ";\n"
                       << "    if (script_function_failed()) return;\n";
            }
        }
        source << "}\n\n";

        const auto notification = std::find_if(
            module.functions.begin(), module.functions.end(), [](const typed::Function& function) {
                return !function.is_static && function.name == "_notification";
            });
        source << "void " << unit.class_name
               << "::_gdpp_dispatch_notification(std::int32_t what, bool reversed) {\n"
               << "    static_cast<void>(what);\n"
               << "    static_cast<void>(reversed);\n";
        if (!native_base_class.empty())
            source << "    if (reversed) " << native_base_class
                   << "::_gdpp_dispatch_notification(what, true);\n";
        if (notification != module.functions.end())
            source << "    " << function_native_name(*notification) << "(what);\n";
        if (!native_base_class.empty())
            source << "    if (!reversed) " << native_base_class
                   << "::_gdpp_dispatch_notification(what, false);\n";
        source << "}\n\n";
        in_function_body_ = false;
    } else if (((has_instance_initializers || has_static_initialization || is_autoload ||
                 has_native_rpc) &&
                (initializer == module.functions.end() || required != 0)) ||
               default_calls_initializer) {
        in_function_body_ = true;
        source << unit.class_name << "::" << unit.class_name << "() {\n"
               << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n";
        if (has_static_initialization) {
            source << "    (void)_gdpp_ensure_static_initialized();\n"
                   << "    if (script_function_failed()) return;\n";
        }
        if (needs_editor_hint || (default_calls_initializer && !module.is_tool))
            source << "    const bool gdpp_editor_hint = gdpp::runtime::is_editor_hint();\n";
        emit_autoload_registration();
        if (std::any_of(module.fields.begin(), module.fields.end(), cached_preload_field)) {
            source << (module.is_tool ? "    _gdpp_preload_resources();\n"
                                      : "    if (!gdpp_editor_hint) _gdpp_preload_resources();\n");
            source << "    if (script_function_failed()) return;\n";
        }
        emit_instance_initializers();
        if (has_native_rpc)
            emit_rpc_configurations(source, module.functions, 1);
        if (default_calls_initializer) {
            if (!module.is_tool)
                source << "    if (gdpp_editor_hint) return;\n";
            source << "    _gdpp_script_method__init(";
            for (std::size_t index = 0; index < initializer->parameters.size(); ++index) {
                if (index != 0)
                    source << ", ";
                source << "gdpp::runtime::default_argument()";
            }
            if (!initializer->parameters.empty())
                source << ", ";
            source << "godot::Array());\n";
        }
        source << "}\n\n";
        in_function_body_ = false;
    }
    if (!attached_script && initializer != module.functions.end()) {
        in_function_body_ = true;
        source << unit.class_name << "::" << unit.class_name << '(';
        for (std::size_t index = 0; index < initializer->parameters.size(); ++index) {
            if (index != 0)
                source << ", ";
            const auto& parameter = initializer->parameters[index];
            source << parameter_native_type(parameter) << ' ' << parameter_native_name(parameter);
        }
        if (initializer->rest_parameter) {
            if (!initializer->parameters.empty())
                source << ", ";
            source << "godot::Array " << sanitize_identifier(initializer->rest_parameter->name);
        }
        source << ") {\n";
        source << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                  "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n";
        if (has_static_initialization) {
            source << "    (void)_gdpp_ensure_static_initialized();\n"
                   << "    if (script_function_failed()) return;\n";
        }
        if (!module.is_tool || is_autoload)
            source << "    const bool gdpp_editor_hint = gdpp::runtime::is_editor_hint();\n";
        emit_autoload_registration();
        if (std::any_of(module.fields.begin(), module.fields.end(), cached_preload_field)) {
            source << (module.is_tool ? "    _gdpp_preload_resources();\n"
                                      : "    if (!gdpp_editor_hint) _gdpp_preload_resources();\n");
            source << "    if (script_function_failed()) return;\n";
        }
        emit_instance_initializers();
        if (has_native_rpc)
            emit_rpc_configurations(source, module.functions, 1);
        if (!module.is_tool)
            source << "    if (gdpp_editor_hint) return;\n";
        source << "    _gdpp_script_method__init(";
        for (std::size_t index = 0; index < initializer->parameters.size(); ++index) {
            if (index != 0)
                source << ", ";
            source << parameter_native_name(initializer->parameters[index]);
        }
        if (initializer->rest_parameter) {
            if (!initializer->parameters.empty())
                source << ", ";
            source << sanitize_identifier(initializer->rest_parameter->name);
        }
        source << ");\n";
        source << "}\n\n";
        in_function_body_ = false;
    }
    for (const auto& variable : module.fields) {
        if (cached_preload_field(variable) && !variable.is_static) {
            const auto name = sanitize_identifier(variable.name);
            const auto type = cpp_type(variable.type);
            source << "std::atomic<" << type << "*>& " << unit.class_name << "::_gdpp_preloaded_"
                   << name << "_pointer() {\n    static std::atomic<" << type
                   << "*> value{nullptr};\n    return value;\n}\n\n"
                   << "std::mutex& " << unit.class_name << "::_gdpp_preloaded_" << name
                   << "_mutex() {\n    static std::mutex value;\n    return value;\n}\n\n"
                   << type << "& " << unit.class_name << "::_gdpp_preloaded_" << name << "() {\n"
                   << "    auto* value = _gdpp_preloaded_" << name
                   << "_pointer().load(std::memory_order_acquire);\n"
                   << "    if (value) return *value;\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_preloaded_" << name
                   << "_mutex());\n"
                   << "    value = _gdpp_preloaded_" << name
                   << "_pointer().load(std::memory_order_relaxed);\n"
                   << "    if (!value) {\n"
                   << "        value = new " << type << "{};\n"
                   << "        _gdpp_preloaded_" << name
                   << "_pointer().store(value, std::memory_order_release);\n"
                   << "    }\n"
                   << "    return *value;\n}\n\n"
                   << "void " << unit.class_name << "::_gdpp_preloaded_" << name << "_release() {\n"
                   << "    std::lock_guard<std::mutex> lock(_gdpp_preloaded_" << name
                   << "_mutex());\n"
                   << "    delete _gdpp_preloaded_" << name
                   << "_pointer().exchange(nullptr, std::memory_order_acq_rel);\n}\n\n";
        }
    }
    const bool has_cached_preloads =
        std::any_of(module.fields.begin(), module.fields.end(), cached_preload_field);
    const auto emit_cached_preload_cleanup = [&](const std::string& prefix) {
        for (const auto& variable : module.fields) {
            if (!cached_preload_field(variable))
                continue;
            if (variable.is_static) {
                source << prefix << "_gdpp_static_" << sanitize_identifier(variable.name)
                       << "_release();\n";
                continue;
            }
            source << prefix << "_gdpp_preloaded_" << sanitize_identifier(variable.name)
                   << "_release();\n";
        }
    };
    if (has_cached_preloads) {
        source << "gdpp::runtime::ScriptInitializationState& " << unit.class_name
               << "::_gdpp_preload_initialization() {\n"
               << "    static gdpp::runtime::ScriptInitializationState state;\n"
               << "    return state;\n}\n\n";
    }
    source << "void " << unit.class_name << "::_gdpp_preload_resources() {\n";
    if (has_cached_preloads) {
        if (!module.is_tool)
            source << "    if (gdpp::runtime::is_editor_hint()) return;\n";
        source << "    (void)_gdpp_preload_initialization().run(\n"
               << "        \"Preload initialization previously failed for " << unit.class_name
               << ".\",\n"
               << "        []() {\n";
    }
    for (const auto& variable : module.fields) {
        if (!cached_preload_field(variable))
            continue;
        const auto prefix = has_cached_preloads ? "            " : "    ";
        std::string target;
        if (!variable.is_static)
            target = "_gdpp_preloaded_";
        target += sanitize_identifier(variable.name);
        if (!variable.is_static)
            target += "()";
        source << prefix
               << emit_storage_assignment(variable.type, std::move(target),
                                          emit_conversion(variable.type,
                                                          emitted_type(*variable.initializer),
                                                          emit_expression(*variable.initializer),
                                                          &variable.initializer->span))
               << ";\n"
               << prefix << "if (script_function_failed()) return;\n";
    }
    if (has_cached_preloads) {
        source << "        },\n"
               << "        []() {\n";
        emit_cached_preload_cleanup("            ");
        source << "        });\n";
    }
    source << "}\n\n";
    source << "void " << unit.class_name << "::_gdpp_release_preloaded_resources() {\n";
    if (has_cached_preloads) {
        source << "    _gdpp_preload_initialization().reset([]() {\n";
        emit_cached_preload_cleanup("        ");
        source << "    });\n";
    }
    if (has_static_initialization) {
        source << "    _gdpp_static_initialization().reset([]() {\n";
        for (const auto& variable : module.fields) {
            if (managed_constant_field(variable)) {
                const auto name = sanitize_identifier(variable.name);
                source << "        _gdpp_constant_" << name << "_release();\n";
            } else if (!variable.is_constant && variable.is_static) {
                source << "        _gdpp_static_" << sanitize_identifier(variable.name)
                       << "_release();\n";
            }
        }
        source << "    });\n";
    } else {
        for (const auto& variable : module.fields) {
            if (!managed_constant_field(variable))
                continue;
            const auto name = sanitize_identifier(variable.name);
            source << "    _gdpp_constant_" << name << "_release();\n";
        }
    }
    source << "}\n\n";
    source << "void " << unit.class_name << "::_bind_methods() {\n";
    source << "    godot::ClassDB::bind_static_method(get_class_static(), "
              "godot::D_METHOD(\"_gdpp_preload_resources\"), &"
           << unit.class_name << "::_gdpp_preload_resources);\n";
    std::unordered_map<std::string, std::size_t> enum_member_counts;
    for (const auto& enumeration : module.enums) {
        for (const auto& entry : enumeration.entries)
            ++enum_member_counts[entry.name];
    }
    for (const auto& enumeration : module.enums) {
        for (const auto& entry : enumeration.entries) {
            if (enum_member_counts[entry.name] != 1)
                continue;
            const auto value = enumeration.name.empty() ? enum_identifier(entry.name)
                                                        : sanitize_identifier(enumeration.name) +
                                                              "::" + enum_identifier(entry.name);
            source << "    godot::ClassDB::bind_integer_constant(get_class_static(), "
                   << godot_text_argument(enumeration.name) << ", "
                   << godot_text_argument(entry.name) << ", " << value << ");\n";
        }
    }
    for (const auto& variable : module.fields) {
        for (const auto& group : variable.property_groups) {
            const auto group_name =
                group.arguments.empty() ? std::string{} : group.arguments[0].value;
            const auto prefix =
                group.arguments.size() < 2 ? std::string{} : group.arguments[1].value;
            if (group.name == "export_category") {
                source << "    gdpp::runtime::register_property_marker(get_class_static(), "
                          "godot::PropertyInfo(godot::Variant::NIL, "
                       << godot_text_argument(group_name)
                       << ", godot::PROPERTY_HINT_NONE, godot::String(), "
                          "godot::PROPERTY_USAGE_CATEGORY));\n";
            } else {
                source << "    " << (group.name == "export_subgroup" ? "ADD_SUBGROUP" : "ADD_GROUP")
                       << "(" << godot_text_argument(group_name) << ", "
                       << godot_text_argument(prefix) << ");\n";
            }
        }
        if (!is_bound_property(variable))
            continue;
        const auto name = sanitize_identifier(variable.name);
        source << "    godot::ClassDB::bind_method(godot::D_METHOD(\"_gdpp_get_" << name << "\"), &"
               << unit.class_name << "::_gdpp_get_" << name << ");\n"
               << "    godot::ClassDB::bind_method(godot::D_METHOD(\"_gdpp_set_" << name
               << "\", \"value\"), &" << unit.class_name << "::_gdpp_set_" << name << ");\n"
               << "    ADD_PROPERTY("
               << property_info(variable, api_, script_symbols_, cpp_type(variable.type))
               << ", \"_gdpp_set_" << name << "\", \"_gdpp_get_" << name << "\");\n";
    }
    for (const auto& function : module.functions) {
        // Engine callbacks are C++ virtual overrides. Registering them again as
        // ordinary extension methods conflicts with godot-cpp's virtual-method
        // registry and can crash when a large extension is initialized.
        if ((!attached_script && function.name == "_init") || function.name == "_static_init" ||
            (!function.is_static && virtual_method_for(function)))
            continue;
        source << emit_method_registration(function, unit.class_name,
                                           function_return_type(function));
    }
    for (const auto& signal : module.signals) {
        source << "    ADD_SIGNAL(godot::MethodInfo(" << godot_text_argument(signal.name);
        for (const auto& parameter : signal.parameters) {
            source << ", " << native_property_info(parameter.type, parameter.name);
        }
        source << "));\n";
    }
    source << "}\n\n";
    if (attached_script) {
        emit_attached_descriptor_definition(
            source, unit.class_name, "res://" + current_source_path_,
            current_script_ && current_script_->globally_named ? current_script_->script_name
                                                               : std::string{},
            attached_native_base, attached_base_script_path, script_contract_hash, module.is_tool,
            module.is_abstract, module.fields, module.functions, module.signals, module.enums,
            module.classes);
    }
    for (const auto& variable : module.fields) {
        if (variable.is_constant)
            continue;
        const auto name = sanitize_identifier(variable.name);
        record_generated_symbol(GeneratedSymbolKind::property_getter, variable.name + ":get",
                                unit.class_name + "::_gdpp_get_" + name,
                                variable.getter ? variable.getter->span : variable.span);
        record_generated_symbol(GeneratedSymbolKind::property_setter, variable.name + ":set",
                                unit.class_name + "::_gdpp_set_" + name,
                                variable.setter ? variable.setter->span : variable.span);
        const bool inline_getter_coroutine =
            variable.getter && variable.getter->method.empty() && variable.getter->is_coroutine;
        current_return_type_ = variable.type;
        current_coroutine_abi_ = inline_getter_coroutine;
        current_static_context_ = variable.is_static;
        current_coroutine_state_ = inline_getter_coroutine ? "_gdpp_property_coroutine_state" : "";
        source << (variable.getter && variable.getter->is_coroutine ? "godot::Variant"
                                                                    : cpp_type(variable.type))
               << ' ' << unit.class_name << "::_gdpp_get_" << name << "() {\n";
        if (inline_getter_coroutine) {
            source << "    const auto " << current_coroutine_state_
                   << " = gdpp::runtime::begin_coroutine("
                   << (variable.is_static ? "nullptr" : self_object_expression()) << ");\n";
        }
        if (variable.is_static && has_static_initialization) {
            source << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                      "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
                   << "    (void)_gdpp_ensure_static_initialized();\n";
            source << emit_script_failure_return(1, false);
        }
        if (variable.getter) {
            if (!variable.getter->method.empty()) {
                source << "    return "
                       << local_function_implementation_name(variable.getter->method) << "();\n";
            } else {
                current_return_type_ = variable.type;
                in_function_body_ = true;
                current_debug_function_ = "@" + variable.name + "_getter";
                current_debug_instance_ =
                    variable.is_static ? "nullptr" : (attached_script ? "owner()" : "this");
                source << emit_script_function_scope(1);
                source << emit_debug_frame(variable.getter->span.begin.line, 1);
                source << emit_statements(variable.getter->body, 1);
                in_function_body_ = false;
                if (requires_native_fallback(variable.getter->body))
                    source << "    return {};\n";
            }
        } else {
            source << "    return "
                   << (variable.is_static ? "_gdpp_static_" + name + "_storage()" : name) << ";\n";
        }
        const auto setter_parameter = variable.setter && variable.setter->method.empty()
                                          ? sanitize_identifier(variable.setter->parameter)
                                          : "value";
        source << "}\n\n";
        current_coroutine_abi_ = false;
        current_coroutine_state_.clear();
        current_static_context_ = variable.is_static;
        current_return_type_ = {TypeKind::void_type, "void"};
        source << "void " << unit.class_name << "::_gdpp_set_" << name << '(' << "[[maybe_unused]] "
               << cpp_type(variable.type) << ' ' << setter_parameter << ") {\n";
        if (variable.is_static && has_static_initialization) {
            source << "    gdpp::runtime::ScriptFunctionScope _gdpp_script_initialization_scope("
                      "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
                   << "    (void)_gdpp_ensure_static_initialized();\n"
                   << "    if (script_function_failed()) return;\n";
        }
        if (variable.setter) {
            if (!variable.setter->method.empty()) {
                source << "    " << local_function_implementation_name(variable.setter->method)
                       << '(' << setter_parameter << ");\n";
            } else {
                current_return_type_ = {TypeKind::void_type, "void"};
                in_function_body_ = true;
                current_debug_function_ = "@" + variable.name + "_setter";
                current_debug_instance_ =
                    variable.is_static ? "nullptr" : (attached_script ? "owner()" : "this");
                source << emit_script_function_scope(1);
                source << emit_debug_frame(variable.setter->span.begin.line, 1);
                source << emit_statements(variable.setter->body, 1);
                in_function_body_ = false;
            }
        } else {
            source << "    "
                   << emit_storage_assignment(
                          variable.type,
                          variable.is_static ? "_gdpp_static_" + name + "_storage()" : name,
                          setter_parameter)
                   << ";\n";
        }
        source << "}\n\n";
        current_coroutine_abi_ = false;
        current_coroutine_state_.clear();
        current_static_context_ = false;
    }
    const auto mir_owner = module.class_name.value_or("<script>");
    for (const auto& function : module.functions) {
        current_return_type_ = function.return_type;
        current_coroutine_abi_ = coroutine_abi_for(function);
        current_static_context_ = function.is_static;
        current_coroutine_state_ = current_coroutine_abi_ ? "_gdpp_coroutine_state" : "";
        in_function_body_ = true;
        current_debug_function_ = function.name;
        current_debug_instance_ =
            function.is_static ? "nullptr" : (attached_script ? "owner()" : "this");
        const auto* engine_virtual = virtual_method_for(function);
        const auto native_function_name = function_native_name(function);
        if (engine_virtual) {
            record_generated_symbol(GeneratedSymbolKind::virtual_adapter, function.name,
                                    unit.class_name + "::" + sanitize_identifier(function.name),
                                    function.span);
            source << virtual_return_type(*engine_virtual) << ' ' << unit.class_name
                   << "::" << sanitize_identifier(function.name) << '(';
            for (std::size_t index = 0; index < engine_virtual->maximum_arguments; ++index) {
                if (index != 0)
                    source << ", ";
                source << "[[maybe_unused]] " << virtual_parameter_type(*engine_virtual, index)
                       << " _gdpp_engine_argument_" << index;
            }
            source << ')';
            if (engine_virtual->is_const)
                source << " const";
            source << " {\n";
            std::string call = engine_virtual->is_const
                                   ? "const_cast<" + unit.class_name + "*>(this)->"
                                   : "this->";
            call += native_function_name + "(";
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0)
                    call += ", ";
                const auto& parameter = function.parameters[index];
                if (index >= engine_virtual->maximum_arguments) {
                    call += "gdpp::runtime::default_argument()";
                    continue;
                }
                const auto argument_name = "_gdpp_engine_argument_" + std::to_string(index);
                if (parameter.default_value) {
                    call += "gdpp::runtime::to_variant(" + argument_name + ")";
                } else if (const auto* argument = api_.argument(*engine_virtual, index)) {
                    call += emit_conversion(parameter.type, type_from_godot_api(argument->type),
                                            argument_name, &parameter.span);
                }
            }
            if (function.rest_parameter) {
                if (!function.parameters.empty())
                    call += ", ";
                call += "godot::Array()";
            }
            call += ")";
            if (std::string_view{engine_virtual->return_type}.empty() ||
                std::string_view{engine_virtual->return_type} == "void") {
                source << "    static_cast<void>(" << call << ')';
            } else if (function.is_coroutine) {
                const auto api_return = type_from_godot_api(engine_virtual->return_type);
                source << "    const godot::Variant _gdpp_virtual_result = " << call << ";\n";
                if (!api_return.is_dynamic()) {
                    source << "    if (!gdpp::runtime::validate_virtual_return("
                              "_gdpp_virtual_result, "
                           << variant_type(api_return) << ", " << godot_string_name(function.name)
                           << ", " << godot_string_name(api_return.display_name()) << ", "
                           << (api_return.kind == TypeKind::object
                                   ? godot_string_name(api_return.name)
                                   : "godot::StringName{}")
                           << ", _gdpp_source_path, " << function.span.begin.line << ", "
                           << function.span.begin.column << ")) return {};\n";
                }
                source << "    return "
                       << emit_api_argument(engine_virtual->return_type,
                                            engine_virtual->return_meta,
                                            {TypeKind::variant, "Variant"}, "_gdpp_virtual_result");
            } else {
                source << "    return "
                       << emit_api_argument(engine_virtual->return_type,
                                            engine_virtual->return_meta, function.return_type,
                                            std::move(call));
            }
            source << ";\n}\n\n";
        }
        if (function.is_abstract) {
            record_generated_symbol(GeneratedSymbolKind::method, function.name,
                                    unit.class_name + "::" + native_function_name, function.span);
            source << function_return_type(function) << ' ' << unit.class_name
                   << "::" << native_function_name << '(';
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0)
                    source << ", ";
                const auto& parameter = function.parameters[index];
                source << "[[maybe_unused]] "
                       << (parameter.default_value ? parameter_native_type(parameter)
                                                   : function_parameter_type(function, index))
                       << ' ' << parameter_native_name(parameter);
            }
            if (function.rest_parameter) {
                if (!function.parameters.empty())
                    source << ", ";
                source << "[[maybe_unused]] godot::Array "
                       << sanitize_identifier(function.rest_parameter->name);
            }
            source << ") {\n"
                   << "    gdpp::runtime::ScriptFunctionScope _gdpp_abstract_scope("
                      "gdpp::runtime::ScriptFaultPolicy::inherit_existing);\n"
                   << "    gdpp::runtime::report_script_failure("
                   << godot_string("Cannot call abstract function '" + function.name + "'.")
                   << ", _gdpp_source_path, " << function.span.begin.line << ", "
                   << function.span.begin.column << ");\n";
            if (function.return_type.kind != TypeKind::void_type)
                source << "    return {};\n";
            source << "}\n\n";
            in_function_body_ = false;
            current_coroutine_abi_ = false;
            current_coroutine_state_.clear();
            current_static_context_ = false;
            continue;
        }
        record_generated_symbol(GeneratedSymbolKind::method, function.name,
                                unit.class_name + "::" + native_function_name, function.span);
        source << function_return_type(function) << ' ' << unit.class_name
               << "::" << native_function_name << '(';
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0)
                source << ", ";
            const auto& parameter = function.parameters[index];
            source << "[[maybe_unused]] "
                   << (parameter.default_value ? parameter_native_type(parameter)
                                               : function_parameter_type(function, index))
                   << ' ' << parameter_native_name(parameter);
        }
        if (function.rest_parameter) {
            if (!function.parameters.empty())
                source << ", ";
            source << "[[maybe_unused]] godot::Array "
                   << sanitize_identifier(function.rest_parameter->name);
        }
        source << ')';
        source << " {\n";
        if (current_coroutine_abi_) {
            source << "    const auto " << current_coroutine_state_
                   << " = gdpp::runtime::begin_coroutine("
                   << (function.is_static ? "nullptr" : self_object_expression()) << ");\n";
        }
        source << emit_script_function_scope(1, function.name == "_static_init");
        if (function.is_static && function.name != "_static_init" && has_static_initialization) {
            source << "    (void)_gdpp_ensure_static_initialized();\n";
            source << emit_script_failure_return(1, false);
        }
        source << emit_debug_frame(function.span.begin.line, 1);
        const auto parameter_control_flow = has_parameter_control_flow(function.parameters);
        if (parameter_control_flow) {
            const auto saved_overrides = local_expression_overrides_;
            source << emit_parameter_cells(function.parameters, 1);
            source << emit_parameter_initialization_chain(
                function.parameters, function.rest_parameter, function.body, {}, 1, 0, false);
            local_expression_overrides_ = saved_overrides;
        } else {
            source << emit_parameter_default_initializers(function.parameters, 1);
        }
        if (!parameter_control_flow && function.name == "_ready" && !attached_script) {
            for (const auto& field : module.fields) {
                if (field.onready && field.initializer) {
                    source << "    "
                           << emit_storage_assignment(
                                  field.type, sanitize_identifier(field.name),
                                  emit_conversion(field.type, emitted_type(*field.initializer),
                                                  emit_expression(*field.initializer),
                                                  &field.initializer->span))
                           << ";\n"
                           << emit_script_failure_return(1, false);
                }
            }
        }
        if (!parameter_control_flow) {
            const auto mir_name = mir_owner + "::" + function.name;
            const auto mir_function =
                std::find_if(mir_module.functions.begin(), mir_module.functions.end(),
                             [&](const mir::ControlFlowFunction& candidate) {
                                 return candidate.role == mir::FunctionRole::method &&
                                        candidate.name == mir_name;
                             });
            if (mir_function != mir_module.functions.end() &&
                can_emit_flat_async(function, *mir_function)) {
                source << emit_flat_async(function, *mir_function, 1);
            } else {
                source << emit_statements(function.body, 1);
            }
        }
        if (!current_coroutine_abi_ && function.return_type.kind != TypeKind::void_type &&
            (requires_native_fallback(function.body) ||
             (function.return_type.is_dynamic() && native_statements_fall_through(function.body))))
            source << "    return {};\n";
        source << "}\n\n";
        in_function_body_ = false;
        current_coroutine_abi_ = false;
        current_coroutine_state_.clear();
        current_static_context_ = false;
    }
    for (const auto& function : module.functions) {
        if (function.name == "_static_init" || (!attached_script && function.name == "_init") ||
            virtual_method_for(function)) {
            continue;
        }
        record_generated_symbol(GeneratedSymbolKind::variant_callback, function.name,
                                unit.class_name + "::" + method_callback_name(function),
                                function.span);
        source << emit_method_callback_definition(function, unit.class_name,
                                                  function_native_name(function),
                                                  function_return_type(function));
    }
    if (!attached_script && has_onready_fields && ready == module.functions.end()) {
        record_generated_symbol(GeneratedSymbolKind::synthesized_ready, "@onready",
                                unit.class_name + "::_ready", module.span);
        current_return_type_ = {TypeKind::void_type, "void"};
        in_function_body_ = true;
        source << "void " << unit.class_name << "::_ready() {\n";
        for (const auto& field : module.fields) {
            if (field.onready && field.initializer) {
                source << "    "
                       << emit_storage_assignment(
                              field.type, sanitize_identifier(field.name),
                              emit_conversion(field.type, emitted_type(*field.initializer),
                                              emit_expression(*field.initializer),
                                              &field.initializer->span))
                       << ";\n";
            }
        }
        source << "}\n\n";
        in_function_body_ = false;
        current_static_context_ = false;
    }
    source << "#if defined(__clang__)\n"
           << "#pragma clang diagnostic pop\n"
           << "#elif defined(__GNUC__)\n"
           << "#pragma GCC diagnostic pop\n"
           << "#elif defined(_MSC_VER)\n"
           << "#pragma warning(pop)\n"
           << "#endif\n";
    unit.source = source.str();
    unit.symbols = generated_symbols_;
    unit.symbol_map = serialize_generated_symbols(unit);
    return unit;
}

} // namespace gdpp
