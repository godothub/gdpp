#include "gdpp/frontend/parser.hpp"

#include "gdpp/frontend/language_features.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace gdpp {
namespace {

SourceSpan joined(const SourceSpan& first, const SourceSpan& last) {
    // Recovery may synthesize a missing trailing node at EOF while `previous()` still identifies
    // the last real token before trailing trivia.  Never let that earlier recovery endpoint turn
    // a child AST span backwards; valid input already has `last` at or after `first`.
    const auto end = last.end.offset < first.end.offset ? first.end : last.end;
    return {first.begin, end};
}

ast::ExpressionPtr make_expression(ast::ExpressionKind kind, std::string value, SourceSpan span,
                                   ast::LiteralKind literal_kind = ast::LiteralKind::none) {
    switch (kind) {
    case ast::ExpressionKind::literal:
        return std::make_unique<ast::Expression>(
            ast::LiteralExpression{literal_kind, std::move(value)}, span);
    case ast::ExpressionKind::identifier:
        return std::make_unique<ast::Expression>(ast::IdentifierExpression{std::move(value)}, span);
    case ast::ExpressionKind::unary:
        return std::make_unique<ast::Expression>(ast::UnaryExpression{std::move(value), nullptr},
                                                 span);
    case ast::ExpressionKind::await_expression:
        return std::make_unique<ast::Expression>(ast::AwaitExpression{}, span);
    case ast::ExpressionKind::binary:
        return std::make_unique<ast::Expression>(
            ast::BinaryExpression{std::move(value), nullptr, nullptr}, span);
    case ast::ExpressionKind::call:
        return std::make_unique<ast::Expression>(ast::CallExpression{}, span);
    case ast::ExpressionKind::member:
        return std::make_unique<ast::Expression>(ast::MemberExpression{nullptr, std::move(value)},
                                                 span);
    case ast::ExpressionKind::subscript:
        return std::make_unique<ast::Expression>(ast::SubscriptExpression{}, span);
    case ast::ExpressionKind::conditional:
        return std::make_unique<ast::Expression>(ast::ConditionalExpression{}, span);
    case ast::ExpressionKind::node_reference:
        return std::make_unique<ast::Expression>(ast::NodeReferenceExpression{std::move(value)},
                                                 span);
    case ast::ExpressionKind::array_literal:
        return std::make_unique<ast::Expression>(ast::ArrayExpression{}, span);
    case ast::ExpressionKind::dictionary_literal:
        return std::make_unique<ast::Expression>(ast::DictionaryExpression{}, span);
    case ast::ExpressionKind::lambda:
        return std::make_unique<ast::Expression>(ast::LambdaValueExpression{}, span);
    }
    return std::make_unique<ast::Expression>();
}

ast::ExpressionPtr balance_logical_chain(ast::ExpressionPtr expression) {
    if (!expression || expression->kind() != ast::ExpressionKind::binary)
        return expression;
    const auto operation = expression->value();
    if (operation != "and" && operation != "or")
        return expression;

    // Precedence parsing naturally creates a left-deep tree. A commercial script can contain
    // hundreds of generated guards, and recursively processing that shape exhausts Windows'
    // default 1 MiB thread stack. Logical operators are boolean, associative and short-circuit in
    // source order, so a balanced tree preserves observable evaluation while bounding every
    // compiler phase to O(log n) recursion depth.
    std::vector<ast::ExpressionPtr> pending;
    std::vector<ast::ExpressionPtr> operands;
    pending.push_back(std::move(expression));
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        auto* binary = current->get_if<ast::BinaryExpression>();
        if (binary && binary->operation == operation) {
            auto left = std::move(binary->left);
            auto right = std::move(binary->right);
            pending.push_back(std::move(right));
            pending.push_back(std::move(left));
        } else {
            operands.push_back(std::move(current));
        }
    }

    while (operands.size() > 1U) {
        std::vector<ast::ExpressionPtr> level;
        level.reserve((operands.size() + 1U) / 2U);
        for (std::size_t index = 0; index < operands.size(); index += 2U) {
            if (index + 1U == operands.size()) {
                level.push_back(std::move(operands[index]));
                continue;
            }
            auto binary =
                make_expression(ast::ExpressionKind::binary, operation,
                                joined(operands[index]->span, operands[index + 1U]->span));
            auto* value = binary->get_if<ast::BinaryExpression>();
            value->left = std::move(operands[index]);
            value->right = std::move(operands[index + 1U]);
            level.push_back(std::move(binary));
        }
        operands = std::move(level);
    }
    return std::move(operands.front());
}

bool is_assignment(TokenKind kind) {
    return kind == TokenKind::equal || kind == TokenKind::plus_equal ||
           kind == TokenKind::minus_equal || kind == TokenKind::star_equal ||
           kind == TokenKind::power_equal || kind == TokenKind::slash_equal ||
           kind == TokenKind::percent_equal || kind == TokenKind::shift_left_equal ||
           kind == TokenKind::shift_right_equal || kind == TokenKind::ampersand_equal ||
           kind == TokenKind::pipe_equal || kind == TokenKind::caret_equal;
}

void validate_annotation_targets(const std::vector<ast::Annotation>& annotations,
                                 const AnnotationTarget target, DiagnosticBag& diagnostics) {
    const auto& registry = LanguageFeatureRegistry::latest();
    for (const auto& annotation : annotations) {
        const auto* feature = registry.find_annotation(annotation.name);
        if (!feature) {
            diagnostics.error("GDS2021", "unknown annotation '@" + annotation.name + "'",
                              annotation.span);
        } else if (!has_annotation_target(feature->targets, target)) {
            diagnostics.error(
                "GDS2022", "annotation '@" + annotation.name + "' is not valid on this declaration",
                annotation.span);
        }
    }
}

} // namespace

Parser::Parser(const std::vector<Token>& tokens, DiagnosticBag& diagnostics, FrontendLimits limits)
    : tokens_(tokens), diagnostics_(diagnostics), limits_(limits) {}

Parser::DepthGuard::DepthGuard(Parser& parser, SourceSpan span) : parser_(parser) {
    if (parser_.recursion_depth_ >= parser_.limits_.max_parser_depth) {
        if (!parser_.recursion_limit_reported_) {
            parser_.diagnostics_.error("GDS2024", "parser recursion exceeds the configured limit",
                                       span);
            parser_.recursion_limit_reported_ = true;
        }
        return;
    }
    ++parser_.recursion_depth_;
    active_ = true;
}

Parser::DepthGuard::~DepthGuard() {
    if (active_)
        --parser_.recursion_depth_;
}

const Token& Parser::current() const noexcept { return tokens_[position_]; }

const Token& Parser::previous() const noexcept {
    return tokens_[position_ == 0 ? 0 : position_ - 1];
}

bool Parser::at_end() const noexcept { return current().kind == TokenKind::end_of_file; }

bool Parser::check(TokenKind kind) const noexcept { return current().kind == kind; }

bool Parser::check_soft_identifier() const noexcept {
    return check(TokenKind::identifier) || check(TokenKind::kw_match) || check(TokenKind::kw_when);
}

bool Parser::check_attribute_name() const noexcept {
    return check(TokenKind::identifier) ||
           (current().kind >= TokenKind::kw_extends && current().kind <= TokenKind::kw_await);
}

bool Parser::is_match_statement_ahead() const noexcept {
    if (!check(TokenKind::kw_match))
        return false;
    std::size_t grouping_depth = 0;
    for (std::size_t index = position_ + 1; index < tokens_.size(); ++index) {
        switch (tokens_[index].kind) {
        case TokenKind::left_paren:
        case TokenKind::left_bracket:
        case TokenKind::left_brace:
            ++grouping_depth;
            break;
        case TokenKind::right_paren:
        case TokenKind::right_bracket:
        case TokenKind::right_brace:
            if (grouping_depth > 0)
                --grouping_depth;
            break;
        case TokenKind::colon:
            if (grouping_depth == 0)
                return true;
            break;
        case TokenKind::newline:
        case TokenKind::semicolon:
        case TokenKind::dedent:
        case TokenKind::end_of_file:
            if (grouping_depth == 0)
                return false;
            break;
        default:
            break;
        }
    }
    return false;
}

bool Parser::match(TokenKind kind) noexcept {
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

bool Parser::match_inferred_assignment() noexcept {
    if (match(TokenKind::colon_equal))
        return true;
    if (check(TokenKind::colon) && position_ + 1 < tokens_.size() &&
        tokens_[position_ + 1].kind == TokenKind::equal) {
        advance();
        advance();
        return true;
    }
    return false;
}

const Token& Parser::advance() noexcept {
    if (!at_end()) {
        ++position_;
    }
    return previous();
}

const Token& Parser::consume(TokenKind kind, const char* message) {
    if (check(kind)) {
        return advance();
    }
    diagnostics_.error("GDS2001",
                       std::string{message} + "; found " + token_kind_name(current().kind),
                       current().span);
    if (!at_end()) {
        return advance();
    }
    return current();
}

const Token& Parser::consume_soft_identifier(const char* message) {
    if (check_soft_identifier())
        return advance();
    return consume(TokenKind::identifier, message);
}

const Token& Parser::consume_attribute_name(const char* message) {
    if (check_attribute_name())
        return advance();
    return consume(TokenKind::identifier, message);
}

void Parser::skip_newlines() noexcept {
    while (match(TokenKind::newline) || match(TokenKind::semicolon)) {
    }
}

void Parser::synchronize() noexcept {
    const auto begin = position_;
    while (!at_end() && !check(TokenKind::newline) && !check(TokenKind::dedent)) {
        advance();
    }
    while (match(TokenKind::newline) || match(TokenKind::semicolon) || match(TokenKind::dedent) ||
           match(TokenKind::indent)) {
    }
    if (position_ == begin && !at_end())
        advance();
}

std::string Parser::parse_type_name(const char* message) {
    const DepthGuard depth{*this, current().span};
    if (!depth) {
        if (!at_end())
            advance();
        return "Variant";
    }
    const auto& name = consume(TokenKind::identifier, message);
    std::string type = name.lexeme;
    while (match(TokenKind::dot)) {
        type += '.';
        type += consume(TokenKind::identifier, "expected a name after '.'").lexeme;
    }
    if (match(TokenKind::left_bracket)) {
        type += '[';
        type += parse_type_name("expected an element type");
        if (match(TokenKind::comma)) {
            type += ", ";
            type += parse_type_name("expected a value type after ','");
        }
        consume(TokenKind::right_bracket, "expected ']' after element type");
        type += ']';
    }
    return type;
}

std::optional<std::string> Parser::parse_type_annotation() {
    if (!check(TokenKind::colon) || position_ + 1 >= tokens_.size() ||
        tokens_[position_ + 1].kind == TokenKind::newline) {
        return std::nullopt;
    }
    advance();
    return parse_type_name("expected a type name after ':'");
}

void Parser::parse_property_accessors(ast::VariableDeclaration& declaration) {
    consume(TokenKind::newline, "expected a newline before property accessors");
    while (match(TokenKind::newline)) {
    }
    consume(TokenKind::indent, "expected indented property accessors");
    skip_newlines();
    if (check(TokenKind::identifier) && position_ + 1 < tokens_.size() &&
        tokens_[position_ + 1].kind == TokenKind::equal) {
        parse_bound_property_accessors(declaration, true);
        return;
    }
    while (!check(TokenKind::dedent) && !at_end()) {
        const auto begin = current().span;
        const auto name =
            consume(TokenKind::identifier, "expected 'get' or 'set' property accessor");
        ast::PropertyAccessor accessor;
        if (name.lexeme == "set") {
            consume(TokenKind::left_paren, "expected '(' after 'set'");
            accessor.parameter =
                consume(TokenKind::identifier, "expected a setter parameter name").lexeme;
            consume(TokenKind::right_paren, "expected ')' after setter parameter");
        } else if (name.lexeme == "get") {
            if (match(TokenKind::left_paren))
                consume(TokenKind::right_paren, "expected ')' after 'get('");
        } else {
            diagnostics_.error("GDS2011", "expected 'get' or 'set' property accessor", name.span);
        }
        consume(TokenKind::colon, "expected ':' after property accessor");
        accessor.body = parse_block();
        accessor.span = joined(begin, previous().span);
        if (name.lexeme == "get") {
            if (declaration.getter) {
                diagnostics_.error("GDS2012", "duplicate property getter", name.span);
            } else {
                declaration.getter = std::move(accessor);
            }
        } else if (name.lexeme == "set") {
            if (declaration.setter) {
                diagnostics_.error("GDS2013", "duplicate property setter", name.span);
            } else {
                declaration.setter = std::move(accessor);
            }
        }
        skip_newlines();
    }
    consume(TokenKind::dedent, "expected the end of property accessors");
}

void Parser::parse_bound_property_accessors(ast::VariableDeclaration& declaration, bool indented) {
    bool expect_another = true;
    for (std::size_t count = 0; count < 2 && expect_another && !at_end(); ++count) {
        const auto begin = current().span;
        const auto& name =
            consume(TokenKind::identifier, "expected 'get' or 'set' property accessor");
        consume(TokenKind::equal, "expected '=' after bound property accessor");
        const auto& method =
            consume(TokenKind::identifier, "expected a method name after property accessor '='");
        ast::PropertyAccessor accessor;
        accessor.method = method.lexeme;
        accessor.span = joined(begin, method.span);
        if (name.lexeme == "get") {
            if (declaration.getter) {
                diagnostics_.error("GDS2012", "duplicate property getter", name.span);
            } else {
                declaration.getter = std::move(accessor);
            }
        } else if (name.lexeme == "set") {
            if (declaration.setter) {
                diagnostics_.error("GDS2013", "duplicate property setter", name.span);
            } else {
                declaration.setter = std::move(accessor);
            }
        } else {
            diagnostics_.error("GDS2011", "expected 'get' or 'set' property accessor", name.span);
        }
        expect_another = match(TokenKind::comma);
        if (expect_another && indented)
            skip_newlines();
    }

    if (indented) {
        skip_newlines();
        consume(TokenKind::dedent, "expected the end of property accessors");
    }
}

ast::Annotation Parser::parse_annotation() {
    const auto begin = previous().span;
    ast::Annotation annotation;
    annotation.name =
        consume(TokenKind::identifier, "expected an annotation name after '@'").lexeme;
    if (match(TokenKind::left_paren)) {
        while (!check(TokenKind::right_paren) && !at_end()) {
            annotation.arguments.push_back(parse_expression());
            if (!match(TokenKind::comma) || check(TokenKind::right_paren))
                break;
        }
        consume(TokenKind::right_paren, "expected ')' after annotation arguments");
    }
    annotation.span = joined(begin, previous().span);
    if (const auto* feature = LanguageFeatureRegistry::latest().find_annotation(annotation.name)) {
        if (annotation.arguments.size() < feature->minimum_arguments ||
            annotation.arguments.size() > feature->maximum_arguments) {
            diagnostics_.error("GDS2020",
                               "@" + annotation.name + " expects " +
                                   (feature->minimum_arguments == feature->maximum_arguments
                                        ? std::to_string(feature->minimum_arguments)
                                        : std::to_string(feature->minimum_arguments) + " to " +
                                              (feature->maximum_arguments ==
                                                       std::numeric_limits<std::uint16_t>::max()
                                                   ? std::string{"any number of"}
                                                   : std::to_string(feature->maximum_arguments))) +
                                   " argument(s), got " +
                                   std::to_string(annotation.arguments.size()),
                               annotation.span);
        }
    }
    if (annotation.name == "warning_ignore" || annotation.name == "warning_ignore_start" ||
        annotation.name == "warning_ignore_restore") {
        const auto& registry = LanguageFeatureRegistry::latest();
        for (const auto& argument : annotation.arguments) {
            if (argument->kind() != ast::ExpressionKind::literal ||
                argument->literal_kind() != ast::LiteralKind::string) {
                diagnostics_.error("GDS2016", "warning directives expect string literals",
                                   argument->span);
            } else if (!registry.is_warning_name(argument->value())) {
                diagnostics_.error("GDS2034",
                                   "unknown GDScript warning '" + argument->value() + "'",
                                   argument->span);
            }
        }
    }
    return annotation;
}

void Parser::apply_warning_directive(const ast::Annotation& annotation) {
    if (annotation.name != "warning_ignore_start" && annotation.name != "warning_ignore_restore") {
        return;
    }
    for (const auto& argument : annotation.arguments) {
        if (argument->kind() != ast::ExpressionKind::literal ||
            argument->literal_kind() != ast::LiteralKind::string) {
            continue;
        }
        const auto& warning = argument->value();
        if (annotation.name == "warning_ignore_start") {
            if (!ignored_warning_ranges_.emplace(warning, annotation.span).second) {
                diagnostics_.error("GDS2029",
                                   "warning '" + warning +
                                       "' is already ignored by an active "
                                       "@warning_ignore_start range",
                                   argument->span);
            }
        } else if (ignored_warning_ranges_.erase(warning) == 0U) {
            diagnostics_.error("GDS2029",
                               "warning '" + warning +
                                   "' is not ignored by an active @warning_ignore_start range",
                               argument->span);
        }
    }
}

void Parser::apply_active_warning_ignores(ast::Statement& statement) const {
    std::vector<std::pair<std::string, SourceSpan>> active;
    active.reserve(ignored_warning_ranges_.size());
    for (const auto& entry : ignored_warning_ranges_)
        active.push_back(entry);
    std::sort(active.begin(), active.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& entry : active) {
        const auto& warning = entry.first;
        const auto& span = entry.second;
        const bool already_present = std::any_of(
            statement.annotations.begin(), statement.annotations.end(),
            [&](const ast::Annotation& annotation) {
                return annotation.name == "warning_ignore" && annotation.arguments.size() == 1 &&
                       annotation.arguments.front()->kind() == ast::ExpressionKind::literal &&
                       annotation.arguments.front()->value() == warning;
            });
        if (already_present)
            continue;
        ast::Annotation annotation;
        annotation.name = "warning_ignore";
        annotation.span = span;
        annotation.arguments.push_back(std::make_unique<ast::Expression>(
            ast::LiteralExpression{ast::LiteralKind::string, warning}, span));
        statement.annotations.push_back(std::move(annotation));
    }
}

Parser::ParsedParameters Parser::parse_parameters(const std::string_view owner,
                                                  const bool allow_defaults,
                                                  const bool allow_rest) {
    ParsedParameters parameters;
    std::unordered_map<std::string, SourceSpan> declared_names;
    bool optional_parameter_seen = false;
    consume(TokenKind::left_paren, "expected '('");
    while (!check(TokenKind::right_paren) && !at_end()) {
        const auto begin = current().span;
        const bool is_rest = match(TokenKind::ellipsis);
        ast::Parameter parameter;
        parameter.name = consume_soft_identifier("expected a parameter name").lexeme;
        if (match_inferred_assignment()) {
            parameter.infer_type = true;
            parameter.default_value = parse_expression();
        } else {
            parameter.type = parse_type_annotation();
            if (match(TokenKind::equal))
                parameter.default_value = parse_expression();
        }
        parameter.span = joined(begin, previous().span);
        const bool has_default = parameter.default_value != nullptr;
        bool accepted = true;
        if (parameters.rest) {
            diagnostics_.error(
                "GDS2037", "parameters cannot follow the rest parameter in a " + std::string{owner},
                parameter.span);
            accepted = false;
        }
        if (is_rest && !allow_rest) {
            diagnostics_.error("GDS2036",
                               std::string{owner} + " declarations cannot have a rest parameter",
                               parameter.span);
            accepted = false;
        } else if (is_rest && has_default) {
            diagnostics_.error("GDS2038", "the rest parameter cannot have a default value",
                               parameter.span);
            accepted = false;
        } else if (has_default && !allow_defaults) {
            diagnostics_.error("GDS2033",
                               std::string{owner} + " parameters cannot have a default value",
                               parameter.span);
        } else if (!is_rest && !has_default && optional_parameter_seen) {
            diagnostics_.error("GDS2034",
                               "mandatory parameters cannot follow optional parameters in a " +
                                   std::string{owner},
                               parameter.span);
            accepted = false;
        }
        if (!declared_names.emplace(parameter.name, parameter.span).second) {
            diagnostics_.error("GDS2035",
                               "parameter '" + parameter.name + "' was already declared for this " +
                                   std::string{owner},
                               parameter.span);
            accepted = false;
        }
        optional_parameter_seen = optional_parameter_seen || (has_default && !is_rest);
        if (accepted) {
            if (is_rest)
                parameters.rest = std::move(parameter);
            else
                parameters.positional.push_back(std::move(parameter));
        }
        if (!match(TokenKind::comma) || check(TokenKind::right_paren)) {
            break;
        }
    }
    consume(TokenKind::right_paren, "expected ')' after parameters");
    return parameters;
}

ast::VariableDeclaration Parser::parse_variable(bool is_constant,
                                                std::vector<ast::Annotation> annotations) {
    const auto begin = previous().span;
    ast::VariableDeclaration declaration;
    declaration.is_constant = is_constant;
    declaration.annotations = std::move(annotations);
    declaration.onready =
        std::any_of(declaration.annotations.begin(), declaration.annotations.end(),
                    [](const ast::Annotation& annotation) { return annotation.name == "onready"; });
    declaration.name = consume_soft_identifier("expected a variable name").lexeme;
    const bool property_without_type =
        check(TokenKind::colon) && position_ + 1 < tokens_.size() &&
        tokens_[position_ + 1].kind == TokenKind::identifier &&
        (tokens_[position_ + 1].lexeme == "get" || tokens_[position_ + 1].lexeme == "set");
    if (property_without_type) {
        advance();
    } else if (match_inferred_assignment()) {
        declaration.infer_type = true;
        declaration.initializer = parse_expression();
    } else {
        declaration.type = parse_type_annotation();
        if (match(TokenKind::equal)) {
            declaration.initializer = parse_expression();
        } else if (is_constant) {
            diagnostics_.error("GDS2002", "a constant requires an initializer", current().span);
        }
    }
    const bool has_accessors = property_without_type || match(TokenKind::colon);
    if (has_accessors) {
        if (is_constant) {
            diagnostics_.error("GDS2014", "constants cannot declare property accessors",
                               previous().span);
        }
        if (check(TokenKind::newline))
            parse_property_accessors(declaration);
        else
            parse_bound_property_accessors(declaration, false);
    }
    declaration.span = joined(begin, previous().span);
    if (has_accessors) {
        skip_newlines();
    } else if (match(TokenKind::semicolon)) {
        skip_newlines();
    } else if (!check(TokenKind::newline) && !check(TokenKind::end_of_file)) {
        diagnostics_.error(
            check(TokenKind::comma) ? "GDS2032" : "GDS2003",
            check(TokenKind::comma)
                ? "GDScript declares one variable per statement; start a new declaration"
                : "unexpected token after variable declaration",
            current().span);
        synchronize();
    } else {
        skip_newlines();
    }
    return declaration;
}

ast::SignalDeclaration Parser::parse_signal(std::vector<ast::Annotation> annotations) {
    const auto begin = previous().span;
    ast::SignalDeclaration declaration;
    declaration.annotations = std::move(annotations);
    declaration.name = consume(TokenKind::identifier, "expected a signal name").lexeme;
    if (check(TokenKind::left_paren)) {
        declaration.parameters = parse_parameters("signal", false, false).positional;
    }
    declaration.span = joined(begin, previous().span);
    if (!check(TokenKind::newline)) {
        diagnostics_.error("GDS2004", "expected end of line after signal", current().span);
        synchronize();
    } else {
        skip_newlines();
    }
    return declaration;
}

ast::EnumDeclaration Parser::parse_enum(std::vector<ast::Annotation> annotations) {
    const auto begin = previous().span;
    ast::EnumDeclaration declaration;
    declaration.annotations = std::move(annotations);
    if (check(TokenKind::identifier))
        declaration.name = advance().lexeme;
    consume(TokenKind::left_brace, "expected '{' after enum name");
    while (!check(TokenKind::right_brace) && !at_end()) {
        const auto entry_begin = current().span;
        ast::EnumEntry entry;
        entry.name = consume_soft_identifier("expected an enum member name").lexeme;
        if (match(TokenKind::equal))
            entry.value = parse_expression();
        entry.span = joined(entry_begin, previous().span);
        declaration.entries.push_back(std::move(entry));
        if (!match(TokenKind::comma) || check(TokenKind::right_brace))
            break;
    }
    const auto& end = consume(TokenKind::right_brace, "expected '}' after enum members");
    declaration.span = joined(begin, end.span);
    if (!check(TokenKind::newline)) {
        diagnostics_.error("GDS2010", "unexpected token after enum declaration", current().span);
        synchronize();
    } else {
        skip_newlines();
    }
    return declaration;
}

ast::FunctionDeclaration Parser::parse_function(bool is_static,
                                                std::vector<ast::Annotation> annotations) {
    const auto begin = previous().span;
    ast::FunctionDeclaration function;
    function.is_static = is_static;
    function.annotations = std::move(annotations);
    function.is_abstract =
        std::any_of(function.annotations.begin(), function.annotations.end(),
                    [](const auto& annotation) { return annotation.name == "abstract"; });
    function.name = consume(TokenKind::identifier, "expected a function name").lexeme;
    auto parameters = parse_parameters("function");
    function.parameters = std::move(parameters.positional);
    function.rest_parameter = std::move(parameters.rest);
    if (match(TokenKind::arrow)) {
        function.return_type = parse_type_name("expected a return type after '->'");
    }
    function.has_body = match(TokenKind::colon);
    if (function.has_body) {
        function.body = parse_suite();
    } else if (!check(TokenKind::newline) && !check(TokenKind::dedent) && !at_end()) {
        diagnostics_.error("GDS2033", "unexpected token after bodyless function signature",
                           current().span);
        synchronize();
    }
    function.span = joined(begin, previous().span);
    return function;
}

void Parser::parse_class_member(ast::ClassDeclaration& declaration,
                                std::vector<ast::Annotation>& annotations) {
    if (match(TokenKind::string)) {
        // Godot permits standalone strings as documentation/comment blocks in class suites.
        annotations.clear();
        skip_newlines();
    } else if (match(TokenKind::kw_extends)) {
        if (!annotations.empty()) {
            diagnostics_.error("GDS2008", "annotations are not valid before 'extends'",
                               annotations.front().span);
            annotations.clear();
        }
        declaration.base_type =
            parse_type_name("expected an internal class base type after 'extends'");
        skip_newlines();
    } else if (match(TokenKind::kw_var)) {
        validate_annotation_targets(annotations, AnnotationTarget::field, diagnostics_);
        declaration.variables.push_back(parse_variable(false, std::move(annotations)));
        annotations.clear();
    } else if (match(TokenKind::kw_const)) {
        validate_annotation_targets(annotations, AnnotationTarget::field, diagnostics_);
        declaration.variables.push_back(parse_variable(true, std::move(annotations)));
        annotations.clear();
    } else if (match(TokenKind::kw_signal)) {
        validate_annotation_targets(annotations, AnnotationTarget::signal, diagnostics_);
        declaration.signals.push_back(parse_signal(std::move(annotations)));
        annotations.clear();
    } else if (match(TokenKind::kw_enum)) {
        validate_annotation_targets(annotations, AnnotationTarget::enumeration, diagnostics_);
        declaration.enums.push_back(parse_enum(std::move(annotations)));
        annotations.clear();
    } else if (match(TokenKind::kw_static)) {
        if (match(TokenKind::kw_var)) {
            validate_annotation_targets(annotations, AnnotationTarget::field, diagnostics_);
            auto variable = parse_variable(false, std::move(annotations));
            variable.is_static = true;
            declaration.variables.push_back(std::move(variable));
            annotations.clear();
        } else {
            validate_annotation_targets(annotations, AnnotationTarget::function, diagnostics_);
            consume(TokenKind::kw_func, "expected 'var' or 'func' after 'static'");
            declaration.functions.push_back(parse_function(true, std::move(annotations)));
            annotations.clear();
        }
    } else if (match(TokenKind::kw_func)) {
        validate_annotation_targets(annotations, AnnotationTarget::function, diagnostics_);
        declaration.functions.push_back(parse_function(false, std::move(annotations)));
        annotations.clear();
    } else if (match(TokenKind::kw_class)) {
        validate_annotation_targets(annotations, AnnotationTarget::inner_class, diagnostics_);
        declaration.classes.push_back(parse_class(std::move(annotations)));
        annotations.clear();
    } else {
        annotations.clear();
        diagnostics_.error("GDS2018", "unsupported internal class declaration", current().span);
        synchronize();
    }
}

ast::ClassDeclaration Parser::parse_class(std::vector<ast::Annotation> annotations) {
    const DepthGuard depth{*this, current().span};
    if (!depth) {
        synchronize();
        return {};
    }
    const auto begin = previous().span;
    ast::ClassDeclaration declaration;
    declaration.annotations = std::move(annotations);
    declaration.name = consume_soft_identifier("expected an internal class name").lexeme;
    if (match(TokenKind::kw_extends)) {
        declaration.base_type =
            parse_type_name("expected an internal class base type after 'extends'");
    }
    consume(TokenKind::colon, "expected ':' after internal class name");
    if (!check(TokenKind::newline)) {
        std::vector<ast::Annotation> pending_annotations;
        if (!match(TokenKind::kw_pass))
            parse_class_member(declaration, pending_annotations);
        declaration.span = joined(begin, previous().span);
        return declaration;
    }
    consume(TokenKind::newline, "expected a newline before internal class body");
    while (match(TokenKind::newline)) {
    }
    consume(TokenKind::indent, "expected an indented internal class body");
    skip_newlines();
    std::vector<ast::Annotation> pending_annotations;
    while (!check(TokenKind::dedent) && !at_end()) {
        if (match(TokenKind::at_sign)) {
            pending_annotations.push_back(parse_annotation());
            if (match(TokenKind::newline))
                skip_newlines();
            continue;
        }
        if (match(TokenKind::kw_pass)) {
            if (!pending_annotations.empty()) {
                diagnostics_.error("GDS2009", "annotation is not attached to a declaration",
                                   pending_annotations.front().span);
                pending_annotations.clear();
            }
            skip_newlines();
            continue;
        }
        parse_class_member(declaration, pending_annotations);
        skip_newlines();
    }
    consume(TokenKind::dedent, "expected the end of an internal class body");
    if (!pending_annotations.empty()) {
        diagnostics_.error("GDS2009", "annotation is not attached to a declaration",
                           pending_annotations.front().span);
    }
    declaration.span = joined(begin, previous().span);
    return declaration;
}

std::vector<ast::Statement> Parser::parse_block() {
    consume(TokenKind::newline, "expected a newline before block");
    while (match(TokenKind::newline)) {
    }
    consume(TokenKind::indent, "expected an indented block");
    std::vector<ast::Statement> statements;
    skip_newlines();
    while (!check(TokenKind::dedent) && !at_end()) {
        auto statement = parse_statement();
        apply_active_warning_ignores(statement);
        statements.push_back(std::move(statement));
        skip_newlines();
    }
    consume(TokenKind::dedent, "expected the end of an indented block");
    return statements;
}

std::vector<ast::Statement> Parser::parse_suite() {
    if (check(TokenKind::newline))
        return parse_block();
    std::vector<ast::Statement> statements;
    do {
        auto statement = parse_statement();
        apply_active_warning_ignores(statement);
        statements.push_back(std::move(statement));
    } while (match(TokenKind::semicolon) && !check(TokenKind::newline) &&
             !check(TokenKind::dedent) && !at_end());
    return statements;
}

ast::Statement Parser::parse_variable_statement(const bool is_constant) {
    const auto begin = previous().span;
    ast::VariableStatement variable;
    variable.is_constant = is_constant;
    variable.name = consume_soft_identifier("expected a local variable name").lexeme;
    const auto parse_initializer = [&] { variable.initializer = parse_expression(); };
    if (match_inferred_assignment()) {
        variable.infer_type = true;
        parse_initializer();
    } else {
        variable.type = parse_type_annotation();
        if (match(TokenKind::equal)) {
            parse_initializer();
        }
    }
    if (is_constant && !variable.initializer) {
        diagnostics_.error("GDS2002", "a local constant requires an initializer", current().span);
    }
    if (is_constant && variable.initializer &&
        variable.initializer->kind() == ast::ExpressionKind::await_expression) {
        diagnostics_.error("GDS2025", "a local constant initializer cannot await a signal",
                           variable.initializer->span);
    }
    if (check(TokenKind::comma)) {
        diagnostics_.error("GDS2032",
                           "GDScript declares one variable per statement; start a new declaration",
                           current().span);
        synchronize();
    }
    return ast::Statement{std::move(variable), joined(begin, previous().span)};
}

ast::Statement Parser::parse_if_statement() {
    const auto begin = previous().span;
    ast::IfStatement conditional;
    conditional.condition = parse_expression();
    consume(TokenKind::colon, "expected ':' after condition");
    conditional.when_true = parse_suite();
    skip_newlines();
    if (match(TokenKind::kw_elif)) {
        conditional.when_false.push_back(parse_if_statement());
    } else if (match(TokenKind::kw_else)) {
        consume(TokenKind::colon, "expected ':' after else");
        conditional.when_false = parse_suite();
    }
    return ast::Statement{std::move(conditional), joined(begin, previous().span)};
}

std::unique_ptr<ast::MatchPattern> Parser::parse_match_pattern() {
    auto pattern = std::make_unique<ast::MatchPattern>();
    const auto begin = current().span;
    if (match(TokenKind::kw_var)) {
        const auto& name = consume_soft_identifier("expected a match binding name");
        pattern->node = ast::BindingPattern{name.lexeme};
        pattern->span = joined(begin, name.span);
        return pattern;
    }
    if (check(TokenKind::identifier) && current().lexeme == "_") {
        pattern->node = ast::WildcardPattern{};
        pattern->span = advance().span;
        return pattern;
    }
    if (match(TokenKind::dot_dot)) {
        const auto end = previous().span;
        pattern->node = ast::RestPattern{};
        pattern->span = joined(begin, end);
        return pattern;
    }
    if (match(TokenKind::left_bracket)) {
        pattern->node = ast::ArrayPattern{};
        while (!check(TokenKind::right_bracket) && !at_end()) {
            pattern->elements.push_back(parse_match_pattern());
            if (!match(TokenKind::comma) || check(TokenKind::right_bracket))
                break;
        }
        const auto& end = consume(TokenKind::right_bracket, "expected ']' after array pattern");
        pattern->span = joined(begin, end.span);
        return pattern;
    }
    if (match(TokenKind::left_brace)) {
        pattern->node = ast::DictionaryPattern{};
        while (!check(TokenKind::right_brace) && !at_end()) {
            if (check(TokenKind::dot_dot)) {
                pattern->keys.push_back(nullptr);
                pattern->elements.push_back(parse_match_pattern());
            } else {
                pattern->keys.push_back(parse_expression());
                if (match(TokenKind::colon)) {
                    pattern->elements.push_back(parse_match_pattern());
                } else {
                    auto wildcard = std::make_unique<ast::MatchPattern>();
                    wildcard->node = ast::WildcardPattern{};
                    wildcard->span = pattern->keys.back()->span;
                    pattern->elements.push_back(std::move(wildcard));
                }
            }
            if (!match(TokenKind::comma) || check(TokenKind::right_brace))
                break;
        }
        const auto& end = consume(TokenKind::right_brace, "expected '}' after dictionary pattern");
        pattern->span = joined(begin, end.span);
        return pattern;
    }
    auto expression = parse_expression();
    pattern->span = expression->span;
    pattern->node = ast::ValuePattern{std::move(expression)};
    return pattern;
}

ast::MatchBranch Parser::parse_match_branch() {
    const auto begin = current().span;
    ast::MatchBranch branch;
    while (!check(TokenKind::colon) && !check(TokenKind::kw_when) && !at_end()) {
        auto pattern = parse_match_pattern();
        branch.patterns.push_back(std::move(*pattern));
        if (!match(TokenKind::comma))
            break;
    }
    if (match(TokenKind::kw_when))
        branch.guard = parse_expression();
    consume(TokenKind::colon, "expected ':' after match pattern");
    branch.body = parse_suite();
    branch.span = joined(begin, previous().span);
    return branch;
}

ast::Statement Parser::parse_match_statement() {
    const auto begin = previous().span;
    ast::MatchStatement match_statement;
    match_statement.subject = parse_expression();
    consume(TokenKind::colon, "expected ':' after match value");
    consume(TokenKind::newline, "expected a newline before match branches");
    while (match(TokenKind::newline)) {
    }
    consume(TokenKind::indent, "expected indented match branches");
    skip_newlines();
    if (match(TokenKind::kw_pass))
        skip_newlines();
    while (!check(TokenKind::dedent) && !at_end()) {
        match_statement.branches.push_back(parse_match_branch());
        skip_newlines();
    }
    consume(TokenKind::dedent, "expected the end of match branches");
    return ast::Statement{std::move(match_statement), joined(begin, previous().span)};
}

ast::Statement Parser::parse_for_statement() {
    const auto begin = previous().span;
    ast::ForStatement loop;
    const auto& iterator = consume(TokenKind::identifier, "expected an iterator variable");
    loop.iterator = iterator.lexeme;
    loop.iterator_span = iterator.span;
    SourceSpan type_begin{};
    const bool has_type_begin = check(TokenKind::colon) && position_ + 1 < tokens_.size();
    if (has_type_begin)
        type_begin = tokens_[position_ + 1].span;
    loop.type = parse_type_annotation();
    if (loop.type && has_type_begin)
        loop.type_span = joined(type_begin, previous().span);
    consume(TokenKind::kw_in, "expected 'in' after iterator variable");
    loop.iterable = parse_expression();
    consume(TokenKind::colon, "expected ':' after iterable expression");
    loop.body = parse_suite();
    return ast::Statement{std::move(loop), joined(begin, previous().span)};
}

ast::Statement Parser::parse_while_statement() {
    const auto begin = previous().span;
    ast::WhileStatement loop;
    loop.condition = parse_expression();
    consume(TokenKind::colon, "expected ':' after while condition");
    loop.body = parse_suite();
    return ast::Statement{std::move(loop), joined(begin, previous().span)};
}

ast::Statement Parser::parse_assert_statement() {
    const auto begin = previous().span;
    ast::AssertStatement assertion;
    consume(TokenKind::left_paren, "expected '(' after 'assert'");
    assertion.condition = parse_expression();
    if (match(TokenKind::comma) && !check(TokenKind::right_paren)) {
        assertion.message = parse_expression();
        (void)match(TokenKind::comma);
    }
    const auto& end = consume(TokenKind::right_paren, "expected ')' after assert arguments");
    return ast::Statement{std::move(assertion), joined(begin, end.span)};
}

ast::Statement Parser::parse_statement() {
    const auto fallback_span = current().span;
    const DepthGuard depth{*this, fallback_span};
    if (!depth) {
        synchronize();
        return ast::Statement{ast::PassStatement{}, fallback_span};
    }
    if (match(TokenKind::at_sign)) {
        auto annotation = parse_annotation();
        const bool warning_range = annotation.name == "warning_ignore_start" ||
                                   annotation.name == "warning_ignore_restore";
        const bool warning_annotation = annotation.name == "warning_ignore" || warning_range;
        if (!warning_annotation) {
            diagnostics_.error("GDS2015",
                               "unsupported statement annotation '@" + annotation.name + "'",
                               annotation.span);
        }
        consume(TokenKind::newline, "expected a newline after statement annotation");
        if (warning_range)
            apply_warning_directive(annotation);
        skip_newlines();
        if (check(TokenKind::dedent) || at_end()) {
            if (!warning_range) {
                diagnostics_.error("GDS2017", "statement annotation is not followed by a statement",
                                   annotation.span);
            }
            return ast::Statement{ast::PassStatement{}, annotation.span};
        }
        auto statement = parse_statement();
        if (!warning_range)
            statement.annotations.push_back(std::move(annotation));
        return statement;
    }
    if (match(TokenKind::kw_return)) {
        ast::ReturnStatement result;
        auto span = previous().span;
        if (!check(TokenKind::newline) && !check(TokenKind::semicolon) &&
            !check(TokenKind::dedent)) {
            result.value = parse_expression();
            span.end = result.value->span.end;
        }
        return ast::Statement{std::move(result), span};
    }
    if (match(TokenKind::kw_assert))
        return parse_assert_statement();
    if (match(TokenKind::kw_var))
        return parse_variable_statement(false);
    if (match(TokenKind::kw_const))
        return parse_variable_statement(true);
    if (match(TokenKind::kw_if))
        return parse_if_statement();
    // `match` is a soft keyword in Godot. A top-level ':' before the statement terminator is the
    // unambiguous grammar boundary; postfix syntax alone cannot distinguish `match[0]` from a
    // match statement whose subject begins with an array literal.
    if (is_match_statement_ahead()) {
        advance();
        return parse_match_statement();
    }
    if (match(TokenKind::kw_while))
        return parse_while_statement();
    if (match(TokenKind::kw_for))
        return parse_for_statement();
    if (match(TokenKind::kw_pass))
        return ast::Statement{ast::PassStatement{}, previous().span};
    if (match(TokenKind::kw_break))
        return ast::Statement{ast::BreakStatement{}, previous().span};
    if (match(TokenKind::kw_continue))
        return ast::Statement{ast::ContinueStatement{}, previous().span};
    if (match(TokenKind::kw_breakpoint))
        return ast::Statement{ast::BreakpointStatement{}, previous().span};

    const auto begin = current().span;
    auto expression = parse_expression();
    if (is_assignment(current().kind)) {
        ast::AssignmentStatement assignment;
        assignment.operation = advance().lexeme;
        assignment.target = std::move(expression);
        assignment.value = parse_expression();
        const auto span = joined(begin, assignment.value->span);
        return ast::Statement{std::move(assignment), span};
    }
    const auto span = expression->span;
    if (expression->kind() == ast::ExpressionKind::lambda) {
        diagnostics_.error("GDS2030",
                           "standalone lambdas cannot be accessed; assign the lambda to a variable",
                           span);
    }
    return ast::Statement{ast::ExpressionStatement{std::move(expression)}, span};
}

int Parser::precedence(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::kw_or:
        return 1;
    case TokenKind::kw_and:
        return 2;
    case TokenKind::equal_equal:
    case TokenKind::bang_equal:
    case TokenKind::less:
    case TokenKind::less_equal:
    case TokenKind::greater:
    case TokenKind::greater_equal:
    case TokenKind::kw_in:
    case TokenKind::kw_is:
    case TokenKind::kw_as:
        return 3;
    case TokenKind::pipe:
        return 4;
    case TokenKind::caret:
        return 5;
    case TokenKind::ampersand:
        return 6;
    case TokenKind::shift_left:
    case TokenKind::shift_right:
        return 7;
    case TokenKind::plus:
    case TokenKind::minus:
        return 8;
    case TokenKind::star:
    case TokenKind::slash:
    case TokenKind::percent:
        return 9;
    case TokenKind::power:
        return 10;
    default:
        return -1;
    }
}

bool Parser::is_binary_operator(TokenKind kind) noexcept { return precedence(kind) >= 0; }

ast::ExpressionPtr Parser::parse_prefix() {
    const auto token = advance();
    switch (token.kind) {
    case TokenKind::integer:
        return make_expression(ast::ExpressionKind::literal, token.lexeme, token.span,
                               ast::LiteralKind::integer);
    case TokenKind::floating:
        return make_expression(ast::ExpressionKind::literal, token.lexeme, token.span,
                               ast::LiteralKind::floating);
    case TokenKind::string:
        return make_expression(ast::ExpressionKind::literal, token.lexeme, token.span,
                               ast::LiteralKind::string);
    case TokenKind::string_name:
        return make_expression(ast::ExpressionKind::literal, token.lexeme, token.span,
                               ast::LiteralKind::string_name);
    case TokenKind::node_path:
        return make_expression(ast::ExpressionKind::literal, token.lexeme, token.span,
                               ast::LiteralKind::node_path);
    case TokenKind::node_reference:
        return make_expression(ast::ExpressionKind::node_reference, token.lexeme, token.span);
    case TokenKind::kw_true:
    case TokenKind::kw_false:
        return make_expression(ast::ExpressionKind::literal, token.lexeme, token.span,
                               ast::LiteralKind::boolean);
    case TokenKind::kw_null:
        return make_expression(ast::ExpressionKind::literal, token.lexeme, token.span,
                               ast::LiteralKind::nil);
    case TokenKind::identifier:
    case TokenKind::kw_match:
    case TokenKind::kw_when:
    case TokenKind::kw_self:
        return make_expression(ast::ExpressionKind::identifier, token.lexeme, token.span);
    case TokenKind::kw_func:
        return parse_lambda(token.span);
    case TokenKind::kw_await: {
        auto expression = make_expression(ast::ExpressionKind::await_expression, "", token.span);
        auto* await = expression->get_if<ast::AwaitExpression>();
        // Godot parses the operand at PREC_AWAIT: calls, attributes and subscripts bind to the
        // operand, while every binary operator remains outside the await expression.
        await->operand = parse_expression(11);
        expression->span.end = await->operand->span.end;
        return expression;
    }
    case TokenKind::minus:
    case TokenKind::plus:
    case TokenKind::kw_not:
    case TokenKind::tilde: {
        auto expression = make_expression(ast::ExpressionKind::unary, token.lexeme, token.span);
        auto* unary = expression->get_if<ast::UnaryExpression>();
        unary->operand = parse_expression(token.kind == TokenKind::kw_not ? 3 : 10);
        expression->span.end = unary->operand->span.end;
        return expression;
    }
    case TokenKind::left_paren: {
        auto expression = parse_expression();
        const auto& end = consume(TokenKind::right_paren, "expected ')' after expression");
        expression->span = joined(token.span, end.span);
        return expression;
    }
    case TokenKind::left_bracket: {
        auto expression = make_expression(ast::ExpressionKind::array_literal, "", token.span);
        auto* array = expression->get_if<ast::ArrayExpression>();
        while (!check(TokenKind::right_bracket) && !at_end()) {
            array->elements.push_back(parse_expression());
            if (!match(TokenKind::comma) || check(TokenKind::right_bracket))
                break;
        }
        const auto& end = consume(TokenKind::right_bracket, "expected ']' after array literal");
        expression->span.end = end.span.end;
        return expression;
    }
    case TokenKind::left_brace: {
        auto expression = make_expression(ast::ExpressionKind::dictionary_literal, "", token.span);
        auto* dictionary = expression->get_if<ast::DictionaryExpression>();
        while (!check(TokenKind::right_brace) && !at_end()) {
            auto key = parse_expression();
            if (match(TokenKind::equal)) {
                if (key->kind() == ast::ExpressionKind::identifier) {
                    key = make_expression(ast::ExpressionKind::literal, key->value(), key->span,
                                          ast::LiteralKind::string);
                } else if (key->kind() != ast::ExpressionKind::literal ||
                           key->literal_kind() != ast::LiteralKind::string) {
                    diagnostics_.error(
                        "GDS2026", "Lua-style dictionary keys must be names or strings", key->span);
                }
            } else {
                consume(TokenKind::colon, "expected ':' between dictionary key and value");
            }
            dictionary->entries.push_back({std::move(key), parse_expression()});
            if (!match(TokenKind::comma) || check(TokenKind::right_brace))
                break;
        }
        const auto& end = consume(TokenKind::right_brace, "expected '}' after dictionary literal");
        expression->span.end = end.span.end;
        return expression;
    }
    default:
        diagnostics_.error("GDS2005", "expected an expression", token.span);
        return make_expression(ast::ExpressionKind::literal, "null", token.span,
                               ast::LiteralKind::nil);
    }
}

ast::ExpressionPtr Parser::parse_lambda(SourceSpan begin) {
    auto expression = make_expression(ast::ExpressionKind::lambda, "", begin);
    auto* lambda_value = expression->get_if<ast::LambdaValueExpression>();
    lambda_value->function = std::make_unique<ast::LambdaExpression>();
    auto& lambda = *lambda_value->function;
    if (check_soft_identifier() && position_ + 1 < tokens_.size() &&
        tokens_[position_ + 1].kind == TokenKind::left_paren) {
        lambda.name = advance().lexeme;
    }
    auto parameters = parse_parameters("lambda");
    lambda.parameters = std::move(parameters.positional);
    lambda.rest_parameter = std::move(parameters.rest);
    if (match(TokenKind::arrow))
        lambda.return_type = parse_type_name("expected a lambda return type");
    consume(TokenKind::colon, "expected ':' after lambda signature");
    if (check(TokenKind::newline)) {
        lambda.body = parse_block();
    } else {
        auto statement = parse_statement();
        apply_active_warning_ignores(statement);
        lambda.body.push_back(std::move(statement));
    }
    lambda.span = joined(begin, previous().span);
    expression->span = lambda.span;
    return expression;
}

ast::ExpressionPtr Parser::parse_postfix(ast::ExpressionPtr expression) {
    while (true) {
        if (match(TokenKind::left_paren)) {
            auto call = make_expression(ast::ExpressionKind::call, "", expression->span);
            auto* value = call->get_if<ast::CallExpression>();
            value->callee = std::move(expression);
            while (!check(TokenKind::right_paren) && !at_end()) {
                value->arguments.push_back(parse_expression());
                if (!match(TokenKind::comma) || check(TokenKind::right_paren))
                    break;
            }
            const auto& end = consume(TokenKind::right_paren, "expected ')' after arguments");
            call->span.end = end.span.end;
            expression = std::move(call);
        } else if (match(TokenKind::dot)) {
            const auto& name = consume_attribute_name("expected a member name after '.'");
            auto member = make_expression(ast::ExpressionKind::member, name.lexeme,
                                          joined(expression->span, name.span));
            member->get_if<ast::MemberExpression>()->receiver = std::move(expression);
            expression = std::move(member);
        } else if (match(TokenKind::left_bracket)) {
            auto subscript = make_expression(ast::ExpressionKind::subscript, "", expression->span);
            auto* value = subscript->get_if<ast::SubscriptExpression>();
            value->receiver = std::move(expression);
            value->index = parse_expression();
            const auto& end = consume(TokenKind::right_bracket, "expected ']' after subscript");
            subscript->span.end = end.span.end;
            expression = std::move(subscript);
        } else {
            break;
        }
    }
    return expression;
}

ast::ExpressionPtr Parser::parse_expression(int minimum_precedence) {
    const auto fallback_span = current().span;
    const DepthGuard depth{*this, fallback_span};
    if (!depth) {
        if (!at_end())
            advance();
        return make_expression(ast::ExpressionKind::literal, "null", fallback_span,
                               ast::LiteralKind::nil);
    }
    auto left = parse_postfix(parse_prefix());
    std::size_t binary_chain_length = 0;
    bool binary_chain_limit_reported = false;
    const auto is_not_in = [&] {
        return current().kind == TokenKind::kw_not && position_ + 1 < tokens_.size() &&
               tokens_[position_ + 1].kind == TokenKind::kw_in;
    };
    while ((is_binary_operator(current().kind) || is_not_in()) &&
           precedence(is_not_in() ? TokenKind::kw_in : current().kind) >= minimum_precedence) {
        const auto operation = advance();
        const bool negated_type_test =
            operation.kind == TokenKind::kw_is && match(TokenKind::kw_not);
        const bool negated_membership =
            operation.kind == TokenKind::kw_not && match(TokenKind::kw_in);
        const auto effective_operation = negated_membership ? TokenKind::kw_in : operation.kind;
        const auto right_precedence = effective_operation == TokenKind::power
                                          ? precedence(effective_operation)
                                          : precedence(effective_operation) + 1;
        ast::ExpressionPtr right;
        if (effective_operation == TokenKind::kw_as || effective_operation == TokenKind::kw_is) {
            const auto type_begin = current().span;
            const auto type_name = parse_type_name("expected a type after cast or type test");
            right = make_expression(ast::ExpressionKind::identifier, type_name,
                                    joined(type_begin, previous().span));
        } else {
            right = parse_expression(right_precedence);
        }
        const bool balanced_logical =
            effective_operation == TokenKind::kw_and || effective_operation == TokenKind::kw_or;
        if (!balanced_logical && ++binary_chain_length > limits_.max_binary_chain_length) {
            if (!binary_chain_limit_reported) {
                diagnostics_.error("GDS2031", "binary operator chain exceeds the configured limit",
                                   joined(left->span, right->span));
                binary_chain_limit_reported = true;
            }
            continue;
        }
        auto binary = make_expression(ast::ExpressionKind::binary,
                                      negated_type_test    ? "is not"
                                      : negated_membership ? "not in"
                                                           : operation.lexeme,
                                      joined(left->span, right->span));
        auto* value = binary->get_if<ast::BinaryExpression>();
        value->left = std::move(left);
        value->right = std::move(right);
        left = std::move(binary);
    }
    left = balance_logical_chain(std::move(left));
    const bool multiline_lambda =
        left->kind() == ast::ExpressionKind::lambda && left->span.end.line > left->span.begin.line;
    if (minimum_precedence == 0 && !multiline_lambda && match(TokenKind::kw_if)) {
        auto conditional = make_expression(ast::ExpressionKind::conditional, "", left->span);
        auto* value = conditional->get_if<ast::ConditionalExpression>();
        value->when_true = std::move(left);
        value->condition = parse_expression();
        consume(TokenKind::kw_else, "expected 'else' in conditional expression");
        value->when_false = parse_expression();
        conditional->span.end = value->when_false->span.end;
        return conditional;
    }
    return left;
}

ast::Script Parser::parse_script() {
    ast::Script script;
    if (!tokens_.empty()) {
        script.span.begin = tokens_.front().span.begin;
    }
    skip_newlines();
    std::vector<ast::Annotation> annotations;
    std::unordered_set<std::string> script_annotation_names;
    bool script_header_closed = false;
    while (!at_end()) {
        if (match(TokenKind::at_sign)) {
            auto annotation = parse_annotation();
            const auto* feature =
                LanguageFeatureRegistry::latest().find_annotation(annotation.name);
            const bool is_script_annotation =
                feature && has_annotation_target(feature->targets, AnnotationTarget::script);
            const bool can_target_declaration =
                feature &&
                (has_annotation_target(feature->targets, AnnotationTarget::field) ||
                 has_annotation_target(feature->targets, AnnotationTarget::function) ||
                 has_annotation_target(feature->targets, AnnotationTarget::signal) ||
                 has_annotation_target(feature->targets, AnnotationTarget::enumeration) ||
                 has_annotation_target(feature->targets, AnnotationTarget::inner_class));
            const bool inline_declaration_annotation =
                can_target_declaration && !check(TokenKind::newline);
            if (is_script_annotation && !script_header_closed && !inline_declaration_annotation) {
                if (!script_annotation_names.insert(annotation.name).second) {
                    diagnostics_.error("GDS2028",
                                       "script annotation '@" + annotation.name +
                                           "' can only be used once",
                                       annotation.span);
                }
                if (annotation.name == "tool")
                    script.tool = true;
                script.annotations.push_back(std::move(annotation));
            } else if (feature &&
                       has_annotation_target(feature->targets, AnnotationTarget::directive)) {
                apply_warning_directive(annotation);
            } else if (is_script_annotation && !can_target_declaration) {
                diagnostics_.error("GDS2027",
                                   "script annotation '@" + annotation.name +
                                       "' must appear before extends and class_name",
                                   annotation.span);
            } else {
                annotations.push_back(std::move(annotation));
            }
            if (match(TokenKind::newline))
                skip_newlines();
            continue;
        }
        if (match(TokenKind::kw_extends)) {
            script_header_closed = true;
            if (!annotations.empty())
                validate_annotation_targets(annotations, AnnotationTarget::script, diagnostics_);
            for (auto& annotation : annotations)
                script.annotations.push_back(std::move(annotation));
            annotations.clear();
            if (check(TokenKind::identifier) || check(TokenKind::string)) {
                script.base_type = advance().lexeme;
            } else {
                diagnostics_.error("GDS2006", "expected a base type after extends", current().span);
            }
            skip_newlines();
        } else if (match(TokenKind::kw_class_name)) {
            script_header_closed = true;
            if (!annotations.empty())
                validate_annotation_targets(annotations, AnnotationTarget::script, diagnostics_);
            for (auto& annotation : annotations)
                script.annotations.push_back(std::move(annotation));
            annotations.clear();
            script.class_name = consume(TokenKind::identifier, "expected a class name").lexeme;
            if (match(TokenKind::kw_extends)) {
                if (script.base_type) {
                    diagnostics_.error("GDS2006", "script base type is already declared",
                                       previous().span);
                }
                if (check(TokenKind::identifier) || check(TokenKind::string)) {
                    script.base_type = advance().lexeme;
                } else {
                    diagnostics_.error("GDS2006", "expected a base type after extends",
                                       current().span);
                }
            }
            skip_newlines();
        } else if (match(TokenKind::kw_var)) {
            script_header_closed = true;
            validate_annotation_targets(annotations, AnnotationTarget::field, diagnostics_);
            script.variables.push_back(parse_variable(false, std::move(annotations)));
            annotations.clear();
        } else if (match(TokenKind::kw_const)) {
            script_header_closed = true;
            validate_annotation_targets(annotations, AnnotationTarget::field, diagnostics_);
            script.variables.push_back(parse_variable(true, std::move(annotations)));
            annotations.clear();
        } else if (match(TokenKind::kw_signal)) {
            script_header_closed = true;
            validate_annotation_targets(annotations, AnnotationTarget::signal, diagnostics_);
            script.signals.push_back(parse_signal(std::move(annotations)));
            annotations.clear();
        } else if (match(TokenKind::kw_enum)) {
            script_header_closed = true;
            validate_annotation_targets(annotations, AnnotationTarget::enumeration, diagnostics_);
            script.enums.push_back(parse_enum(std::move(annotations)));
            annotations.clear();
        } else if (match(TokenKind::kw_static)) {
            script_header_closed = true;
            if (match(TokenKind::kw_var)) {
                validate_annotation_targets(annotations, AnnotationTarget::field, diagnostics_);
                auto variable = parse_variable(false, std::move(annotations));
                variable.is_static = true;
                script.variables.push_back(std::move(variable));
                annotations.clear();
            } else {
                validate_annotation_targets(annotations, AnnotationTarget::function, diagnostics_);
                consume(TokenKind::kw_func, "expected 'var' or 'func' after 'static'");
                script.functions.push_back(parse_function(true, std::move(annotations)));
                annotations.clear();
            }
        } else if (match(TokenKind::kw_func)) {
            script_header_closed = true;
            validate_annotation_targets(annotations, AnnotationTarget::function, diagnostics_);
            script.functions.push_back(parse_function(false, std::move(annotations)));
            annotations.clear();
        } else if (match(TokenKind::kw_class)) {
            script_header_closed = true;
            validate_annotation_targets(annotations, AnnotationTarget::inner_class, diagnostics_);
            script.classes.push_back(parse_class(std::move(annotations)));
            annotations.clear();
        } else if (match(TokenKind::string)) {
            // Standalone triple/single/double quoted strings are accepted by Godot as script
            // documentation/comment blocks and do not become runtime declarations.
            annotations.clear();
            skip_newlines();
        } else {
            if (!annotations.empty())
                annotations.clear();
            diagnostics_.error("GDS2007", "unsupported or unexpected top-level declaration",
                               current().span);
            synchronize();
        }
        skip_newlines();
    }
    if (!annotations.empty()) {
        diagnostics_.error("GDS2009", "annotation is not attached to a declaration",
                           annotations.front().span);
    }
    script.span.end = current().span.end;
    return script;
}

} // namespace gdpp
