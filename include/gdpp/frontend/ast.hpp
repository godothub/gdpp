#pragma once

#include "gdpp/core/source.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace gdpp::ast {

struct Expression;
struct Statement;
struct LambdaExpression;

using ExpressionPtr = std::unique_ptr<Expression>;
using Block = std::vector<Statement>;

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

struct LiteralExpression {
    LiteralKind kind{LiteralKind::nil};
    std::string text{"null"};
};
struct IdentifierExpression {
    std::string name;
};
struct UnaryExpression {
    std::string operation;
    ExpressionPtr operand;
};
struct AwaitExpression {
    ExpressionPtr operand;
};
struct BinaryExpression {
    std::string operation;
    ExpressionPtr left;
    ExpressionPtr right;
};
struct CallExpression {
    ExpressionPtr callee;
    std::vector<ExpressionPtr> arguments;
};
struct MemberExpression {
    ExpressionPtr receiver;
    std::string name;
};
struct SubscriptExpression {
    ExpressionPtr receiver;
    ExpressionPtr index;
};
struct ConditionalExpression {
    ExpressionPtr when_true;
    ExpressionPtr condition;
    ExpressionPtr when_false;
};
struct NodeReferenceExpression {
    std::string path;
};
struct ArrayExpression {
    std::vector<ExpressionPtr> elements;
};
struct DictionaryEntry {
    ExpressionPtr key;
    ExpressionPtr value;
};
struct DictionaryExpression {
    std::vector<DictionaryEntry> entries;
};
struct LambdaValueExpression {
    std::unique_ptr<LambdaExpression> function;
};

using ExpressionNode =
    std::variant<LiteralExpression, IdentifierExpression, UnaryExpression, AwaitExpression,
                 BinaryExpression, CallExpression, MemberExpression, SubscriptExpression,
                 ConditionalExpression, NodeReferenceExpression, ArrayExpression,
                 DictionaryExpression, LambdaValueExpression>;

struct Expression {
    ExpressionNode node{LiteralExpression{}};
    SourceSpan span{};

    Expression() = default;

    template <typename Node>
    explicit Expression(Node value, SourceSpan source_span = {})
        : node(std::move(value)), span(source_span) {}

    [[nodiscard]] ExpressionKind kind() const noexcept;
    [[nodiscard]] LiteralKind literal_kind() const noexcept;
    [[nodiscard]] const std::string& value() const noexcept;
    [[nodiscard]] std::size_t operand_count() const noexcept;
    [[nodiscard]] const ExpressionPtr& operand(std::size_t index) const;
    [[nodiscard]] const LambdaExpression* lambda() const noexcept;

    template <typename Node> [[nodiscard]] const Node* get_if() const noexcept {
        return std::get_if<Node>(&node);
    }

    template <typename Node> [[nodiscard]] Node* get_if() noexcept {
        return std::get_if<Node>(&node);
    }

    template <typename Visitor> decltype(auto) visit(Visitor&& visitor) const {
        return std::visit(std::forward<Visitor>(visitor), node);
    }

    template <typename Visitor> decltype(auto) visit(Visitor&& visitor) {
        return std::visit(std::forward<Visitor>(visitor), node);
    }
};

struct Parameter {
    std::string name;
    std::optional<std::string> type;
    ExpressionPtr default_value;
    bool infer_type{false};
    SourceSpan span{};
};

struct Annotation {
    std::string name;
    std::vector<ExpressionPtr> arguments;
    SourceSpan span{};
};

enum class MatchPatternKind { value, wildcard, binding, rest, array, dictionary };

struct ValuePattern {
    ExpressionPtr expression;
};
struct WildcardPattern {};
struct BindingPattern {
    std::string name;
};
struct RestPattern {};
struct ArrayPattern {};
struct DictionaryPattern {};

using MatchPatternNode = std::variant<ValuePattern, WildcardPattern, BindingPattern, RestPattern,
                                      ArrayPattern, DictionaryPattern>;

struct MatchPattern {
    MatchPatternNode node{WildcardPattern{}};
    // Array patterns use `elements`. Dictionary patterns keep one key and one value pattern at
    // each matching index; a presence-only entry stores a wildcard value. Rest markers are
    // explicit child patterns so validation can reject duplicates and non-terminal placement.
    std::vector<std::unique_ptr<MatchPattern>> elements;
    std::vector<ExpressionPtr> keys;
    SourceSpan span{};

    [[nodiscard]] MatchPatternKind kind() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const ExpressionPtr& expression() const noexcept;
};

struct MatchBranch {
    std::vector<MatchPattern> patterns;
    ExpressionPtr guard;
    Block body;
    SourceSpan span{};
};

struct ExpressionStatement {
    ExpressionPtr expression;
};
struct ReturnStatement {
    ExpressionPtr value;
};
struct VariableStatement {
    std::string name;
    std::optional<std::string> type;
    ExpressionPtr initializer;
    bool infer_type{false};
    bool is_constant{false};
};
struct AssertStatement {
    ExpressionPtr condition;
    ExpressionPtr message;
};
struct AssignmentStatement {
    std::string operation{"="};
    ExpressionPtr target;
    ExpressionPtr value;
};
struct IfStatement {
    ExpressionPtr condition;
    Block when_true;
    Block when_false;
};
struct MatchStatement {
    ExpressionPtr subject;
    std::vector<MatchBranch> branches;
};
struct WhileStatement {
    ExpressionPtr condition;
    Block body;
};
struct ForStatement {
    std::string iterator;
    std::optional<std::string> type;
    ExpressionPtr iterable;
    Block body;
    SourceSpan iterator_span{};
    std::optional<SourceSpan> type_span;
};
struct PassStatement {};
struct BreakStatement {};
struct ContinueStatement {};
struct BreakpointStatement {};

using StatementNode =
    std::variant<ExpressionStatement, ReturnStatement, AssertStatement, VariableStatement,
                 AssignmentStatement, IfStatement, MatchStatement, WhileStatement, ForStatement,
                 PassStatement, BreakStatement, ContinueStatement, BreakpointStatement>;

enum class StatementKind {
    expression,
    return_statement,
    assert_statement,
    variable,
    assignment,
    if_statement,
    match_statement,
    while_statement,
    for_statement,
    pass_statement,
    break_statement,
    continue_statement,
    breakpoint_statement,
};

struct Statement {
    StatementNode node{PassStatement{}};
    SourceSpan span{};
    std::vector<Annotation> annotations;

    Statement() = default;

    template <typename Node>
    explicit Statement(Node value, SourceSpan source_span = {})
        : node(std::move(value)), span(source_span) {}

    [[nodiscard]] StatementKind kind() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const std::optional<std::string>& type() const noexcept;
    [[nodiscard]] const std::string& operation() const noexcept;
    [[nodiscard]] bool infer_type() const noexcept;
    [[nodiscard]] bool is_constant() const noexcept;
    [[nodiscard]] const ExpressionPtr& expression() const noexcept;
    [[nodiscard]] const ExpressionPtr& condition() const noexcept;
    [[nodiscard]] const Block& body() const noexcept;
    [[nodiscard]] const Block& else_body() const noexcept;
    [[nodiscard]] const std::vector<MatchBranch>& match_branches() const noexcept;

    template <typename Node> [[nodiscard]] const Node* get_if() const noexcept {
        return std::get_if<Node>(&node);
    }

    template <typename Node> [[nodiscard]] Node* get_if() noexcept {
        return std::get_if<Node>(&node);
    }

    template <typename Visitor> decltype(auto) visit(Visitor&& visitor) const {
        return std::visit(std::forward<Visitor>(visitor), node);
    }

    template <typename Visitor> decltype(auto) visit(Visitor&& visitor) {
        return std::visit(std::forward<Visitor>(visitor), node);
    }
};

struct PropertyAccessor {
    std::string parameter;
    // Empty for an inline accessor body. Godot's `set = method` / `get = method`
    // form stores the referenced script method here instead.
    std::string method;
    Block body;
    SourceSpan span{};
};

struct VariableDeclaration {
    std::string name;
    std::optional<std::string> type;
    ExpressionPtr initializer;
    std::vector<Annotation> annotations;
    std::optional<PropertyAccessor> getter;
    std::optional<PropertyAccessor> setter;
    bool is_constant{false};
    bool is_static{false};
    bool infer_type{false};
    bool onready{false};
    SourceSpan span{};
};

struct SignalDeclaration {
    std::string name;
    std::vector<Parameter> parameters;
    std::vector<Annotation> annotations;
    SourceSpan span{};
};

struct EnumEntry {
    std::string name;
    ExpressionPtr value;
    SourceSpan span{};
};

struct EnumDeclaration {
    std::optional<std::string> name;
    std::vector<EnumEntry> entries;
    std::vector<Annotation> annotations;
    SourceSpan span{};
};

struct FunctionDeclaration {
    std::string name;
    std::vector<Parameter> parameters;
    std::optional<Parameter> rest_parameter;
    std::optional<std::string> return_type;
    Block body;
    std::vector<Annotation> annotations;
    bool is_static{false};
    bool is_abstract{false};
    bool has_body{true};
    SourceSpan span{};
};

struct LambdaExpression {
    std::string name;
    std::vector<Parameter> parameters;
    std::optional<Parameter> rest_parameter;
    std::optional<std::string> return_type;
    Block body;
    SourceSpan span{};
};

struct ClassDeclaration {
    std::string name;
    std::optional<std::string> base_type;
    std::vector<Annotation> annotations;
    std::vector<VariableDeclaration> variables;
    std::vector<EnumDeclaration> enums;
    std::vector<SignalDeclaration> signals;
    std::vector<FunctionDeclaration> functions;
    std::vector<ClassDeclaration> classes;
    SourceSpan span{};
};

struct Script {
    std::optional<std::string> base_type;
    std::optional<std::string> class_name;
    std::vector<Annotation> annotations;
    std::vector<VariableDeclaration> variables;
    std::vector<EnumDeclaration> enums;
    std::vector<SignalDeclaration> signals;
    std::vector<FunctionDeclaration> functions;
    std::vector<ClassDeclaration> classes;
    bool tool{false};
    SourceSpan span{};
};

inline ExpressionKind Expression::kind() const noexcept {
    return static_cast<ExpressionKind>(node.index());
}

inline LiteralKind Expression::literal_kind() const noexcept {
    if (const auto* literal = get_if<LiteralExpression>())
        return literal->kind;
    return LiteralKind::none;
}

inline const std::string& Expression::value() const noexcept {
    static const std::string empty;
    if (const auto* item = get_if<LiteralExpression>())
        return item->text;
    if (const auto* item = get_if<IdentifierExpression>())
        return item->name;
    if (const auto* item = get_if<UnaryExpression>())
        return item->operation;
    if (const auto* item = get_if<BinaryExpression>())
        return item->operation;
    if (const auto* item = get_if<MemberExpression>())
        return item->name;
    if (const auto* item = get_if<NodeReferenceExpression>())
        return item->path;
    return empty;
}

inline std::size_t Expression::operand_count() const noexcept {
    if (get_if<UnaryExpression>() || get_if<AwaitExpression>() || get_if<MemberExpression>())
        return 1;
    if (get_if<BinaryExpression>() || get_if<SubscriptExpression>())
        return 2;
    if (get_if<ConditionalExpression>())
        return 3;
    if (const auto* call = get_if<CallExpression>())
        return call->arguments.size() + 1;
    if (const auto* array = get_if<ArrayExpression>())
        return array->elements.size();
    if (const auto* dictionary = get_if<DictionaryExpression>())
        return dictionary->entries.size() * 2;
    return 0;
}

inline const ExpressionPtr& Expression::operand(std::size_t index) const {
    if (const auto* unary = get_if<UnaryExpression>()) {
        if (index == 0)
            return unary->operand;
    }
    if (const auto* await = get_if<AwaitExpression>()) {
        if (index == 0)
            return await->operand;
    }
    if (const auto* binary = get_if<BinaryExpression>()) {
        if (index == 0)
            return binary->left;
        if (index == 1)
            return binary->right;
    }
    if (const auto* call = get_if<CallExpression>()) {
        if (index == 0)
            return call->callee;
        if (index - 1 < call->arguments.size())
            return call->arguments[index - 1];
    }
    if (const auto* member = get_if<MemberExpression>()) {
        if (index == 0)
            return member->receiver;
    }
    if (const auto* subscript = get_if<SubscriptExpression>()) {
        if (index == 0)
            return subscript->receiver;
        if (index == 1)
            return subscript->index;
    }
    if (const auto* conditional = get_if<ConditionalExpression>()) {
        if (index == 0)
            return conditional->when_true;
        if (index == 1)
            return conditional->condition;
        if (index == 2)
            return conditional->when_false;
    }
    if (const auto* array = get_if<ArrayExpression>()) {
        if (index < array->elements.size())
            return array->elements[index];
    }
    if (const auto* dictionary = get_if<DictionaryExpression>()) {
        if (index / 2 < dictionary->entries.size())
            return index % 2 == 0 ? dictionary->entries[index / 2].key
                                  : dictionary->entries[index / 2].value;
    }
    throw std::out_of_range{"AST expression operand index"};
}

inline const LambdaExpression* Expression::lambda() const noexcept {
    const auto* value = get_if<LambdaValueExpression>();
    return value ? value->function.get() : nullptr;
}

inline MatchPatternKind MatchPattern::kind() const noexcept {
    return static_cast<MatchPatternKind>(node.index());
}

inline const std::string& MatchPattern::name() const noexcept {
    static const std::string empty;
    const auto* binding = std::get_if<BindingPattern>(&node);
    return binding ? binding->name : empty;
}

inline const ExpressionPtr& MatchPattern::expression() const noexcept {
    static const ExpressionPtr empty;
    const auto* value = std::get_if<ValuePattern>(&node);
    return value ? value->expression : empty;
}

inline StatementKind Statement::kind() const noexcept {
    return static_cast<StatementKind>(node.index());
}

inline const std::string& Statement::name() const noexcept {
    static const std::string empty;
    if (const auto* variable = get_if<VariableStatement>())
        return variable->name;
    if (const auto* loop = get_if<ForStatement>())
        return loop->iterator;
    return empty;
}

inline const std::optional<std::string>& Statement::type() const noexcept {
    static const std::optional<std::string> empty;
    if (const auto* variable = get_if<VariableStatement>())
        return variable->type;
    if (const auto* loop = get_if<ForStatement>())
        return loop->type;
    return empty;
}

inline const std::string& Statement::operation() const noexcept {
    static const std::string empty;
    const auto* assignment = get_if<AssignmentStatement>();
    return assignment ? assignment->operation : empty;
}

inline bool Statement::infer_type() const noexcept {
    const auto* variable = get_if<VariableStatement>();
    return variable && variable->infer_type;
}

inline bool Statement::is_constant() const noexcept {
    const auto* variable = get_if<VariableStatement>();
    return variable && variable->is_constant;
}

inline const ExpressionPtr& Statement::expression() const noexcept {
    static const ExpressionPtr empty;
    if (const auto* item = get_if<ExpressionStatement>())
        return item->expression;
    if (const auto* item = get_if<ReturnStatement>())
        return item->value;
    if (const auto* item = get_if<VariableStatement>())
        return item->initializer;
    if (const auto* item = get_if<AssertStatement>())
        return item->message;
    if (const auto* item = get_if<AssignmentStatement>())
        return item->value;
    return empty;
}

inline const ExpressionPtr& Statement::condition() const noexcept {
    static const ExpressionPtr empty;
    if (const auto* item = get_if<AssertStatement>())
        return item->condition;
    if (const auto* item = get_if<AssignmentStatement>())
        return item->target;
    if (const auto* item = get_if<IfStatement>())
        return item->condition;
    if (const auto* item = get_if<MatchStatement>())
        return item->subject;
    if (const auto* item = get_if<WhileStatement>())
        return item->condition;
    if (const auto* item = get_if<ForStatement>())
        return item->iterable;
    return empty;
}

inline const Block& Statement::body() const noexcept {
    static const Block empty;
    if (const auto* item = get_if<IfStatement>())
        return item->when_true;
    if (const auto* item = get_if<WhileStatement>())
        return item->body;
    if (const auto* item = get_if<ForStatement>())
        return item->body;
    return empty;
}

inline const Block& Statement::else_body() const noexcept {
    static const Block empty;
    const auto* item = get_if<IfStatement>();
    return item ? item->when_false : empty;
}

inline const std::vector<MatchBranch>& Statement::match_branches() const noexcept {
    static const std::vector<MatchBranch> empty;
    const auto* item = get_if<MatchStatement>();
    return item ? item->branches : empty;
}

} // namespace gdpp::ast
