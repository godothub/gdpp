#include "gdpp/frontend/ast_serializer.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace gdpp {
namespace {

template <typename> struct UnhandledAstNode : std::false_type {};

void indent(std::ostream& output, const std::size_t depth) {
    for (std::size_t index = 0; index < depth; ++index)
        output << "  ";
}

void span(std::ostream& output, const SourceSpan& value) {
    output << '@' << value.begin.offset << ':' << value.begin.line << ':' << value.begin.column
           << '-' << value.end.offset << ':' << value.end.line << ':' << value.end.column;
}

void optional_text(std::ostream& output, const std::optional<std::string>& value) {
    if (value)
        output << std::quoted(*value);
    else
        output << "none";
}

std::string_view literal_kind_name(const ast::LiteralKind kind) {
    switch (kind) {
    case ast::LiteralKind::none:
        return "none";
    case ast::LiteralKind::nil:
        return "nil";
    case ast::LiteralKind::boolean:
        return "boolean";
    case ast::LiteralKind::integer:
        return "integer";
    case ast::LiteralKind::floating:
        return "floating";
    case ast::LiteralKind::string:
        return "string";
    case ast::LiteralKind::string_name:
        return "string_name";
    case ast::LiteralKind::node_path:
        return "node_path";
    }
    return "unknown";
}

void expression(std::ostream& output, const ast::Expression* value, std::size_t depth);
void block(std::ostream& output, const ast::Block& value, std::size_t depth);

void expression_field(std::ostream& output, const std::string_view name,
                      const ast::Expression* value, const std::size_t depth) {
    indent(output, depth);
    output << name << '\n';
    expression(output, value, depth + 1);
}

void annotation(std::ostream& output, const ast::Annotation& value, const std::size_t depth) {
    indent(output, depth);
    output << "annotation name " << std::quoted(value.name) << ' ';
    span(output, value.span);
    output << '\n';
    indent(output, depth + 1);
    output << "arguments " << value.arguments.size() << '\n';
    for (const auto& argument : value.arguments)
        expression(output, argument.get(), depth + 2);
}

void annotations(std::ostream& output, const std::vector<ast::Annotation>& values,
                 const std::size_t depth) {
    indent(output, depth);
    output << "annotations " << values.size() << '\n';
    for (const auto& value : values)
        annotation(output, value, depth + 1);
}

void parameter(std::ostream& output, const ast::Parameter& value, const std::size_t depth) {
    indent(output, depth);
    output << "parameter name " << std::quoted(value.name) << " type ";
    optional_text(output, value.type);
    output << " infer " << value.infer_type << ' ';
    span(output, value.span);
    output << '\n';
    expression_field(output, "default", value.default_value.get(), depth + 1);
}

void parameters(std::ostream& output, const std::vector<ast::Parameter>& values,
                const std::optional<ast::Parameter>& rest, const std::size_t depth) {
    indent(output, depth);
    output << "parameters " << values.size() << '\n';
    for (const auto& value : values)
        parameter(output, value, depth + 1);
    indent(output, depth);
    output << "rest_parameter " << rest.has_value() << '\n';
    if (rest)
        parameter(output, *rest, depth + 1);
}

void lambda(std::ostream& output, const ast::LambdaExpression& value, const std::size_t depth) {
    indent(output, depth);
    output << "lambda name " << std::quoted(value.name) << " return ";
    optional_text(output, value.return_type);
    output << ' ';
    span(output, value.span);
    output << '\n';
    parameters(output, value.parameters, value.rest_parameter, depth + 1);
    block(output, value.body, depth + 1);
}

void expression(std::ostream& output, const ast::Expression* value, const std::size_t depth) {
    if (!value) {
        indent(output, depth);
        output << "none\n";
        return;
    }
    value->visit([&](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        indent(output, depth);
        if constexpr (std::is_same_v<Node, ast::LiteralExpression>) {
            output << "literal kind " << literal_kind_name(node.kind) << " text "
                   << std::quoted(node.text) << ' ';
            span(output, value->span);
            output << '\n';
        } else if constexpr (std::is_same_v<Node, ast::IdentifierExpression>) {
            output << "identifier name " << std::quoted(node.name) << ' ';
            span(output, value->span);
            output << '\n';
        } else if constexpr (std::is_same_v<Node, ast::UnaryExpression>) {
            output << "unary operation " << std::quoted(node.operation) << ' ';
            span(output, value->span);
            output << '\n';
            expression_field(output, "operand", node.operand.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::AwaitExpression>) {
            output << "await ";
            span(output, value->span);
            output << '\n';
            expression_field(output, "operand", node.operand.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::BinaryExpression>) {
            output << "binary operation " << std::quoted(node.operation) << ' ';
            span(output, value->span);
            output << '\n';
            expression_field(output, "left", node.left.get(), depth + 1);
            expression_field(output, "right", node.right.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::CallExpression>) {
            output << "call ";
            span(output, value->span);
            output << '\n';
            expression_field(output, "callee", node.callee.get(), depth + 1);
            indent(output, depth + 1);
            output << "arguments " << node.arguments.size() << '\n';
            for (const auto& argument : node.arguments)
                expression(output, argument.get(), depth + 2);
        } else if constexpr (std::is_same_v<Node, ast::MemberExpression>) {
            output << "member name " << std::quoted(node.name) << ' ';
            span(output, value->span);
            output << '\n';
            expression_field(output, "receiver", node.receiver.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::SubscriptExpression>) {
            output << "subscript ";
            span(output, value->span);
            output << '\n';
            expression_field(output, "receiver", node.receiver.get(), depth + 1);
            expression_field(output, "index", node.index.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::ConditionalExpression>) {
            output << "conditional ";
            span(output, value->span);
            output << '\n';
            expression_field(output, "when_true", node.when_true.get(), depth + 1);
            expression_field(output, "condition", node.condition.get(), depth + 1);
            expression_field(output, "when_false", node.when_false.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::NodeReferenceExpression>) {
            output << "node_reference path " << std::quoted(node.path) << ' ';
            span(output, value->span);
            output << '\n';
        } else if constexpr (std::is_same_v<Node, ast::ArrayExpression>) {
            output << "array ";
            span(output, value->span);
            output << '\n';
            indent(output, depth + 1);
            output << "elements " << node.elements.size() << '\n';
            for (const auto& element : node.elements)
                expression(output, element.get(), depth + 2);
        } else if constexpr (std::is_same_v<Node, ast::DictionaryExpression>) {
            output << "dictionary ";
            span(output, value->span);
            output << '\n';
            indent(output, depth + 1);
            output << "entries " << node.entries.size() << '\n';
            for (const auto& entry : node.entries) {
                indent(output, depth + 2);
                output << "entry\n";
                expression_field(output, "key", entry.key.get(), depth + 3);
                expression_field(output, "value", entry.value.get(), depth + 3);
            }
        } else if constexpr (std::is_same_v<Node, ast::LambdaValueExpression>) {
            output << "lambda_value ";
            span(output, value->span);
            output << '\n';
            if (node.function)
                lambda(output, *node.function, depth + 1);
            else {
                indent(output, depth + 1);
                output << "none\n";
            }
        } else {
            static_assert(UnhandledAstNode<Node>::value, "unhandled AST expression node");
        }
    });
}

void match_pattern(std::ostream& output, const ast::MatchPattern& value, const std::size_t depth) {
    std::visit(
        [&](const auto& node) {
            using Node = std::decay_t<decltype(node)>;
            indent(output, depth);
            if constexpr (std::is_same_v<Node, ast::ValuePattern>) {
                output << "value_pattern ";
            } else if constexpr (std::is_same_v<Node, ast::WildcardPattern>) {
                output << "wildcard_pattern ";
            } else if constexpr (std::is_same_v<Node, ast::BindingPattern>) {
                output << "binding_pattern name " << std::quoted(node.name) << ' ';
            } else if constexpr (std::is_same_v<Node, ast::RestPattern>) {
                output << "rest_pattern ";
            } else if constexpr (std::is_same_v<Node, ast::ArrayPattern>) {
                output << "array_pattern ";
            } else if constexpr (std::is_same_v<Node, ast::DictionaryPattern>) {
                output << "dictionary_pattern ";
            } else {
                static_assert(UnhandledAstNode<Node>::value, "unhandled AST match pattern node");
            }
            span(output, value.span);
            output << '\n';
            if constexpr (std::is_same_v<Node, ast::ValuePattern>)
                expression_field(output, "value", node.expression.get(), depth + 1);
        },
        value.node);
    indent(output, depth + 1);
    output << "keys " << value.keys.size() << '\n';
    for (const auto& key : value.keys)
        expression(output, key.get(), depth + 2);
    indent(output, depth + 1);
    output << "elements " << value.elements.size() << '\n';
    for (const auto& element : value.elements) {
        if (element)
            match_pattern(output, *element, depth + 2);
        else {
            indent(output, depth + 2);
            output << "none\n";
        }
    }
}

void match_branch(std::ostream& output, const ast::MatchBranch& value, const std::size_t depth) {
    indent(output, depth);
    output << "match_branch ";
    span(output, value.span);
    output << '\n';
    indent(output, depth + 1);
    output << "patterns " << value.patterns.size() << '\n';
    for (const auto& pattern : value.patterns)
        match_pattern(output, pattern, depth + 2);
    expression_field(output, "guard", value.guard.get(), depth + 1);
    block(output, value.body, depth + 1);
}

void statement(std::ostream& output, const ast::Statement& value, const std::size_t depth) {
    value.visit([&](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        indent(output, depth);
        if constexpr (std::is_same_v<Node, ast::ExpressionStatement>) {
            output << "expression_statement ";
        } else if constexpr (std::is_same_v<Node, ast::ReturnStatement>) {
            output << "return_statement ";
        } else if constexpr (std::is_same_v<Node, ast::AssertStatement>) {
            output << "assert_statement ";
        } else if constexpr (std::is_same_v<Node, ast::VariableStatement>) {
            output << "variable_statement name " << std::quoted(node.name) << " type ";
            optional_text(output, node.type);
            output << " infer " << node.infer_type << " constant " << node.is_constant << ' ';
        } else if constexpr (std::is_same_v<Node, ast::AssignmentStatement>) {
            output << "assignment_statement operation " << std::quoted(node.operation) << ' ';
        } else if constexpr (std::is_same_v<Node, ast::IfStatement>) {
            output << "if_statement ";
        } else if constexpr (std::is_same_v<Node, ast::MatchStatement>) {
            output << "match_statement ";
        } else if constexpr (std::is_same_v<Node, ast::WhileStatement>) {
            output << "while_statement ";
        } else if constexpr (std::is_same_v<Node, ast::ForStatement>) {
            output << "for_statement iterator " << std::quoted(node.iterator) << " type ";
            optional_text(output, node.type);
            output << " iterator_span ";
            span(output, node.iterator_span);
            output << " type_span ";
            if (node.type_span)
                span(output, *node.type_span);
            else
                output << "none";
            output << ' ';
        } else if constexpr (std::is_same_v<Node, ast::PassStatement>) {
            output << "pass_statement ";
        } else if constexpr (std::is_same_v<Node, ast::BreakStatement>) {
            output << "break_statement ";
        } else if constexpr (std::is_same_v<Node, ast::ContinueStatement>) {
            output << "continue_statement ";
        } else if constexpr (std::is_same_v<Node, ast::BreakpointStatement>) {
            output << "breakpoint_statement ";
        } else {
            static_assert(UnhandledAstNode<Node>::value, "unhandled AST statement node");
        }
        span(output, value.span);
        output << '\n';
        annotations(output, value.annotations, depth + 1);
        if constexpr (std::is_same_v<Node, ast::ExpressionStatement>) {
            expression_field(output, "expression", node.expression.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::ReturnStatement>) {
            expression_field(output, "value", node.value.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::AssertStatement>) {
            expression_field(output, "condition", node.condition.get(), depth + 1);
            expression_field(output, "message", node.message.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::VariableStatement>) {
            expression_field(output, "initializer", node.initializer.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::AssignmentStatement>) {
            expression_field(output, "target", node.target.get(), depth + 1);
            expression_field(output, "value", node.value.get(), depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::IfStatement>) {
            expression_field(output, "condition", node.condition.get(), depth + 1);
            indent(output, depth + 1);
            output << "when_true\n";
            block(output, node.when_true, depth + 2);
            indent(output, depth + 1);
            output << "when_false\n";
            block(output, node.when_false, depth + 2);
        } else if constexpr (std::is_same_v<Node, ast::MatchStatement>) {
            expression_field(output, "subject", node.subject.get(), depth + 1);
            indent(output, depth + 1);
            output << "branches " << node.branches.size() << '\n';
            for (const auto& branch : node.branches)
                match_branch(output, branch, depth + 2);
        } else if constexpr (std::is_same_v<Node, ast::WhileStatement>) {
            expression_field(output, "condition", node.condition.get(), depth + 1);
            block(output, node.body, depth + 1);
        } else if constexpr (std::is_same_v<Node, ast::ForStatement>) {
            expression_field(output, "iterable", node.iterable.get(), depth + 1);
            block(output, node.body, depth + 1);
        }
    });
}

void block(std::ostream& output, const ast::Block& value, const std::size_t depth) {
    indent(output, depth);
    output << "block " << value.size() << '\n';
    for (const auto& item : value)
        statement(output, item, depth + 1);
}

void property_accessor(std::ostream& output, const std::string_view kind,
                       const std::optional<ast::PropertyAccessor>& value, const std::size_t depth) {
    indent(output, depth);
    output << kind << ' ' << value.has_value() << '\n';
    if (!value)
        return;
    indent(output, depth + 1);
    output << "property_accessor parameter " << std::quoted(value->parameter) << " method "
           << std::quoted(value->method) << ' ';
    span(output, value->span);
    output << '\n';
    block(output, value->body, depth + 2);
}

void variable(std::ostream& output, const ast::VariableDeclaration& value,
              const std::size_t depth) {
    indent(output, depth);
    output << "variable name " << std::quoted(value.name) << " type ";
    optional_text(output, value.type);
    output << " constant " << value.is_constant << " static " << value.is_static << " infer "
           << value.infer_type << " onready " << value.onready << ' ';
    span(output, value.span);
    output << '\n';
    annotations(output, value.annotations, depth + 1);
    expression_field(output, "initializer", value.initializer.get(), depth + 1);
    property_accessor(output, "getter", value.getter, depth + 1);
    property_accessor(output, "setter", value.setter, depth + 1);
}

void signal(std::ostream& output, const ast::SignalDeclaration& value, const std::size_t depth) {
    indent(output, depth);
    output << "signal name " << std::quoted(value.name) << ' ';
    span(output, value.span);
    output << '\n';
    annotations(output, value.annotations, depth + 1);
    parameters(output, value.parameters, std::nullopt, depth + 1);
}

void enumeration(std::ostream& output, const ast::EnumDeclaration& value, const std::size_t depth) {
    indent(output, depth);
    output << "enum name ";
    optional_text(output, value.name);
    output << ' ';
    span(output, value.span);
    output << '\n';
    annotations(output, value.annotations, depth + 1);
    indent(output, depth + 1);
    output << "entries " << value.entries.size() << '\n';
    for (const auto& entry : value.entries) {
        indent(output, depth + 2);
        output << "enum_entry name " << std::quoted(entry.name) << ' ';
        span(output, entry.span);
        output << '\n';
        expression_field(output, "value", entry.value.get(), depth + 3);
    }
}

void function(std::ostream& output, const ast::FunctionDeclaration& value,
              const std::size_t depth) {
    indent(output, depth);
    output << "function name " << std::quoted(value.name) << " return ";
    optional_text(output, value.return_type);
    output << " static " << value.is_static << " abstract " << value.is_abstract << " body "
           << value.has_body << ' ';
    span(output, value.span);
    output << '\n';
    annotations(output, value.annotations, depth + 1);
    parameters(output, value.parameters, value.rest_parameter, depth + 1);
    block(output, value.body, depth + 1);
}

void class_declaration(std::ostream& output, const ast::ClassDeclaration& value,
                       const std::size_t depth) {
    indent(output, depth);
    output << "class name " << std::quoted(value.name) << " base ";
    optional_text(output, value.base_type);
    output << ' ';
    span(output, value.span);
    output << '\n';
    annotations(output, value.annotations, depth + 1);
    indent(output, depth + 1);
    output << "variables " << value.variables.size() << '\n';
    for (const auto& item : value.variables)
        variable(output, item, depth + 2);
    indent(output, depth + 1);
    output << "enums " << value.enums.size() << '\n';
    for (const auto& item : value.enums)
        enumeration(output, item, depth + 2);
    indent(output, depth + 1);
    output << "signals " << value.signals.size() << '\n';
    for (const auto& item : value.signals)
        signal(output, item, depth + 2);
    indent(output, depth + 1);
    output << "functions " << value.functions.size() << '\n';
    for (const auto& item : value.functions)
        function(output, item, depth + 2);
    indent(output, depth + 1);
    output << "classes " << value.classes.size() << '\n';
    for (const auto& item : value.classes)
        class_declaration(output, item, depth + 2);
}

} // namespace

std::string AstSerializer::serialize(const ast::Script& script) const {
    std::ostringstream output;
    output << "GDPP_AST 1\nscript base ";
    optional_text(output, script.base_type);
    output << " class ";
    optional_text(output, script.class_name);
    output << " tool " << script.tool << ' ';
    span(output, script.span);
    output << '\n';
    annotations(output, script.annotations, 1);
    indent(output, 1);
    output << "variables " << script.variables.size() << '\n';
    for (const auto& item : script.variables)
        variable(output, item, 2);
    indent(output, 1);
    output << "enums " << script.enums.size() << '\n';
    for (const auto& item : script.enums)
        enumeration(output, item, 2);
    indent(output, 1);
    output << "signals " << script.signals.size() << '\n';
    for (const auto& item : script.signals)
        signal(output, item, 2);
    indent(output, 1);
    output << "functions " << script.functions.size() << '\n';
    for (const auto& item : script.functions)
        function(output, item, 2);
    indent(output, 1);
    output << "classes " << script.classes.size() << '\n';
    for (const auto& item : script.classes)
        class_declaration(output, item, 2);
    return output.str();
}

} // namespace gdpp
