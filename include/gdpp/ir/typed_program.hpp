#pragma once

#include "gdpp/core/source.hpp"
#include "gdpp/semantic/default_argument.hpp"
#include "gdpp/semantic/flow.hpp"
#include "gdpp/semantic/intrinsics.hpp"
#include "gdpp/semantic/iteration.hpp"
#include "gdpp/semantic/rpc.hpp"
#include "gdpp/semantic/type.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gdpp::typed {

struct LambdaExpression;

struct CallContract {
    std::vector<Type> parameters;
    std::size_t required_arguments{0};
    bool is_vararg{false};
};

enum class ExpressionKind {
    literal,
    identifier,
    unary,
    await_expression,
    binary,
    call,
    member,
    subscript,
    conditional,
    node_reference,
    array_literal,
    dictionary_literal,
    lambda,
};

enum class LiteralKind { none, nil, boolean, integer, floating, string, string_name, node_path };

enum class ResolutionKind {
    none,
    godot_method,
    godot_callable,
    godot_property,
    godot_constructor,
    godot_singleton,
    external_singleton,
    godot_type,
    external_type,
    script_type,
    script_autoload,
    script_constant,
    local_constant,
    script_enum_type,
    script_resource,
    script_constructor,
    external_constructor,
    external_static_method,
    external_super_method,
    external_callable,
    external_signal,
    inner_constructor,
    inner_type,
    script_super,
    script_signal,
    script_callable,
    script_static_callable,
    script_static_field,
    script_runtime_static_field,
    script_free,
    enum_member,
    script_property,
    dynamic_method,
    dynamic_property,
    utility_function,
    global_constant,
    global_enum_type,
    global_enum_value,
    builtin_constant,
    intrinsic,
};

struct DebugVariable {
    std::string name;
    Type type;
    FlowSymbolId symbol_identity{0};
};

struct Expression {
    ExpressionKind kind{ExpressionKind::literal};
    Type type;
    Type storage_type;
    Type assignment_type;
    bool non_null{false};
    SourceSpan span{};
    std::string value;
    LiteralKind literal_kind{LiteralKind::none};
    ResolutionKind resolution{ResolutionKind::none};
    std::string resolved_owner;
    std::string getter;
    std::string setter;
    bool direct_access{false};
    std::uint16_t callable_required_arguments{0};
    std::uint16_t callable_maximum_arguments{0};
    bool callable_is_vararg{false};
    bool coroutine_call{false};
    std::int64_t indexed_argument{-1};
    FlowSymbolId symbol_identity{0};
    IntrinsicKind intrinsic{IntrinsicKind::none};
    std::optional<CallContract> call_contract;
    std::vector<DebugVariable> suspension_variables;
    std::vector<std::unique_ptr<Expression>> operands;
    std::unique_ptr<LambdaExpression> lambda;
};

using ExpressionPtr = std::unique_ptr<Expression>;

enum class StatementKind {
    expression,
    return_statement,
    await_statement,
    await_variable,
    assert_statement,
    variable,
    assignment,
    if_statement,
    match_statement,
    match_branch,
    while_statement,
    for_statement,
    pass_statement,
    break_statement,
    continue_statement,
    breakpoint_statement,
};

enum class MatchPatternKind { value, wildcard, binding, rest, array, dictionary };

struct MatchPattern {
    MatchPatternKind kind{MatchPatternKind::value};
    std::string name;
    Type type;
    FlowSymbolId symbol_identity{0};
    ExpressionPtr expression;
    std::vector<MatchPattern> elements;
    std::vector<ExpressionPtr> keys;
    SourceSpan span{};
};

struct Statement {
    StatementKind kind{StatementKind::expression};
    SourceSpan span{};
    std::string name;
    Type declared_type;
    std::string operation;
    bool is_constant{false};
    FlowSymbolId symbol_identity{0};
    IterationPlan iteration;
    ExpressionPtr expression;
    ExpressionPtr condition;
    std::vector<Statement> body;
    std::vector<Statement> else_body;
    std::vector<Statement> guard_prefix;
    std::vector<Statement> assert_condition_prefix;
    std::vector<Statement> assert_message_prefix;
    std::vector<MatchPattern> patterns;
    std::vector<DebugVariable> debug_variables;
    std::vector<DebugVariable> suspension_variables;
};

struct PropertyAccessor {
    std::string parameter;
    std::string method;
    std::vector<Statement> body;
    SourceSpan span{};
    bool is_coroutine{false};
};

struct Parameter {
    std::string name;
    Type type;
    OwnershipKind ownership{OwnershipKind::value};
    FlowSymbolId symbol_identity{0};
    std::vector<Statement> default_prefix;
    ExpressionPtr default_value;
    DefaultArgumentEvaluation default_evaluation{DefaultArgumentEvaluation::absent};
    SourceSpan span{};
};

enum class PropertyArgumentKind { number, string };

struct PropertyArgument {
    PropertyArgumentKind kind{PropertyArgumentKind::string};
    std::string value;
};

struct PropertyAnnotation {
    std::string name;
    std::vector<PropertyArgument> arguments;
};

struct EnumEntry {
    std::string name;
    std::int64_t value{0};
};

struct Enum {
    std::string name;
    std::vector<EnumEntry> entries;
    SourceSpan span{};
};

struct Field {
    std::string name;
    Type type;
    Type property_type;
    ExpressionPtr initializer;
    std::optional<PropertyAnnotation> property;
    std::vector<PropertyAnnotation> property_groups;
    std::optional<PropertyAccessor> getter;
    std::optional<PropertyAccessor> setter;
    std::string enum_hint;
    bool is_constant{false};
    bool is_static{false};
    bool onready{false};
    SourceSpan span{};
};

struct Signal {
    std::string name;
    std::vector<Parameter> parameters;
    SourceSpan span{};
};

struct Function {
    std::string name;
    std::vector<Parameter> parameters;
    std::optional<Parameter> rest_parameter;
    Type return_type;
    std::vector<Statement> body;
    bool is_static{false};
    bool is_abstract{false};
    bool is_coroutine{false};
    std::optional<RpcConfiguration> rpc;
    SourceSpan span{};
};

struct LambdaExpression {
    std::string name;
    std::vector<Parameter> parameters;
    std::optional<Parameter> rest_parameter;
    Type return_type;
    std::vector<Statement> body;
    bool owner_bound{false};
    bool is_coroutine{false};
    SourceSpan span{};
};

struct Class {
    std::string name;
    std::string base_type{"RefCounted"};
    bool is_abstract{false};
    std::vector<Enum> enums;
    std::vector<Field> fields;
    std::vector<Signal> signals;
    std::vector<Function> functions;
    std::vector<Class> classes;
    SourceSpan span{};
};

struct Module {
    std::optional<std::string> base_type;
    std::optional<std::string> class_name;
    std::optional<std::string> icon_path;
    bool is_abstract{false};
    bool is_tool{false};
    bool static_unload{false};
    std::vector<Enum> enums;
    std::vector<Field> fields;
    std::vector<Signal> signals;
    std::vector<Function> functions;
    std::vector<Class> classes;
    SourceSpan span{};
};

} // namespace gdpp::typed
