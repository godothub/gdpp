#include "gdpp/ir/optimizer.hpp"

#include "gdpp/numeric/integer_semantics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

namespace gdpp {
namespace {

bool is_numeric_literal(const ir::Expression& expression) {
    return expression.kind == ir::ExpressionKind::literal &&
           (expression.literal_kind == ir::LiteralKind::integer ||
            expression.literal_kind == ir::LiteralKind::floating);
}

std::string normalized_number(const ir::Expression& expression) {
    auto value = expression.value;
    value.erase(std::remove(value.begin(), value.end(), '_'), value.end());
    return value;
}

std::optional<std::int64_t> integer_value(const ir::Expression& expression) {
    auto value = normalized_number(expression);
    bool negative = false;
    std::size_t sign = 0;
    if (!value.empty() && (value.front() == '+' || value.front() == '-')) {
        negative = value.front() == '-';
        sign = 1;
    }
    int base = 10;
    std::size_t prefix = sign;
    if (value.size() > sign + 2U && value[sign] == '0' &&
        (value[sign + 1U] == 'x' || value[sign + 1U] == 'X')) {
        base = 16;
        prefix += 2U;
    } else if (value.size() > sign + 2U && value[sign] == '0' &&
               (value[sign + 1U] == 'b' || value[sign + 1U] == 'B')) {
        base = 2;
        prefix += 2U;
    }
    std::uint64_t parsed = 0;
    const auto* begin = value.data() + static_cast<std::ptrdiff_t>(prefix);
    const auto* end = value.data() + static_cast<std::ptrdiff_t>(value.size());
    const auto result = std::from_chars(begin, end, parsed, base);
    if (result.ec != std::errc{} || result.ptr != end)
        return std::nullopt;
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (parsed > maximum + 1U)
            return std::nullopt;
        if (parsed == maximum + 1U)
            return std::numeric_limits<std::int64_t>::min();
        return -static_cast<std::int64_t>(parsed);
    }
    if (parsed <= maximum)
        return static_cast<std::int64_t>(parsed);
    if (base == 10 && parsed == maximum + 1U)
        return std::numeric_limits<std::int64_t>::min();
    return std::nullopt;
}

std::optional<double> floating_value(const ir::Expression& expression) {
    const auto value = normalized_number(expression);
    if (value == "inf" || value == "+inf")
        return std::numeric_limits<double>::infinity();
    if (value == "-inf")
        return -std::numeric_limits<double>::infinity();
    if (value == "nan" || value == "+nan" || value == "-nan")
        return std::numeric_limits<double>::quiet_NaN();

    std::istringstream input{value};
    input.imbue(std::locale::classic());
    double parsed = 0.0;
    input >> parsed;
    if (input.fail())
        return std::nullopt;
    return parsed;
}

std::string floating_text(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return std::signbit(value) ? "-inf" : "inf";
    if (value == 0.0 && std::signbit(value))
        return "-0.0";

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    auto text = output.str();
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";
    return text;
}

void replace_literal(ir::Expression& expression, ir::LiteralKind literal_kind, Type type,
                     std::string value) {
    expression.kind = ir::ExpressionKind::literal;
    expression.literal_kind = literal_kind;
    expression.type = std::move(type);
    expression.value = std::move(value);
    expression.operands.clear();
}

bool is_terminal(ir::StatementKind kind) {
    return kind == ir::StatementKind::return_statement ||
           kind == ir::StatementKind::break_statement ||
           kind == ir::StatementKind::continue_statement;
}

std::optional<bool> boolean_literal(const ir::Expression* expression) {
    if (!expression || expression->kind != ir::ExpressionKind::literal ||
        expression->literal_kind != ir::LiteralKind::boolean) {
        return std::nullopt;
    }
    return expression->value == "true";
}

std::size_t statement_tree_size(const ir::Statement& statement) {
    const auto count = [](const std::vector<ir::Statement>& statements) {
        std::size_t result = statements.size();
        for (const auto& child : statements)
            result += statement_tree_size(child) - 1U;
        return result;
    };
    return 1U + count(statement.body) + count(statement.else_body) + count(statement.guard_prefix) +
           count(statement.assert_condition_prefix) + count(statement.assert_message_prefix);
}

} // namespace

void IrOptimizer::optimize_expression(ir::Expression& expression, OptimizationStats& stats) const {
    for (auto& operand : expression.operands)
        optimize_expression(*operand, stats);
    if (expression.lambda) {
        for (auto& parameter : expression.lambda->parameters) {
            optimize_statements(parameter.default_prefix, stats);
            if (parameter.default_value)
                optimize_expression(*parameter.default_value, stats);
        }
        optimize_statements(expression.lambda->body, stats);
    }

    if (expression.kind == ir::ExpressionKind::unary && expression.operands.size() == 1) {
        const auto& operand = *expression.operands.front();
        if (is_numeric_literal(operand) &&
            (expression.value == "+" || expression.value == "-" || expression.value == "~")) {
            if (operand.literal_kind == ir::LiteralKind::integer) {
                const auto parsed = integer_value(operand);
                if (!parsed)
                    return;
                const auto result = expression.value == "-"   ? integer::negate(*parsed)
                                    : expression.value == "~" ? integer::bit_not(*parsed)
                                                              : *parsed;
                replace_literal(expression, ir::LiteralKind::integer, {TypeKind::integer, "int"},
                                std::to_string(result));
            } else if (expression.value != "~") {
                const auto parsed = floating_value(operand);
                if (!parsed)
                    return;
                const auto result = expression.value == "-" ? -*parsed : *parsed;
                replace_literal(expression, ir::LiteralKind::floating,
                                {TypeKind::floating, "float"}, floating_text(result));
            }
            ++stats.constants_folded;
        } else if (operand.literal_kind == ir::LiteralKind::boolean && expression.value == "not") {
            replace_literal(expression, ir::LiteralKind::boolean, {TypeKind::boolean, "bool"},
                            operand.value == "true" ? "false" : "true");
            ++stats.constants_folded;
        }
        return;
    }

    if (expression.kind != ir::ExpressionKind::binary || expression.operands.size() != 2)
        return;
    const auto& left = *expression.operands[0];
    const auto& right = *expression.operands[1];
    if (left.literal_kind == ir::LiteralKind::string &&
        right.literal_kind == ir::LiteralKind::string && expression.value == "+") {
        replace_literal(expression, ir::LiteralKind::string, {TypeKind::string, "String"},
                        left.value + right.value);
        ++stats.constants_folded;
        return;
    }
    if (!is_numeric_literal(left) || !is_numeric_literal(right))
        return;

    const bool floating = left.literal_kind == ir::LiteralKind::floating ||
                          right.literal_kind == ir::LiteralKind::floating;
    const auto& operation = expression.value;

    if (!floating) {
        const auto left_value = integer_value(left);
        const auto right_value = integer_value(right);
        if (!left_value || !right_value)
            return;
        if ((operation == "/" || operation == "%") && *right_value == 0)
            return;
        if (operation == "==" || operation == "!=" || operation == "<" || operation == "<=" ||
            operation == ">" || operation == ">=") {
            const bool result = operation == "=="   ? *left_value == *right_value
                                : operation == "!=" ? *left_value != *right_value
                                : operation == "<"  ? *left_value < *right_value
                                : operation == "<=" ? *left_value <= *right_value
                                : operation == ">"  ? *left_value > *right_value
                                                    : *left_value >= *right_value;
            replace_literal(expression, ir::LiteralKind::boolean, {TypeKind::boolean, "bool"},
                            result ? "true" : "false");
            ++stats.constants_folded;
            return;
        }
        std::optional<std::int64_t> result;
        if (operation == "+")
            result = integer::add(*left_value, *right_value);
        else if (operation == "-")
            result = integer::subtract(*left_value, *right_value);
        else if (operation == "*")
            result = integer::multiply(*left_value, *right_value);
        else if (operation == "/") {
            const auto divided = integer::divide(*left_value, *right_value);
            if (!divided)
                return;
            result = divided.value;
        } else if (operation == "%") {
            const auto modulo = integer::modulo(*left_value, *right_value);
            if (!modulo)
                return;
            result = modulo.value;
        } else if (operation == "<<")
            result = integer::shift_left(*left_value, *right_value);
        else if (operation == ">>")
            result = integer::shift_right(*left_value, *right_value);
        else if (operation == "&")
            result = integer::bit_and(*left_value, *right_value);
        else if (operation == "|")
            result = integer::bit_or(*left_value, *right_value);
        else if (operation == "^")
            result = integer::bit_xor(*left_value, *right_value);
        else
            return;
        replace_literal(expression, ir::LiteralKind::integer, {TypeKind::integer, "int"},
                        std::to_string(*result));
        ++stats.constants_folded;
        return;
    }

    const auto parsed_left = floating_value(left);
    const auto parsed_right = floating_value(right);
    if (!parsed_left || !parsed_right)
        return;
    const double left_value = *parsed_left;
    const double right_value = *parsed_right;
    if ((operation == "/" || operation == "%") && right_value == 0.0)
        return;

    if (operation == "==" || operation == "!=" || operation == "<" || operation == "<=" ||
        operation == ">" || operation == ">=") {
        bool result = false;
        if (operation == "==")
            result = left_value == right_value;
        if (operation == "!=")
            result = left_value != right_value;
        if (operation == "<")
            result = left_value < right_value;
        if (operation == "<=")
            result = left_value <= right_value;
        if (operation == ">")
            result = left_value > right_value;
        if (operation == ">=")
            result = left_value >= right_value;
        replace_literal(expression, ir::LiteralKind::boolean, {TypeKind::boolean, "bool"},
                        result ? "true" : "false");
        ++stats.constants_folded;
        return;
    }

    double result = 0.0;
    if (operation == "+")
        result = left_value + right_value;
    else if (operation == "-")
        result = left_value - right_value;
    else if (operation == "*")
        result = left_value * right_value;
    else if (operation == "/")
        result = left_value / right_value;
    else if (operation == "%")
        result = std::fmod(left_value, right_value);
    else
        return;

    replace_literal(expression, ir::LiteralKind::floating, {TypeKind::floating, "float"},
                    floating_text(result));
    ++stats.constants_folded;
}

void IrOptimizer::optimize_class(ir::Class& declaration, OptimizationStats& stats) const {
    for (auto& field : declaration.fields) {
        if (field.initializer)
            optimize_expression(*field.initializer, stats);
        if (field.getter)
            optimize_statements(field.getter->body, stats);
        if (field.setter)
            optimize_statements(field.setter->body, stats);
    }
    for (auto& function : declaration.functions) {
        for (auto& parameter : function.parameters) {
            optimize_statements(parameter.default_prefix, stats);
            if (parameter.default_value)
                optimize_expression(*parameter.default_value, stats);
        }
        optimize_statements(function.body, stats);
    }
    for (auto& inner : declaration.classes)
        optimize_class(inner, stats);
}

void IrOptimizer::optimize_statements(std::vector<ir::Statement>& statements,
                                      OptimizationStats& stats) const {
    for (auto iterator = statements.begin(); iterator != statements.end();) {
        auto& statement = *iterator;
        if (statement.expression)
            optimize_expression(*statement.expression, stats);
        if (statement.condition)
            optimize_expression(*statement.condition, stats);
        for (auto& pattern : statement.patterns)
            optimize_match_pattern(pattern, stats);

        const auto condition = boolean_literal(statement.condition.get());
        if (statement.kind == ir::StatementKind::while_statement && condition && !*condition) {
            stats.statements_removed += statement_tree_size(statement);
            ++stats.branches_simplified;
            iterator = statements.erase(iterator);
            continue;
        }
        if (statement.kind == ir::StatementKind::if_statement && condition) {
            auto& discarded = *condition ? statement.else_body : statement.body;
            for (const auto& child : discarded)
                stats.statements_removed += statement_tree_size(child);
            discarded.clear();
            ++stats.branches_simplified;
        }
        optimize_statements(statement.body, stats);
        optimize_statements(statement.else_body, stats);
        optimize_statements(statement.guard_prefix, stats);
        optimize_statements(statement.assert_condition_prefix, stats);
        optimize_statements(statement.assert_message_prefix, stats);
        ++iterator;
    }

    const auto before_passes = statements.size();
    statements.erase(std::remove_if(statements.begin(), statements.end(),
                                    [](const ir::Statement& statement) {
                                        return statement.kind == ir::StatementKind::pass_statement;
                                    }),
                     statements.end());
    stats.statements_removed += before_passes - statements.size();

    const auto terminal =
        std::find_if(statements.begin(), statements.end(),
                     [](const ir::Statement& statement) { return is_terminal(statement.kind); });
    if (terminal != statements.end()) {
        const auto keep = static_cast<std::size_t>(std::distance(statements.begin(), terminal)) + 1;
        stats.statements_removed += statements.size() - keep;
        statements.erase(statements.begin() + static_cast<std::ptrdiff_t>(keep), statements.end());
    }
}

void IrOptimizer::optimize_match_pattern(ir::MatchPattern& pattern,
                                         OptimizationStats& stats) const {
    if (pattern.expression)
        optimize_expression(*pattern.expression, stats);
    for (auto& key : pattern.keys) {
        if (key)
            optimize_expression(*key, stats);
    }
    for (auto& element : pattern.elements)
        optimize_match_pattern(element, stats);
}

OptimizationStats IrOptimizer::optimize(ir::Module& module) const {
    OptimizationStats stats;
    for (auto& field : module.fields) {
        if (field.initializer)
            optimize_expression(*field.initializer, stats);
        if (field.getter)
            optimize_statements(field.getter->body, stats);
        if (field.setter)
            optimize_statements(field.setter->body, stats);
    }
    for (auto& signal : module.signals) {
        for (auto& parameter : signal.parameters) {
            optimize_statements(parameter.default_prefix, stats);
            if (parameter.default_value)
                optimize_expression(*parameter.default_value, stats);
        }
    }
    for (auto& function : module.functions) {
        for (auto& parameter : function.parameters) {
            optimize_statements(parameter.default_prefix, stats);
            if (parameter.default_value)
                optimize_expression(*parameter.default_value, stats);
        }
        optimize_statements(function.body, stats);
    }
    for (auto& declaration : module.classes)
        optimize_class(declaration, stats);
    return stats;
}

} // namespace gdpp
