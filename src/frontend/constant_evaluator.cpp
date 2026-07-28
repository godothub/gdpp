#include "gdpp/frontend/constant_evaluator.hpp"

#include "gdpp/frontend/literal.hpp"
#include "gdpp/numeric/integer_semantics.hpp"

#include <limits>

namespace gdpp {
namespace {

std::optional<std::string> qualified_constant_name(const ast::Expression& expression) {
    if (const auto* identifier = expression.get_if<ast::IdentifierExpression>())
        return identifier->name;
    const auto* member = expression.get_if<ast::MemberExpression>();
    if (!member || !member->receiver)
        return std::nullopt;
    const auto receiver = qualified_constant_name(*member->receiver);
    return receiver ? std::optional<std::string>{*receiver + "." + member->name} : std::nullopt;
}

} // namespace

std::optional<std::int64_t>
evaluate_integer_constant(const ast::Expression& expression,
                          const std::unordered_map<std::string, std::int64_t>& previous) {
    if (const auto* literal = expression.get_if<ast::LiteralExpression>();
        literal && literal->kind == ast::LiteralKind::integer) {
        const auto parsed = parse_integer_literal(literal->text);
        if (!parsed)
            return std::nullopt;
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (parsed->magnitude <= maximum)
            return static_cast<std::int64_t>(parsed->magnitude);
        if (parsed->base == 10 && parsed->magnitude == maximum + 1U)
            return std::numeric_limits<std::int64_t>::min();
        return std::nullopt;
    }
    if (const auto* identifier = expression.get_if<ast::IdentifierExpression>()) {
        const auto found = previous.find(identifier->name);
        return found == previous.end() ? std::nullopt : std::optional<std::int64_t>{found->second};
    }
    if (expression.get_if<ast::MemberExpression>()) {
        const auto name = qualified_constant_name(expression);
        const auto found = name ? previous.find(*name) : previous.end();
        return found == previous.end() ? std::nullopt : std::optional<std::int64_t>{found->second};
    }
    if (const auto* unary = expression.get_if<ast::UnaryExpression>()) {
        const auto operand = evaluate_integer_constant(*unary->operand, previous);
        if (!operand)
            return std::nullopt;
        if (unary->operation == "+")
            return operand;
        if (unary->operation == "~")
            return integer::bit_not(*operand);
        if (unary->operation == "-")
            return integer::negate(*operand);
        return std::nullopt;
    }
    const auto* binary = expression.get_if<ast::BinaryExpression>();
    if (!binary)
        return std::nullopt;
    const auto left = evaluate_integer_constant(*binary->left, previous);
    const auto right = evaluate_integer_constant(*binary->right, previous);
    if (!left || !right)
        return std::nullopt;
    const auto& operation = binary->operation;
    if (operation == "+")
        return integer::add(*left, *right);
    if (operation == "-")
        return integer::subtract(*left, *right);
    if (operation == "*")
        return integer::multiply(*left, *right);
    if (operation == "/" || operation == "%") {
        const auto result =
            operation == "/" ? integer::divide(*left, *right) : integer::modulo(*left, *right);
        return result ? std::optional<std::int64_t>{result.value} : std::nullopt;
    }
    if (operation == "<<")
        return integer::shift_left(*left, *right);
    if (operation == ">>")
        return integer::shift_right(*left, *right);
    if (operation == "&")
        return integer::bit_and(*left, *right);
    if (operation == "|")
        return integer::bit_or(*left, *right);
    if (operation == "^")
        return integer::bit_xor(*left, *right);
    return std::nullopt;
}

std::optional<std::string>
evaluate_string_constant(const ast::Expression& expression,
                         const std::unordered_map<std::string, std::string>& previous) {
    if (const auto* literal = expression.get_if<ast::LiteralExpression>();
        literal && literal->kind == ast::LiteralKind::string) {
        return literal->text;
    }
    if (const auto* identifier = expression.get_if<ast::IdentifierExpression>()) {
        const auto found = previous.find(identifier->name);
        return found == previous.end() ? std::nullopt : std::optional<std::string>{found->second};
    }
    if (expression.get_if<ast::MemberExpression>()) {
        const auto name = qualified_constant_name(expression);
        const auto found = name ? previous.find(*name) : previous.end();
        return found == previous.end() ? std::nullopt : std::optional<std::string>{found->second};
    }
    const auto* binary = expression.get_if<ast::BinaryExpression>();
    if (!binary || binary->operation != "+")
        return std::nullopt;
    const auto left = evaluate_string_constant(*binary->left, previous);
    const auto right = evaluate_string_constant(*binary->right, previous);
    return left && right ? std::optional<std::string>{*left + *right} : std::nullopt;
}

} // namespace gdpp
