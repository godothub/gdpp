#include "gdpp/ir/lowering.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace gdpp {
namespace {

ir::ExpressionKind lower_expression_kind(ast::ExpressionKind kind) {
    switch (kind) {
    case ast::ExpressionKind::literal:
        return ir::ExpressionKind::literal;
    case ast::ExpressionKind::identifier:
        return ir::ExpressionKind::identifier;
    case ast::ExpressionKind::unary:
        return ir::ExpressionKind::unary;
    case ast::ExpressionKind::await_expression:
        return ir::ExpressionKind::await_expression;
    case ast::ExpressionKind::binary:
        return ir::ExpressionKind::binary;
    case ast::ExpressionKind::call:
        return ir::ExpressionKind::call;
    case ast::ExpressionKind::member:
        return ir::ExpressionKind::member;
    case ast::ExpressionKind::subscript:
        return ir::ExpressionKind::subscript;
    case ast::ExpressionKind::conditional:
        return ir::ExpressionKind::conditional;
    case ast::ExpressionKind::node_reference:
        return ir::ExpressionKind::node_reference;
    case ast::ExpressionKind::array_literal:
        return ir::ExpressionKind::array_literal;
    case ast::ExpressionKind::dictionary_literal:
        return ir::ExpressionKind::dictionary_literal;
    case ast::ExpressionKind::lambda:
        return ir::ExpressionKind::lambda;
    }
    return ir::ExpressionKind::literal;
}

ir::LiteralKind lower_literal_kind(ast::LiteralKind kind) {
    switch (kind) {
    case ast::LiteralKind::none:
        return ir::LiteralKind::none;
    case ast::LiteralKind::nil:
        return ir::LiteralKind::nil;
    case ast::LiteralKind::boolean:
        return ir::LiteralKind::boolean;
    case ast::LiteralKind::integer:
        return ir::LiteralKind::integer;
    case ast::LiteralKind::floating:
        return ir::LiteralKind::floating;
    case ast::LiteralKind::string:
        return ir::LiteralKind::string;
    case ast::LiteralKind::string_name:
        return ir::LiteralKind::string_name;
    case ast::LiteralKind::node_path:
        return ir::LiteralKind::node_path;
    }
    return ir::LiteralKind::none;
}

ir::StatementKind lower_statement_kind(ast::StatementKind kind) {
    switch (kind) {
    case ast::StatementKind::expression:
        return ir::StatementKind::expression;
    case ast::StatementKind::return_statement:
        return ir::StatementKind::return_statement;
    case ast::StatementKind::assert_statement:
        return ir::StatementKind::assert_statement;
    case ast::StatementKind::variable:
        return ir::StatementKind::variable;
    case ast::StatementKind::assignment:
        return ir::StatementKind::assignment;
    case ast::StatementKind::if_statement:
        return ir::StatementKind::if_statement;
    case ast::StatementKind::match_statement:
        return ir::StatementKind::match_statement;
    case ast::StatementKind::while_statement:
        return ir::StatementKind::while_statement;
    case ast::StatementKind::for_statement:
        return ir::StatementKind::for_statement;
    case ast::StatementKind::pass_statement:
        return ir::StatementKind::pass_statement;
    case ast::StatementKind::break_statement:
        return ir::StatementKind::break_statement;
    case ast::StatementKind::continue_statement:
        return ir::StatementKind::continue_statement;
    case ast::StatementKind::breakpoint_statement:
        return ir::StatementKind::breakpoint_statement;
    }
    return ir::StatementKind::pass_statement;
}

bool contains_await_expression(const ir::Expression& expression) {
    if (expression.kind == ir::ExpressionKind::await_expression)
        return true;
    return std::any_of(expression.operands.begin(), expression.operands.end(),
                       [](const auto& operand) { return contains_await_expression(*operand); });
}

bool is_type_reference(const ir::Expression& expression) {
    return expression.resolution == ir::ResolutionKind::godot_type ||
           expression.resolution == ir::ResolutionKind::external_type ||
           expression.resolution == ir::ResolutionKind::script_type ||
           expression.resolution == ir::ResolutionKind::inner_type ||
           expression.resolution == ir::ResolutionKind::script_enum_type ||
           expression.resolution == ir::ResolutionKind::global_enum_type;
}

ir::ExpressionPtr make_temporary_reference(const std::string& name, const Type& type,
                                           SourceSpan span) {
    auto reference = std::make_unique<ir::Expression>();
    reference->kind = ir::ExpressionKind::identifier;
    reference->type = type;
    reference->storage_type = type;
    reference->span = span;
    reference->value = name;
    return reference;
}

ir::ExpressionPtr make_boolean_literal(bool value, SourceSpan span) {
    auto literal = std::make_unique<ir::Expression>();
    literal->kind = ir::ExpressionKind::literal;
    literal->type = {TypeKind::boolean, "bool"};
    literal->storage_type = literal->type;
    literal->span = span;
    literal->value = value ? "true" : "false";
    literal->literal_kind = ir::LiteralKind::boolean;
    return literal;
}

ir::ExpressionPtr make_truthiness(ir::ExpressionPtr expression) {
    const auto span = expression->span;
    auto negate = std::make_unique<ir::Expression>();
    negate->kind = ir::ExpressionKind::unary;
    negate->type = {TypeKind::boolean, "bool"};
    negate->storage_type = negate->type;
    negate->span = span;
    negate->value = "not";
    negate->operands.push_back(std::move(expression));

    auto truthy = std::make_unique<ir::Expression>();
    truthy->kind = ir::ExpressionKind::unary;
    truthy->type = {TypeKind::boolean, "bool"};
    truthy->storage_type = truthy->type;
    truthy->span = span;
    truthy->value = "not";
    truthy->operands.push_back(std::move(negate));
    return truthy;
}

ir::Statement make_temporary_declaration(const std::string& name, const Type& type, SourceSpan span,
                                         ir::ExpressionPtr initializer = {}) {
    ir::Statement declaration;
    declaration.kind = ir::StatementKind::variable;
    declaration.span = span;
    declaration.name = name;
    declaration.declared_type = type;
    declaration.expression = std::move(initializer);
    return declaration;
}

ir::Statement make_temporary_assignment(const std::string& name, const Type& type, SourceSpan span,
                                        ir::ExpressionPtr value) {
    ir::Statement assignment;
    assignment.kind = ir::StatementKind::assignment;
    assignment.span = span;
    assignment.operation = "=";
    assignment.condition = make_temporary_reference(name, type, span);
    assignment.expression = std::move(value);
    return assignment;
}

ir::ExpressionPtr materialize_before_suspend(ir::ExpressionPtr expression,
                                             std::vector<ir::Statement>& prefix,
                                             std::size_t& temporary_counter) {
    if (!expression || is_type_reference(*expression) ||
        expression->type.kind == TypeKind::void_type)
        return expression;
    const auto type = expression->type;
    const auto span = expression->span;
    // '@' and '-' cannot occur in a GDScript identifier. The synthetic source identity therefore
    // cannot collide with user symbols before the C++ identifier encoder makes it portable.
    const auto name = "@gdpp-await-value-" + std::to_string(temporary_counter++);
    ir::Statement declaration;
    declaration.kind = ir::StatementKind::variable;
    declaration.span = span;
    declaration.name = name;
    declaration.declared_type = type;
    declaration.expression = std::move(expression);
    prefix.push_back(std::move(declaration));
    return make_temporary_reference(name, type, span);
}

ir::ExpressionPtr normalize_await_expression(ir::ExpressionPtr expression,
                                             std::vector<ir::Statement>& prefix,
                                             std::size_t& temporary_counter) {
    if (!expression)
        return expression;
    if (expression->kind == ir::ExpressionKind::await_expression) {
        auto awaited = normalize_await_expression(std::move(expression->operands.at(0)), prefix,
                                                  temporary_counter);
        const auto type = expression->type;
        const auto span = expression->span;
        const auto name = "@gdpp-await-result-" + std::to_string(temporary_counter++);
        ir::Statement suspension;
        suspension.kind = ir::StatementKind::await_variable;
        suspension.span = span;
        suspension.name = name;
        suspension.declared_type = type;
        suspension.expression = std::move(awaited);
        prefix.push_back(std::move(suspension));
        return make_temporary_reference(name, type, span);
    }

    if (expression->kind == ir::ExpressionKind::binary &&
        (expression->value == "and" || expression->value == "or") &&
        contains_await_expression(*expression->operands.at(1))) {
        const auto conjunction = expression->value == "and";
        const auto span = expression->span;
        auto left = normalize_await_expression(std::move(expression->operands.at(0)), prefix,
                                               temporary_counter);
        const auto name = "@gdpp-await-logic-" + std::to_string(temporary_counter++);
        const Type result_type{TypeKind::boolean, "bool"};
        prefix.push_back(make_temporary_declaration(name, result_type, span));

        ir::Statement branch;
        branch.kind = ir::StatementKind::if_statement;
        branch.span = span;
        branch.condition = std::move(left);

        auto right = normalize_await_expression(std::move(expression->operands.at(1)),
                                                conjunction ? branch.body : branch.else_body,
                                                temporary_counter);
        auto right_assignment =
            make_temporary_assignment(name, result_type, span, make_truthiness(std::move(right)));
        auto short_circuit_assignment = make_temporary_assignment(
            name, result_type, span, make_boolean_literal(!conjunction, span));
        if (conjunction) {
            branch.body.push_back(std::move(right_assignment));
            branch.else_body.push_back(std::move(short_circuit_assignment));
        } else {
            branch.body.push_back(std::move(short_circuit_assignment));
            branch.else_body.push_back(std::move(right_assignment));
        }
        prefix.push_back(std::move(branch));
        return make_temporary_reference(name, result_type, span);
    }

    if (expression->kind == ir::ExpressionKind::conditional &&
        contains_await_expression(*expression)) {
        const auto type = expression->type;
        const auto span = expression->span;
        auto condition = normalize_await_expression(std::move(expression->operands.at(1)), prefix,
                                                    temporary_counter);
        const auto name = "@gdpp-await-branch-" + std::to_string(temporary_counter++);
        prefix.push_back(make_temporary_declaration(name, type, span));

        ir::Statement branch;
        branch.kind = ir::StatementKind::if_statement;
        branch.span = span;
        branch.condition = std::move(condition);
        auto when_true = normalize_await_expression(std::move(expression->operands.at(0)),
                                                    branch.body, temporary_counter);
        branch.body.push_back(make_temporary_assignment(name, type, span, std::move(when_true)));
        auto when_false = normalize_await_expression(std::move(expression->operands.at(2)),
                                                     branch.else_body, temporary_counter);
        branch.else_body.push_back(
            make_temporary_assignment(name, type, span, std::move(when_false)));
        prefix.push_back(std::move(branch));
        return make_temporary_reference(name, type, span);
    }

    std::vector<bool> await_at_or_after(expression->operands.size() + 1U, false);
    for (std::size_t index = expression->operands.size(); index > 0; --index) {
        await_at_or_after[index - 1] =
            await_at_or_after[index] || contains_await_expression(*expression->operands[index - 1]);
    }
    for (std::size_t index = 0; index < expression->operands.size(); ++index) {
        expression->operands[index] = normalize_await_expression(
            std::move(expression->operands[index]), prefix, temporary_counter);
        if (!await_at_or_after[index + 1])
            continue;
        if (expression->kind == ir::ExpressionKind::call && index == 0) {
            auto& callee = expression->operands[index];
            if (callee && callee->kind == ir::ExpressionKind::member && !callee->operands.empty() &&
                !is_type_reference(*callee->operands.front())) {
                callee->operands.front() = materialize_before_suspend(
                    std::move(callee->operands.front()), prefix, temporary_counter);
            }
            continue;
        }
        expression->operands[index] = materialize_before_suspend(
            std::move(expression->operands[index]), prefix, temporary_counter);
    }
    return expression;
}

void stabilize_assignment_target(ir::Expression& target, std::vector<ir::Statement>& prefix,
                                 std::size_t& temporary_counter) {
    if (target.kind == ir::ExpressionKind::member && !target.operands.empty() &&
        !is_type_reference(*target.operands.front())) {
        target.operands.front() = materialize_before_suspend(std::move(target.operands.front()),
                                                             prefix, temporary_counter);
    } else if (target.kind == ir::ExpressionKind::subscript && target.operands.size() == 2U) {
        target.operands[0] =
            materialize_before_suspend(std::move(target.operands[0]), prefix, temporary_counter);
        target.operands[1] =
            materialize_before_suspend(std::move(target.operands[1]), prefix, temporary_counter);
    }
}

void normalize_await_block(std::vector<ir::Statement>& statements, std::size_t& temporary_counter) {
    std::vector<ir::Statement> normalized;
    normalized.reserve(statements.size());
    for (auto& statement : statements) {
        if (statement.kind == ir::StatementKind::match_statement) {
            for (auto& branch : statement.body) {
                normalize_await_block(branch.body, temporary_counter);
                normalize_await_block(branch.else_body, temporary_counter);
                if (branch.expression) {
                    branch.expression = normalize_await_expression(
                        std::move(branch.expression), branch.guard_prefix, temporary_counter);
                }
            }
        } else {
            normalize_await_block(statement.body, temporary_counter);
            normalize_await_block(statement.else_body, temporary_counter);
        }
        std::vector<ir::Statement> prefix;

        if (statement.kind == ir::StatementKind::assert_statement) {
            statement.condition =
                normalize_await_expression(std::move(statement.condition),
                                           statement.assert_condition_prefix, temporary_counter);
            if (statement.expression) {
                statement.expression =
                    normalize_await_expression(std::move(statement.expression),
                                               statement.assert_message_prefix, temporary_counter);
            }
            normalized.push_back(std::move(statement));
            continue;
        }

        if (statement.kind == ir::StatementKind::while_statement && statement.condition &&
            contains_await_expression(*statement.condition)) {
            const auto state_name =
                "@gdpp-await-loop-condition-" + std::to_string(temporary_counter++);
            const Type state_type{TypeKind::boolean, "bool"};
            prefix.push_back(
                make_temporary_declaration(state_name, state_type, statement.span,
                                           make_boolean_literal(true, statement.condition->span)));

            std::vector<ir::Statement> condition_body;
            auto condition = normalize_await_expression(std::move(statement.condition),
                                                        condition_body, temporary_counter);
            condition_body.push_back(make_temporary_assignment(
                state_name, state_type, statement.span, make_truthiness(std::move(condition))));

            ir::Statement guarded_body;
            guarded_body.kind = ir::StatementKind::if_statement;
            guarded_body.span = statement.span;
            guarded_body.condition =
                make_temporary_reference(state_name, state_type, statement.span);
            guarded_body.body = std::move(statement.body);
            condition_body.push_back(std::move(guarded_body));

            statement.condition = make_temporary_reference(state_name, state_type, statement.span);
            statement.body = std::move(condition_body);
        }

        if (statement.kind == ir::StatementKind::if_statement ||
            statement.kind == ir::StatementKind::for_statement ||
            statement.kind == ir::StatementKind::match_statement) {
            statement.condition = normalize_await_expression(std::move(statement.condition), prefix,
                                                             temporary_counter);
        }
        if (statement.kind == ir::StatementKind::assignment && statement.expression &&
            contains_await_expression(*statement.expression) && statement.condition) {
            stabilize_assignment_target(*statement.condition, prefix, temporary_counter);
        }
        if (statement.expression && statement.kind != ir::StatementKind::await_statement &&
            statement.kind != ir::StatementKind::await_variable) {
            statement.expression = normalize_await_expression(std::move(statement.expression),
                                                              prefix, temporary_counter);
        }
        if (!prefix.empty() && prefix.back().kind == ir::StatementKind::await_variable &&
            statement.expression && statement.expression->kind == ir::ExpressionKind::identifier &&
            statement.expression->value == prefix.back().name) {
            if (statement.kind == ir::StatementKind::expression) {
                prefix.back().kind = ir::StatementKind::await_statement;
                prefix.back().name.clear();
                prefix.back().declared_type = {TypeKind::unknown, "unknown"};
                normalized.insert(normalized.end(), std::make_move_iterator(prefix.begin()),
                                  std::make_move_iterator(prefix.end()));
                continue;
            }
            if (statement.kind == ir::StatementKind::variable) {
                prefix.back().name = statement.name;
                prefix.back().declared_type = statement.declared_type;
                prefix.back().is_constant = statement.is_constant;
                normalized.insert(normalized.end(), std::make_move_iterator(prefix.begin()),
                                  std::make_move_iterator(prefix.end()));
                continue;
            }
        }
        normalized.insert(normalized.end(), std::make_move_iterator(prefix.begin()),
                          std::make_move_iterator(prefix.end()));
        normalized.push_back(std::move(statement));
    }
    statements = std::move(normalized);
}

} // namespace

ir::ExpressionPtr IrLowerer::lower_expression(const ast::Expression& expression) const {
    auto lowered = std::make_unique<ir::Expression>();
    lowered->kind = lower_expression_kind(expression.kind());
    lowered->literal_kind = lower_literal_kind(expression.literal_kind());
    lowered->type = semantic_.type_of(expression);
    lowered->storage_type = semantic_.storage_type_of(expression);
    lowered->assignment_type = lowered->type;
    lowered->non_null = semantic_.is_non_null(expression);
    lowered->span = expression.span;
    lowered->value = expression.value();
    lowered->coroutine_call = semantic_.is_coroutine_call(expression);
    if (const auto* contract = semantic_.call_contract_of(expression)) {
        lowered->call_contract = ir::CallContract{
            contract->parameters,
            contract->required_arguments,
            contract->is_vararg,
        };
    }
    if (const auto* resolution = semantic_.api_resolution_of(expression)) {
        if (resolution->kind == ApiResolutionKind::method)
            lowered->resolution = ir::ResolutionKind::godot_method;
        else if (resolution->kind == ApiResolutionKind::property)
            lowered->resolution = ir::ResolutionKind::godot_property;
        else if (resolution->kind == ApiResolutionKind::constructor)
            lowered->resolution = ir::ResolutionKind::godot_constructor;
        else if (resolution->kind == ApiResolutionKind::singleton)
            lowered->resolution = ir::ResolutionKind::godot_singleton;
        else if (resolution->kind == ApiResolutionKind::external_singleton)
            lowered->resolution = ir::ResolutionKind::external_singleton;
        else if (resolution->kind == ApiResolutionKind::type_reference)
            lowered->resolution = ir::ResolutionKind::godot_type;
        else if (resolution->kind == ApiResolutionKind::external_type_reference)
            lowered->resolution = ir::ResolutionKind::external_type;
        else if (resolution->kind == ApiResolutionKind::script_type_reference)
            lowered->resolution = ir::ResolutionKind::script_type;
        else if (resolution->kind == ApiResolutionKind::script_autoload)
            lowered->resolution = ir::ResolutionKind::script_autoload;
        else if (resolution->kind == ApiResolutionKind::script_constant)
            lowered->resolution = ir::ResolutionKind::script_constant;
        else if (resolution->kind == ApiResolutionKind::local_constant)
            lowered->resolution = ir::ResolutionKind::local_constant;
        else if (resolution->kind == ApiResolutionKind::script_enum_type)
            lowered->resolution = ir::ResolutionKind::script_enum_type;
        else if (resolution->kind == ApiResolutionKind::script_resource)
            lowered->resolution = ir::ResolutionKind::script_resource;
        else if (resolution->kind == ApiResolutionKind::script_constructor)
            lowered->resolution = ir::ResolutionKind::script_constructor;
        else if (resolution->kind == ApiResolutionKind::external_constructor)
            lowered->resolution = ir::ResolutionKind::external_constructor;
        else if (resolution->kind == ApiResolutionKind::external_static_method)
            lowered->resolution = ir::ResolutionKind::external_static_method;
        else if (resolution->kind == ApiResolutionKind::external_super_method)
            lowered->resolution = ir::ResolutionKind::external_super_method;
        else if (resolution->kind == ApiResolutionKind::external_callable)
            lowered->resolution = ir::ResolutionKind::external_callable;
        else if (resolution->kind == ApiResolutionKind::external_signal)
            lowered->resolution = ir::ResolutionKind::external_signal;
        else if (resolution->kind == ApiResolutionKind::inner_constructor)
            lowered->resolution = ir::ResolutionKind::inner_constructor;
        else if (resolution->kind == ApiResolutionKind::inner_type_reference)
            lowered->resolution = ir::ResolutionKind::inner_type;
        else if (resolution->kind == ApiResolutionKind::script_super)
            lowered->resolution = ir::ResolutionKind::script_super;
        else if (resolution->kind == ApiResolutionKind::script_signal)
            lowered->resolution = ir::ResolutionKind::script_signal;
        else if (resolution->kind == ApiResolutionKind::script_callable)
            lowered->resolution = ir::ResolutionKind::script_callable;
        else if (resolution->kind == ApiResolutionKind::script_static_callable)
            lowered->resolution = ir::ResolutionKind::script_static_callable;
        else if (resolution->kind == ApiResolutionKind::script_static_field)
            lowered->resolution = ir::ResolutionKind::script_static_field;
        else if (resolution->kind == ApiResolutionKind::script_runtime_static_field)
            lowered->resolution = ir::ResolutionKind::script_runtime_static_field;
        else if (resolution->kind == ApiResolutionKind::script_free)
            lowered->resolution = ir::ResolutionKind::script_free;
        else if (resolution->kind == ApiResolutionKind::enum_member)
            lowered->resolution = ir::ResolutionKind::enum_member;
        else if (resolution->kind == ApiResolutionKind::script_property)
            lowered->resolution = ir::ResolutionKind::script_property;
        else if (resolution->kind == ApiResolutionKind::dynamic_method)
            lowered->resolution = ir::ResolutionKind::dynamic_method;
        else if (resolution->kind == ApiResolutionKind::dynamic_property)
            lowered->resolution = ir::ResolutionKind::dynamic_property;
        else if (resolution->kind == ApiResolutionKind::utility_function)
            lowered->resolution = ir::ResolutionKind::utility_function;
        else if (resolution->kind == ApiResolutionKind::global_constant)
            lowered->resolution = ir::ResolutionKind::global_constant;
        else if (resolution->kind == ApiResolutionKind::global_enum_type)
            lowered->resolution = ir::ResolutionKind::global_enum_type;
        else if (resolution->kind == ApiResolutionKind::global_enum_value)
            lowered->resolution = ir::ResolutionKind::global_enum_value;
        else if (resolution->kind == ApiResolutionKind::builtin_constant)
            lowered->resolution = ir::ResolutionKind::builtin_constant;
        else if (resolution->kind == ApiResolutionKind::intrinsic)
            lowered->resolution = ir::ResolutionKind::intrinsic;
        if (resolution->kind == ApiResolutionKind::constructor)
            lowered->type = resolution->type;
        lowered->resolved_owner = resolution->owner;
        lowered->getter = resolution->getter;
        lowered->setter = resolution->setter;
        lowered->direct_access = resolution->direct;
        lowered->indexed_argument = resolution->indexed_argument;
        lowered->intrinsic = resolution->intrinsic;
        if (resolution->assignment_type.kind != TypeKind::unknown)
            lowered->assignment_type = resolution->assignment_type;
    }
    lowered->operands.reserve(expression.operand_count());
    for (std::size_t index = 0; index < expression.operand_count(); ++index)
        lowered->operands.push_back(lower_expression(*expression.operand(index)));
    if (const auto* lambda = expression.lambda()) {
        lowered->lambda = std::make_unique<ir::LambdaExpression>();
        lowered->lambda->name = lambda->name;
        lowered->lambda->return_type = semantic_.return_type_of(*lambda);
        lowered->lambda->owner_bound = semantic_.owner_bound(*lambda);
        lowered->lambda->is_coroutine = semantic_.is_coroutine(*lambda);
        lowered->lambda->span = lambda->span;
        lowered->lambda->parameters.reserve(lambda->parameters.size());
        for (const auto& parameter : lambda->parameters)
            lowered->lambda->parameters.push_back(lower_parameter(parameter));
        if (lambda->rest_parameter)
            lowered->lambda->rest_parameter = lower_parameter(*lambda->rest_parameter);
        lowered->lambda->body.reserve(lambda->body.size());
        for (const auto& statement : lambda->body)
            lowered->lambda->body.push_back(lower_statement(statement));
        std::size_t temporary_counter = 0;
        normalize_await_block(lowered->lambda->body, temporary_counter);
    }
    return lowered;
}

ir::Parameter IrLowerer::lower_parameter(const ast::Parameter& parameter) const {
    ir::Parameter lowered;
    lowered.name = parameter.name;
    lowered.type = semantic_.type_of(parameter);
    lowered.ownership = lowered.type.ownership();
    lowered.default_evaluation = semantic_.default_argument_evaluation_of(parameter);
    if (parameter.default_value)
        lowered.default_value = lower_expression(*parameter.default_value);
    lowered.span = parameter.span;
    return lowered;
}

ir::MatchPattern IrLowerer::lower_match_pattern(const ast::MatchPattern& pattern) const {
    ir::MatchPattern lowered;
    switch (pattern.kind()) {
    case ast::MatchPatternKind::value:
        lowered.kind = ir::MatchPatternKind::value;
        break;
    case ast::MatchPatternKind::wildcard:
        lowered.kind = ir::MatchPatternKind::wildcard;
        break;
    case ast::MatchPatternKind::binding:
        lowered.kind = ir::MatchPatternKind::binding;
        break;
    case ast::MatchPatternKind::rest:
        lowered.kind = ir::MatchPatternKind::rest;
        break;
    case ast::MatchPatternKind::array:
        lowered.kind = ir::MatchPatternKind::array;
        break;
    case ast::MatchPatternKind::dictionary:
        lowered.kind = ir::MatchPatternKind::dictionary;
        break;
    }
    lowered.name = pattern.name();
    lowered.type = semantic_.type_of(pattern);
    lowered.span = pattern.span;
    if (pattern.expression())
        lowered.expression = lower_expression(*pattern.expression());
    lowered.elements.reserve(pattern.elements.size());
    for (const auto& element : pattern.elements)
        lowered.elements.push_back(lower_match_pattern(*element));
    lowered.keys.reserve(pattern.keys.size());
    for (const auto& key : pattern.keys)
        lowered.keys.push_back(key ? lower_expression(*key) : nullptr);
    return lowered;
}

ir::Statement IrLowerer::lower_statement(const ast::Statement& statement) const {
    ir::Statement lowered;
    lowered.kind = lower_statement_kind(statement.kind());
    lowered.span = statement.span;
    lowered.name = statement.name();
    lowered.declared_type = statement.kind() == ast::StatementKind::variable ||
                                    statement.kind() == ast::StatementKind::for_statement
                                ? semantic_.type_of(statement)
                                : Type{TypeKind::unknown, "unknown"};
    lowered.operation = statement.operation();
    lowered.is_constant = statement.is_constant();
    if (statement.kind() == ast::StatementKind::for_statement)
        lowered.iteration = semantic_.iteration_plan_of(statement);
    if (statement.kind() == ast::StatementKind::breakpoint_statement) {
        for (const auto& variable : semantic_.debug_variables_at(statement))
            lowered.debug_variables.push_back({variable.name, variable.type});
    }
    if (statement.expression())
        lowered.expression = lower_expression(*statement.expression());
    if (statement.condition())
        lowered.condition = lower_expression(*statement.condition());
    lowered.body.reserve(statement.body().size());
    for (const auto& child : statement.body())
        lowered.body.push_back(lower_statement(child));
    lowered.else_body.reserve(statement.else_body().size());
    for (const auto& child : statement.else_body()) {
        lowered.else_body.push_back(lower_statement(child));
    }
    if (statement.kind() == ast::StatementKind::match_statement) {
        lowered.body.reserve(statement.match_branches().size());
        for (const auto& branch : statement.match_branches()) {
            ir::Statement lowered_branch;
            lowered_branch.kind = ir::StatementKind::match_branch;
            lowered_branch.span = branch.span;
            if (branch.guard)
                lowered_branch.expression = lower_expression(*branch.guard);
            for (const auto& child : branch.body)
                lowered_branch.body.push_back(lower_statement(child));
            for (const auto& pattern : branch.patterns)
                lowered_branch.patterns.push_back(lower_match_pattern(pattern));
            lowered.body.push_back(std::move(lowered_branch));
        }
    }
    return lowered;
}

ir::Class IrLowerer::lower_class(const ast::ClassDeclaration& declaration) const {
    ir::Class lowered;
    lowered.name = declaration.name;
    lowered.base_type = declaration.base_type.value_or("RefCounted");
    lowered.is_abstract = std::any_of(
        declaration.annotations.begin(), declaration.annotations.end(),
        [](const ast::Annotation& annotation) { return annotation.name == "abstract"; });
    lowered.span = declaration.span;
    for (const auto& enumeration : declaration.enums) {
        ir::Enum result;
        result.name = enumeration.name.value_or("");
        result.span = enumeration.span;
        for (const auto& entry : enumeration.entries)
            result.entries.push_back({entry.name, semantic_.value_of(entry)});
        lowered.enums.push_back(std::move(result));
    }
    for (const auto& variable : declaration.variables) {
        ir::Field field;
        field.name = variable.name;
        field.type = semantic_.type_of(variable);
        field.property_type = semantic_.property_type_of(variable);
        field.is_constant = variable.is_constant;
        field.is_static = variable.is_static;
        field.onready = variable.onready;
        field.span = variable.span;
        if (variable.initializer)
            field.initializer = lower_expression(*variable.initializer);
        if (variable.getter) {
            ir::PropertyAccessor accessor;
            accessor.method = variable.getter->method;
            accessor.span = variable.getter->span;
            for (const auto& statement : variable.getter->body)
                accessor.body.push_back(lower_statement(statement));
            std::size_t temporary_counter = 0;
            normalize_await_block(accessor.body, temporary_counter);
            field.getter = std::move(accessor);
        }
        if (variable.setter) {
            ir::PropertyAccessor accessor;
            accessor.parameter = variable.setter->parameter;
            accessor.method = variable.setter->method;
            accessor.span = variable.setter->span;
            for (const auto& statement : variable.setter->body)
                accessor.body.push_back(lower_statement(statement));
            std::size_t temporary_counter = 0;
            normalize_await_block(accessor.body, temporary_counter);
            field.setter = std::move(accessor);
        }
        lowered.fields.push_back(std::move(field));
    }
    for (const auto& signal : declaration.signals) {
        ir::Signal result;
        result.name = signal.name;
        result.span = signal.span;
        for (const auto& parameter : signal.parameters)
            result.parameters.push_back(lower_parameter(parameter));
        lowered.signals.push_back(std::move(result));
    }
    for (const auto& function : declaration.functions) {
        ir::Function result;
        result.name = function.name;
        result.return_type = semantic_.return_type_of(function);
        result.is_static = function.is_static;
        result.is_abstract = function.is_abstract;
        result.is_coroutine = semantic_.is_coroutine(function);
        if (const auto* rpc = semantic_.rpc_configuration_of(function))
            result.rpc = *rpc;
        result.span = function.span;
        for (const auto& parameter : function.parameters)
            result.parameters.push_back(lower_parameter(parameter));
        if (function.rest_parameter)
            result.rest_parameter = lower_parameter(*function.rest_parameter);
        for (const auto& statement : function.body)
            result.body.push_back(lower_statement(statement));
        std::size_t temporary_counter = 0;
        normalize_await_block(result.body, temporary_counter);
        lowered.functions.push_back(std::move(result));
    }
    for (const auto& inner : declaration.classes)
        lowered.classes.push_back(lower_class(inner));
    return lowered;
}

ir::Module IrLowerer::lower(const ast::Script& script) const {
    ir::Module module;
    module.base_type = script.base_type;
    module.class_name = script.class_name;
    const auto icon =
        std::find_if(script.annotations.begin(), script.annotations.end(),
                     [](const ast::Annotation& annotation) { return annotation.name == "icon"; });
    if (icon != script.annotations.end() && icon->arguments.size() == 1 &&
        icon->arguments.front()->literal_kind() == ast::LiteralKind::string) {
        module.icon_path = icon->arguments.front()->value();
    }
    module.is_abstract = std::any_of(
        script.annotations.begin(), script.annotations.end(),
        [](const ast::Annotation& annotation) { return annotation.name == "abstract"; });
    module.is_tool = script.tool;
    module.span = script.span;
    module.enums.reserve(script.enums.size());
    for (const auto& declaration : script.enums) {
        ir::Enum enumeration;
        enumeration.name = declaration.name.value_or("");
        enumeration.span = declaration.span;
        enumeration.entries.reserve(declaration.entries.size());
        for (const auto& entry : declaration.entries) {
            enumeration.entries.push_back({entry.name, semantic_.value_of(entry)});
        }
        module.enums.push_back(std::move(enumeration));
    }
    module.fields.reserve(script.variables.size());
    for (const auto& variable : script.variables) {
        ir::Field field;
        field.name = variable.name;
        field.type = semantic_.type_of(variable);
        field.property_type = semantic_.property_type_of(variable);
        field.is_static = variable.is_static;
        field.onready = variable.onready;
        if (variable.initializer)
            field.initializer = lower_expression(*variable.initializer);
        if (variable.getter) {
            ir::PropertyAccessor getter;
            getter.method = variable.getter->method;
            getter.span = variable.getter->span;
            getter.body.reserve(variable.getter->body.size());
            for (const auto& statement : variable.getter->body)
                getter.body.push_back(lower_statement(statement));
            std::size_t temporary_counter = 0;
            normalize_await_block(getter.body, temporary_counter);
            field.getter = std::move(getter);
        }
        if (variable.setter) {
            ir::PropertyAccessor setter;
            setter.parameter = variable.setter->parameter;
            setter.method = variable.setter->method;
            setter.span = variable.setter->span;
            setter.body.reserve(variable.setter->body.size());
            for (const auto& statement : variable.setter->body)
                setter.body.push_back(lower_statement(statement));
            std::size_t temporary_counter = 0;
            normalize_await_block(setter.body, temporary_counter);
            field.setter = std::move(setter);
        }
        for (const auto& annotation : variable.annotations) {
            if (annotation.name == "onready" || annotation.name == "warning_ignore")
                continue;
            ir::PropertyAnnotation property;
            property.name = annotation.name;
            for (std::size_t index = 0; index < annotation.arguments.size(); ++index) {
                const auto& argument = annotation.arguments[index];
                ir::PropertyArgument lowered;
                if (annotation.name == "export_custom" && (index == 0 || index == 2)) {
                    if (const auto value = semantic_.constant_integer_value_of(*argument)) {
                        lowered.kind = ir::PropertyArgumentKind::number;
                        lowered.value = std::to_string(*value);
                        property.arguments.push_back(std::move(lowered));
                        continue;
                    }
                }
                const ast::Expression* value = argument.get();
                std::string prefix;
                if (const auto* unary = value->get_if<ast::UnaryExpression>();
                    unary && (unary->operation == "+" || unary->operation == "-")) {
                    prefix = unary->operation;
                    value = unary->operand.get();
                }
                lowered.kind = value->literal_kind() == ast::LiteralKind::string
                                   ? ir::PropertyArgumentKind::string
                                   : ir::PropertyArgumentKind::number;
                lowered.value = prefix + value->value();
                property.arguments.push_back(std::move(lowered));
            }
            if (annotation.name == "export_group" || annotation.name == "export_subgroup" ||
                annotation.name == "export_category")
                field.property_groups.push_back(std::move(property));
            else
                field.property = std::move(property);
        }
        if (field.property && field.type.kind == TypeKind::enumeration) {
            for (const auto& enumeration : module.enums) {
                const auto separator = field.type.name.rfind('.');
                const auto local_name = separator == std::string::npos
                                            ? field.type.name
                                            : field.type.name.substr(separator + 1);
                if (enumeration.name != local_name)
                    continue;
                for (const auto& entry : enumeration.entries) {
                    if (!field.enum_hint.empty())
                        field.enum_hint.push_back(',');
                    field.enum_hint += entry.name + ":" + std::to_string(entry.value);
                }
                break;
            }
        }
        field.is_constant = variable.is_constant;
        field.span = variable.span;
        module.fields.push_back(std::move(field));
    }
    module.signals.reserve(script.signals.size());
    for (const auto& signal : script.signals) {
        ir::Signal lowered;
        lowered.name = signal.name;
        lowered.span = signal.span;
        lowered.parameters.reserve(signal.parameters.size());
        for (const auto& parameter : signal.parameters) {
            lowered.parameters.push_back(lower_parameter(parameter));
        }
        module.signals.push_back(std::move(lowered));
    }
    module.functions.reserve(script.functions.size());
    for (const auto& function : script.functions) {
        ir::Function lowered;
        lowered.name = function.name;
        lowered.return_type = semantic_.return_type_of(function);
        lowered.is_static = function.is_static;
        lowered.is_abstract = function.is_abstract;
        lowered.is_coroutine = semantic_.is_coroutine(function);
        if (const auto* rpc = semantic_.rpc_configuration_of(function))
            lowered.rpc = *rpc;
        lowered.span = function.span;
        lowered.parameters.reserve(function.parameters.size());
        for (const auto& parameter : function.parameters) {
            lowered.parameters.push_back(lower_parameter(parameter));
        }
        if (function.rest_parameter)
            lowered.rest_parameter = lower_parameter(*function.rest_parameter);
        lowered.body.reserve(function.body.size());
        for (const auto& statement : function.body) {
            lowered.body.push_back(lower_statement(statement));
        }
        std::size_t temporary_counter = 0;
        normalize_await_block(lowered.body, temporary_counter);
        module.functions.push_back(std::move(lowered));
    }
    module.classes.reserve(script.classes.size());
    for (const auto& declaration : script.classes)
        module.classes.push_back(lower_class(declaration));
    return module;
}

bool IrVerifier::verify_parameter(const ir::Parameter& parameter) {
    bool valid = true;
    if (parameter.ownership != parameter.type.ownership()) {
        diagnostics_.error("GDS5045", "parameter IR ownership disagrees with its semantic type",
                           parameter.span);
        valid = false;
    }
    const bool has_default = parameter.default_value != nullptr;
    const bool has_evaluation = parameter.default_evaluation != DefaultArgumentEvaluation::absent;
    if (has_default != has_evaluation) {
        diagnostics_.error("GDS5038",
                           has_default
                               ? "default argument IR is missing its evaluation contract"
                               : "parameter without a default carries an evaluation contract",
                           parameter.span);
        valid = false;
    }
    if (parameter.default_value)
        valid = verify_expression(*parameter.default_value) && valid;
    return valid;
}

bool IrVerifier::verify_rest_parameter(const ir::Parameter& parameter) {
    bool valid = verify_parameter(parameter);
    if (parameter.name.empty()) {
        diagnostics_.error("GDS5042", "rest parameter IR is missing its name", parameter.span);
        valid = false;
    }
    if (parameter.type.kind != TypeKind::array) {
        diagnostics_.error("GDS5043", "rest parameter IR must have Array type", parameter.span);
        valid = false;
    }
    if (parameter.default_value ||
        parameter.default_evaluation != DefaultArgumentEvaluation::absent) {
        diagnostics_.error("GDS5044", "rest parameter IR cannot carry a default value",
                           parameter.span);
        valid = false;
    }
    return valid;
}

bool IrVerifier::verify_expression(const ir::Expression& expression) {
    std::size_t minimum = 0;
    std::optional<std::size_t> exact;
    switch (expression.kind) {
    case ir::ExpressionKind::literal:
    case ir::ExpressionKind::identifier:
    case ir::ExpressionKind::node_reference:
    case ir::ExpressionKind::array_literal:
        break;
    case ir::ExpressionKind::lambda:
        exact = 0;
        break;
    case ir::ExpressionKind::unary:
    case ir::ExpressionKind::await_expression:
    case ir::ExpressionKind::member:
        exact = 1;
        break;
    case ir::ExpressionKind::binary:
    case ir::ExpressionKind::subscript:
        exact = 2;
        break;
    case ir::ExpressionKind::conditional:
        exact = 3;
        break;
    case ir::ExpressionKind::call:
        minimum = 1;
        break;
    case ir::ExpressionKind::dictionary_literal:
        if (expression.operands.size() % 2 != 0) {
            diagnostics_.error("GDS5001", "dictionary IR must contain key/value operand pairs",
                               expression.span);
            return false;
        }
        break;
    }
    bool valid = true;
    if ((exact.has_value() && expression.operands.size() != *exact) ||
        expression.operands.size() < minimum) {
        diagnostics_.error("GDS5002", "invalid operand count in typed IR", expression.span);
        valid = false;
    }
    if (expression.kind == ir::ExpressionKind::literal &&
        expression.literal_kind == ir::LiteralKind::none) {
        diagnostics_.error("GDS5003", "literal IR is missing its literal type", expression.span);
        valid = false;
    }
    if (expression.call_contract) {
        if (expression.kind != ir::ExpressionKind::call) {
            diagnostics_.error("GDS5046", "call contract is attached to non-call IR",
                               expression.span);
            valid = false;
        }
        if (expression.call_contract->required_arguments >
            expression.call_contract->parameters.size()) {
            diagnostics_.error("GDS5047", "call contract requires more arguments than it declares",
                               expression.span);
            valid = false;
        }
        for (const auto& parameter : expression.call_contract->parameters) {
            if (parameter.kind == TypeKind::unknown || parameter.kind == TypeKind::void_type) {
                diagnostics_.error("GDS5048", "call contract contains an invalid parameter type",
                                   expression.span);
                valid = false;
            }
        }
    }
    for (const auto& operand : expression.operands) {
        if (!operand || !verify_expression(*operand))
            valid = false;
    }
    if (expression.kind == ir::ExpressionKind::lambda) {
        if (!expression.lambda) {
            diagnostics_.error("GDS5026", "lambda IR is missing its function payload",
                               expression.span);
            valid = false;
        } else {
            for (const auto& parameter : expression.lambda->parameters) {
                if (parameter.name.empty()) {
                    diagnostics_.error("GDS5027", "lambda parameter is missing its name",
                                       parameter.span);
                    valid = false;
                }
                valid = verify_parameter(parameter) && valid;
            }
            if (expression.lambda->rest_parameter)
                valid = verify_rest_parameter(*expression.lambda->rest_parameter) && valid;
            for (const auto& statement : expression.lambda->body) {
                if (!verify_statement(statement))
                    valid = false;
            }
        }
    }
    if (expression.kind == ir::ExpressionKind::binary &&
        (expression.value == "is" || expression.value == "is not") &&
        expression.operands.size() == 2) {
        const auto resolution = expression.operands[1]->resolution;
        if (resolution != ir::ResolutionKind::godot_type &&
            resolution != ir::ResolutionKind::external_type &&
            resolution != ir::ResolutionKind::script_type &&
            resolution != ir::ResolutionKind::inner_type &&
            resolution != ir::ResolutionKind::script_enum_type &&
            resolution != ir::ResolutionKind::global_enum_type) {
            diagnostics_.error("GDS5023", "type-test IR requires a resolved type operand",
                               expression.span);
            valid = false;
        }
    }
    if (expression.kind == ir::ExpressionKind::member &&
        (expression.resolution == ir::ResolutionKind::dynamic_method ||
         expression.resolution == ir::ResolutionKind::dynamic_property)) {
        const bool valid_receiver =
            expression.operands.size() == 1 &&
            (expression.operands.front()->type.is_dynamic() ||
             expression.operands.front()->type.kind == TypeKind::object ||
             (expression.resolution == ir::ResolutionKind::dynamic_property &&
              expression.operands.front()->type.kind == TypeKind::dictionary));
        const bool contract_typed = !expression.resolved_owner.empty();
        if (!valid_receiver || (!expression.type.is_dynamic() && !contract_typed)) {
            diagnostics_.error("GDS5024",
                               "dynamic member IR requires an object-compatible receiver and "
                               "a Variant or bridge-contract result",
                               expression.span);
            valid = false;
        }
    }
    return valid;
}

bool IrVerifier::verify_match_pattern(const ir::MatchPattern& pattern) {
    bool valid = true;
    const auto reject_payload = [&] {
        if (pattern.expression || !pattern.elements.empty() || !pattern.keys.empty()) {
            diagnostics_.error("GDS5031", "marker match pattern contains an invalid payload",
                               pattern.span);
            valid = false;
        }
    };
    switch (pattern.kind) {
    case ir::MatchPatternKind::value:
        if (!pattern.expression || !verify_expression(*pattern.expression)) {
            diagnostics_.error("GDS5018", "match value pattern IR is invalid", pattern.span);
            valid = false;
        }
        if (!pattern.elements.empty() || !pattern.keys.empty()) {
            diagnostics_.error("GDS5031", "match value pattern contains structural children",
                               pattern.span);
            valid = false;
        }
        break;
    case ir::MatchPatternKind::binding:
        if (pattern.name.empty()) {
            diagnostics_.error("GDS5019", "match binding IR is missing a name", pattern.span);
            valid = false;
        }
        reject_payload();
        break;
    case ir::MatchPatternKind::wildcard:
    case ir::MatchPatternKind::rest:
        reject_payload();
        break;
    case ir::MatchPatternKind::array: {
        if (pattern.expression || !pattern.keys.empty()) {
            diagnostics_.error("GDS5031", "array match pattern IR has an invalid payload",
                               pattern.span);
            valid = false;
        }
        std::size_t rest_count = 0;
        for (std::size_t index = 0; index < pattern.elements.size(); ++index) {
            const auto& child = pattern.elements[index];
            if (child.kind == ir::MatchPatternKind::rest) {
                ++rest_count;
                if (index + 1U != pattern.elements.size()) {
                    diagnostics_.error("GDS5032", "array rest pattern must be terminal",
                                       child.span);
                    valid = false;
                }
            }
            if (!verify_match_pattern(child))
                valid = false;
        }
        if (rest_count > 1U) {
            diagnostics_.error("GDS5032", "array match pattern has multiple rest markers",
                               pattern.span);
            valid = false;
        }
        break;
    }
    case ir::MatchPatternKind::dictionary: {
        if (pattern.expression || pattern.keys.size() != pattern.elements.size()) {
            diagnostics_.error("GDS5031", "dictionary match pattern IR has invalid key/value shape",
                               pattern.span);
            valid = false;
        }
        const auto count = std::min(pattern.keys.size(), pattern.elements.size());
        std::size_t rest_count = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& child = pattern.elements[index];
            if (!pattern.keys[index]) {
                ++rest_count;
                if (child.kind != ir::MatchPatternKind::rest || index + 1U != count) {
                    diagnostics_.error("GDS5032", "dictionary rest pattern must be terminal",
                                       child.span);
                    valid = false;
                }
            } else if (!verify_expression(*pattern.keys[index])) {
                valid = false;
            }
            if (!verify_match_pattern(child))
                valid = false;
        }
        if (rest_count > 1U) {
            diagnostics_.error("GDS5032", "dictionary match pattern has multiple rest markers",
                               pattern.span);
            valid = false;
        }
        break;
    }
    }
    return valid;
}

bool IrVerifier::verify_statement(const ir::Statement& statement) {
    bool valid = true;
    const auto require_expression = [&] {
        if (!statement.expression) {
            diagnostics_.error("GDS5004", "statement IR is missing an expression", statement.span);
            valid = false;
        }
    };
    const auto require_condition = [&] {
        if (!statement.condition) {
            diagnostics_.error("GDS5005", "control-flow IR is missing a condition", statement.span);
            valid = false;
        }
    };
    switch (statement.kind) {
    case ir::StatementKind::expression:
    case ir::StatementKind::assignment:
        require_expression();
        break;
    case ir::StatementKind::variable:
    case ir::StatementKind::await_variable:
        if (statement.name.empty()) {
            diagnostics_.error("GDS5006", "variable IR is missing a name", statement.span);
            valid = false;
        }
        if (statement.is_constant && !statement.expression) {
            diagnostics_.error("GDS5022", "local constant IR is missing an initializer",
                               statement.span);
            valid = false;
        }
        break;
    case ir::StatementKind::if_statement:
    case ir::StatementKind::while_statement:
    case ir::StatementKind::for_statement:
        require_condition();
        break;
    case ir::StatementKind::match_statement:
        require_condition();
        break;
    case ir::StatementKind::match_branch:
        if (statement.patterns.empty()) {
            diagnostics_.error("GDS5017", "match branch IR is missing patterns", statement.span);
            valid = false;
        }
        break;
    case ir::StatementKind::return_statement:
    case ir::StatementKind::await_statement:
    case ir::StatementKind::assert_statement:
    case ir::StatementKind::pass_statement:
    case ir::StatementKind::break_statement:
    case ir::StatementKind::continue_statement:
    case ir::StatementKind::breakpoint_statement:
        break;
    }
    if (statement.kind == ir::StatementKind::for_statement) {
        if (!statement.iteration.valid()) {
            diagnostics_.error("GDS5035", "for-loop IR is missing its iteration plan",
                               statement.span);
            valid = false;
        } else if (statement.condition) {
            const auto& iterable = statement.condition->type;
            bool strategy_matches = false;
            switch (statement.iteration.strategy) {
            case IterationStrategy::dynamic_protocol:
                strategy_matches = iterable.is_dynamic();
                break;
            case IterationStrategy::integer_count:
                strategy_matches = iterable.kind == TypeKind::integer;
                break;
            case IterationStrategy::floating_count:
                strategy_matches = iterable.kind == TypeKind::floating;
                break;
            case IterationStrategy::intrinsic_range:
                strategy_matches =
                    statement.condition->kind == ir::ExpressionKind::call &&
                    !statement.condition->operands.empty() &&
                    statement.condition->operands.front()->resolution ==
                        ir::ResolutionKind::intrinsic &&
                    statement.condition->operands.front()->intrinsic == IntrinsicKind::range;
                break;
            case IterationStrategy::vector2_range:
                strategy_matches = iterable.kind == TypeKind::builtin && iterable.name == "Vector2";
                break;
            case IterationStrategy::vector2i_range:
                strategy_matches =
                    iterable.kind == TypeKind::builtin && iterable.name == "Vector2i";
                break;
            case IterationStrategy::vector3_range:
                strategy_matches = iterable.kind == TypeKind::builtin && iterable.name == "Vector3";
                break;
            case IterationStrategy::vector3i_range:
                strategy_matches =
                    iterable.kind == TypeKind::builtin && iterable.name == "Vector3i";
                break;
            case IterationStrategy::indexed_string:
                strategy_matches = iterable.kind == TypeKind::string;
                break;
            case IterationStrategy::indexed_array:
                strategy_matches = iterable.kind == TypeKind::array;
                break;
            case IterationStrategy::indexed_packed_array:
                strategy_matches = iterable.is_packed_array();
                break;
            case IterationStrategy::dictionary_protocol:
                strategy_matches = iterable.kind == TypeKind::dictionary;
                break;
            case IterationStrategy::object_protocol:
                strategy_matches = iterable.kind == TypeKind::object;
                break;
            case IterationStrategy::none:
                break;
            }
            if (!strategy_matches) {
                diagnostics_.error("GDS5036",
                                   "for-loop iteration plan does not match its iterable type",
                                   statement.span);
                valid = false;
            }
        }
    } else if (statement.iteration.strategy != IterationStrategy::none) {
        diagnostics_.error("GDS5037", "non-loop IR contains an iteration plan", statement.span);
        valid = false;
    }
    if (statement.kind == ir::StatementKind::assignment && !statement.condition) {
        diagnostics_.error("GDS5007", "assignment IR is missing its target", statement.span);
        valid = false;
    }
    if (statement.kind == ir::StatementKind::assignment && statement.condition &&
        statement.condition->kind != ir::ExpressionKind::identifier &&
        statement.condition->kind != ir::ExpressionKind::member &&
        statement.condition->kind != ir::ExpressionKind::subscript) {
        diagnostics_.error("GDS5029", "assignment IR target is not an lvalue",
                           statement.condition->span);
        valid = false;
    }
    if (statement.kind == ir::StatementKind::assert_statement && !statement.condition) {
        diagnostics_.error("GDS5022", "assert IR is missing its condition", statement.span);
        valid = false;
    }
    if ((statement.kind == ir::StatementKind::await_statement ||
         statement.kind == ir::StatementKind::await_variable) &&
        !statement.expression) {
        diagnostics_.error("GDS5025", "await IR is missing its signal expression", statement.span);
        valid = false;
    }
    if (statement.kind != ir::StatementKind::match_branch && !statement.guard_prefix.empty()) {
        diagnostics_.error("GDS5033", "only match branches may contain guard-prefix IR",
                           statement.span);
        valid = false;
    }
    if (statement.kind != ir::StatementKind::assert_statement &&
        (!statement.assert_condition_prefix.empty() || !statement.assert_message_prefix.empty())) {
        diagnostics_.error("GDS5034", "only assert statements may contain assert-prefix IR",
                           statement.span);
        valid = false;
    }
    if (statement.expression && !verify_expression(*statement.expression))
        valid = false;
    if (statement.condition && !verify_expression(*statement.condition))
        valid = false;
    for (const auto& pattern : statement.patterns) {
        if (!verify_match_pattern(pattern))
            valid = false;
    }
    for (const auto& child : statement.body) {
        if (!verify_statement(child))
            valid = false;
    }
    for (const auto& child : statement.else_body) {
        if (!verify_statement(child))
            valid = false;
    }
    for (const auto& child : statement.guard_prefix) {
        if (!verify_statement(child))
            valid = false;
    }
    for (const auto& child : statement.assert_condition_prefix) {
        if (!verify_statement(child))
            valid = false;
    }
    for (const auto& child : statement.assert_message_prefix) {
        if (!verify_statement(child))
            valid = false;
    }
    return valid;
}

bool IrVerifier::verify_class(const ir::Class& declaration) {
    bool valid = !declaration.name.empty();
    if (!valid)
        diagnostics_.error("GDS5028", "internal class IR is missing its name", declaration.span);
    for (const auto& field : declaration.fields) {
        if (field.name.empty()) {
            diagnostics_.error("GDS5008", "internal field IR is missing a name", field.span);
            valid = false;
        }
        if (field.initializer && !verify_expression(*field.initializer))
            valid = false;
        if (field.getter) {
            for (const auto& statement : field.getter->body)
                valid = verify_statement(statement) && valid;
        }
        if (field.setter) {
            for (const auto& statement : field.setter->body)
                valid = verify_statement(statement) && valid;
        }
    }
    for (const auto& function : declaration.functions) {
        if (function.name.empty()) {
            diagnostics_.error("GDS5011", "internal function IR is missing a name", function.span);
            valid = false;
        }
        for (const auto& parameter : function.parameters) {
            valid = verify_parameter(parameter) && valid;
        }
        if (function.rest_parameter)
            valid = verify_rest_parameter(*function.rest_parameter) && valid;
        if (function.is_abstract && !function.body.empty()) {
            diagnostics_.error("GDS5039", "abstract function IR cannot contain a body",
                               function.span);
            valid = false;
        }
        if (function.is_abstract && (function.is_static || function.is_coroutine)) {
            diagnostics_.error("GDS5040", "abstract function IR has an executable ABI",
                               function.span);
            valid = false;
        }
        if (function.is_abstract && !declaration.is_abstract) {
            diagnostics_.error("GDS5041", "concrete internal class IR declares an abstract method",
                               function.span);
            valid = false;
        }
        for (const auto& statement : function.body)
            valid = verify_statement(statement) && valid;
    }
    for (const auto& inner : declaration.classes)
        valid = verify_class(inner) && valid;
    return valid;
}

bool IrVerifier::verify(const ir::Module& module) {
    bool valid = true;
    for (const auto& enumeration : module.enums) {
        if (enumeration.entries.empty()) {
            diagnostics_.error("GDS5014", "enum IR must contain at least one member",
                               enumeration.span);
            valid = false;
        }
        std::unordered_set<std::string> names;
        for (const auto& entry : enumeration.entries) {
            if (entry.name.empty() || !names.insert(entry.name).second) {
                diagnostics_.error("GDS5015", "enum IR contains an invalid member name",
                                   enumeration.span);
                valid = false;
            }
        }
    }
    for (const auto& field : module.fields) {
        if (field.name.empty()) {
            diagnostics_.error("GDS5008", "field IR is missing a name", field.span);
            valid = false;
        }
        if (field.is_constant && !field.initializer) {
            diagnostics_.error("GDS5009", "constant IR is missing an initializer", field.span);
            valid = false;
        }
        if (field.property && field.is_constant) {
            diagnostics_.error("GDS5012", "export property IR cannot describe a constant",
                               field.span);
            valid = false;
        }
        if ((field.getter || field.setter) && field.is_constant) {
            diagnostics_.error("GDS5020", "property accessor IR cannot describe a constant",
                               field.span);
            valid = false;
        }
        if (field.property && field.property->name.empty()) {
            diagnostics_.error("GDS5013", "export property IR is missing its annotation",
                               field.span);
            valid = false;
        }
        if (field.initializer && !verify_expression(*field.initializer))
            valid = false;
        if (field.getter) {
            for (const auto& statement : field.getter->body) {
                if (!verify_statement(statement))
                    valid = false;
            }
        }
        if (field.setter) {
            if (field.setter->parameter.empty() && field.setter->method.empty()) {
                diagnostics_.error("GDS5021", "setter IR is missing its parameter", field.span);
                valid = false;
            }
            for (const auto& statement : field.setter->body) {
                if (!verify_statement(statement))
                    valid = false;
            }
        }
    }
    for (const auto& signal : module.signals) {
        if (signal.name.empty()) {
            diagnostics_.error("GDS5010", "signal IR is missing a name", signal.span);
            valid = false;
        }
        for (const auto& parameter : signal.parameters) {
            valid = verify_parameter(parameter) && valid;
        }
    }
    for (const auto& function : module.functions) {
        if (function.name.empty()) {
            diagnostics_.error("GDS5011", "function IR is missing a name", function.span);
            valid = false;
        }
        for (const auto& parameter : function.parameters) {
            valid = verify_parameter(parameter) && valid;
        }
        if (function.rest_parameter)
            valid = verify_rest_parameter(*function.rest_parameter) && valid;
        if (function.is_abstract && !function.body.empty()) {
            diagnostics_.error("GDS5039", "abstract function IR cannot contain a body",
                               function.span);
            valid = false;
        }
        if (function.is_abstract && (function.is_static || function.is_coroutine)) {
            diagnostics_.error("GDS5040", "abstract function IR has an executable ABI",
                               function.span);
            valid = false;
        }
        if (function.is_abstract && !module.is_abstract) {
            diagnostics_.error("GDS5041", "concrete module IR declares an abstract method",
                               function.span);
            valid = false;
        }
        for (const auto& statement : function.body) {
            if (!verify_statement(statement))
                valid = false;
        }
    }
    for (const auto& declaration : module.classes)
        valid = verify_class(declaration) && valid;
    return valid;
}

} // namespace gdpp
