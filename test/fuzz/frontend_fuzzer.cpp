#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/frontend/ast.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Digest {
    std::uint64_t value{1469598103934665603ULL};
    std::size_t nodes{0};

    friend bool operator==(const Digest& left, const Digest& right) noexcept {
        return left.value == right.value && left.nodes == right.nodes;
    }
};

[[noreturn]] void reject_invariant() { std::abort(); }

void hash_bytes(Digest& digest, const std::string_view bytes) {
    for (const auto byte : bytes) {
        digest.value ^= static_cast<unsigned char>(byte);
        digest.value *= 1099511628211ULL;
    }
    digest.value ^= 0xffU;
    digest.value *= 1099511628211ULL;
}

template <typename Value> void hash_integer(Digest& digest, const Value value) {
    auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(bits); ++index) {
        digest.value ^= static_cast<unsigned char>(bits & 0xffU);
        digest.value *= 1099511628211ULL;
        bits >>= 8U;
    }
}

void inspect_span(Digest& digest, const gdpp::SourceSpan& span, const std::size_t source_size) {
    if (span.begin.offset > span.end.offset || span.end.offset > source_size ||
        span.begin.line == 0 || span.begin.column == 0 || span.end.line == 0 ||
        span.end.column == 0) {
        reject_invariant();
    }
    hash_integer(digest, span.begin.offset);
    hash_integer(digest, span.begin.line);
    hash_integer(digest, span.begin.column);
    hash_integer(digest, span.end.offset);
    hash_integer(digest, span.end.line);
    hash_integer(digest, span.end.column);
}

void inspect_expression(Digest& digest, const gdpp::ast::Expression* expression,
                        const std::size_t source_size);
void inspect_block(Digest& digest, const gdpp::ast::Block& block, std::size_t source_size);

void inspect_annotation(Digest& digest, const gdpp::ast::Annotation& annotation,
                        const std::size_t source_size) {
    ++digest.nodes;
    hash_bytes(digest, annotation.name);
    inspect_span(digest, annotation.span, source_size);
    for (const auto& argument : annotation.arguments)
        inspect_expression(digest, argument.get(), source_size);
}

void inspect_parameter(Digest& digest, const gdpp::ast::Parameter& parameter,
                       const std::size_t source_size) {
    ++digest.nodes;
    hash_bytes(digest, parameter.name);
    hash_bytes(digest, parameter.type.value_or(""));
    hash_integer(digest, parameter.infer_type);
    inspect_span(digest, parameter.span, source_size);
    inspect_expression(digest, parameter.default_value.get(), source_size);
}

void inspect_pattern(Digest& digest, const gdpp::ast::MatchPattern& pattern,
                     const std::size_t source_size) {
    ++digest.nodes;
    hash_integer(digest, pattern.kind());
    hash_bytes(digest, pattern.name());
    inspect_span(digest, pattern.span, source_size);
    inspect_expression(digest, pattern.expression().get(), source_size);
    for (const auto& key : pattern.keys)
        inspect_expression(digest, key.get(), source_size);
    for (const auto& element : pattern.elements) {
        if (!element)
            reject_invariant();
        inspect_pattern(digest, *element, source_size);
    }
}

void inspect_expression(Digest& digest, const gdpp::ast::Expression* expression,
                        const std::size_t source_size) {
    if (!expression)
        return;
    ++digest.nodes;
    hash_integer(digest, expression->kind());
    hash_integer(digest, expression->literal_kind());
    hash_bytes(digest, expression->value());
    inspect_span(digest, expression->span, source_size);
    for (std::size_t index = 0; index < expression->operand_count(); ++index) {
        const auto& operand = expression->operand(index);
        if (!operand)
            reject_invariant();
        inspect_expression(digest, operand.get(), source_size);
    }
    if (const auto* lambda = expression->lambda()) {
        ++digest.nodes;
        hash_bytes(digest, lambda->name);
        hash_bytes(digest, lambda->return_type.value_or(""));
        inspect_span(digest, lambda->span, source_size);
        for (const auto& parameter : lambda->parameters)
            inspect_parameter(digest, parameter, source_size);
        if (lambda->rest_parameter)
            inspect_parameter(digest, *lambda->rest_parameter, source_size);
        inspect_block(digest, lambda->body, source_size);
    }
}

void inspect_statement(Digest& digest, const gdpp::ast::Statement& statement,
                       const std::size_t source_size) {
    ++digest.nodes;
    hash_integer(digest, statement.kind());
    hash_bytes(digest, statement.name());
    hash_bytes(digest, statement.type().value_or(""));
    hash_bytes(digest, statement.operation());
    hash_integer(digest, statement.infer_type());
    hash_integer(digest, statement.is_constant());
    inspect_span(digest, statement.span, source_size);
    inspect_expression(digest, statement.expression().get(), source_size);
    if (statement.condition().get() != statement.expression().get())
        inspect_expression(digest, statement.condition().get(), source_size);
    for (const auto& annotation : statement.annotations)
        inspect_annotation(digest, annotation, source_size);
    inspect_block(digest, statement.body(), source_size);
    inspect_block(digest, statement.else_body(), source_size);
    for (const auto& branch : statement.match_branches()) {
        ++digest.nodes;
        inspect_span(digest, branch.span, source_size);
        inspect_expression(digest, branch.guard.get(), source_size);
        for (const auto& pattern : branch.patterns)
            inspect_pattern(digest, pattern, source_size);
        inspect_block(digest, branch.body, source_size);
    }
}

void inspect_block(Digest& digest, const gdpp::ast::Block& block, const std::size_t source_size) {
    hash_integer(digest, block.size());
    for (const auto& statement : block)
        inspect_statement(digest, statement, source_size);
}

void inspect_variable(Digest& digest, const gdpp::ast::VariableDeclaration& variable,
                      const std::size_t source_size) {
    ++digest.nodes;
    hash_bytes(digest, variable.name);
    hash_bytes(digest, variable.type.value_or(""));
    hash_integer(digest, variable.is_constant);
    hash_integer(digest, variable.is_static);
    hash_integer(digest, variable.infer_type);
    hash_integer(digest, variable.onready);
    inspect_span(digest, variable.span, source_size);
    inspect_expression(digest, variable.initializer.get(), source_size);
    for (const auto& annotation : variable.annotations)
        inspect_annotation(digest, annotation, source_size);
    if (variable.getter) {
        inspect_span(digest, variable.getter->span, source_size);
        hash_bytes(digest, variable.getter->parameter);
        hash_bytes(digest, variable.getter->method);
        inspect_block(digest, variable.getter->body, source_size);
    }
    if (variable.setter) {
        inspect_span(digest, variable.setter->span, source_size);
        hash_bytes(digest, variable.setter->parameter);
        hash_bytes(digest, variable.setter->method);
        inspect_block(digest, variable.setter->body, source_size);
    }
}

void inspect_function(Digest& digest, const gdpp::ast::FunctionDeclaration& function,
                      const std::size_t source_size) {
    ++digest.nodes;
    hash_bytes(digest, function.name);
    hash_bytes(digest, function.return_type.value_or(""));
    hash_integer(digest, function.is_static);
    hash_integer(digest, function.is_abstract);
    hash_integer(digest, function.has_body);
    inspect_span(digest, function.span, source_size);
    for (const auto& annotation : function.annotations)
        inspect_annotation(digest, annotation, source_size);
    for (const auto& parameter : function.parameters)
        inspect_parameter(digest, parameter, source_size);
    if (function.rest_parameter)
        inspect_parameter(digest, *function.rest_parameter, source_size);
    inspect_block(digest, function.body, source_size);
}

void inspect_enum(Digest& digest, const gdpp::ast::EnumDeclaration& enumeration,
                  const std::size_t source_size) {
    ++digest.nodes;
    hash_bytes(digest, enumeration.name.value_or(""));
    inspect_span(digest, enumeration.span, source_size);
    for (const auto& annotation : enumeration.annotations)
        inspect_annotation(digest, annotation, source_size);
    for (const auto& entry : enumeration.entries) {
        ++digest.nodes;
        hash_bytes(digest, entry.name);
        inspect_span(digest, entry.span, source_size);
        inspect_expression(digest, entry.value.get(), source_size);
    }
}

void inspect_signal(Digest& digest, const gdpp::ast::SignalDeclaration& signal,
                    const std::size_t source_size) {
    ++digest.nodes;
    hash_bytes(digest, signal.name);
    inspect_span(digest, signal.span, source_size);
    for (const auto& annotation : signal.annotations)
        inspect_annotation(digest, annotation, source_size);
    for (const auto& parameter : signal.parameters)
        inspect_parameter(digest, parameter, source_size);
}

void inspect_class(Digest& digest, const gdpp::ast::ClassDeclaration& declaration,
                   const std::size_t source_size) {
    ++digest.nodes;
    hash_bytes(digest, declaration.name);
    hash_bytes(digest, declaration.base_type.value_or(""));
    inspect_span(digest, declaration.span, source_size);
    for (const auto& annotation : declaration.annotations)
        inspect_annotation(digest, annotation, source_size);
    for (const auto& variable : declaration.variables)
        inspect_variable(digest, variable, source_size);
    for (const auto& enumeration : declaration.enums)
        inspect_enum(digest, enumeration, source_size);
    for (const auto& signal : declaration.signals)
        inspect_signal(digest, signal, source_size);
    for (const auto& function : declaration.functions)
        inspect_function(digest, function, source_size);
    for (const auto& child : declaration.classes)
        inspect_class(digest, child, source_size);
}

Digest run_frontend(const std::string& bytes) {
    gdpp::FrontendLimits limits;
    limits.max_source_bytes = 64U * 1024U;
    limits.max_line_bytes = 64U * 1024U;
    limits.max_tokens = 16U * 1024U;
    limits.max_literal_bytes = 64U * 1024U;
    limits.max_indentation_depth = 64;
    limits.max_grouping_depth = 128;
    limits.max_parser_depth = 64;
    limits.max_binary_chain_length = 128;
    limits.max_diagnostics = 64;

    const gdpp::SourceFile source{"fuzz.gd", bytes};
    gdpp::DiagnosticBag diagnostics{limits.max_diagnostics};
    const auto tokens = gdpp::Lexer{source, diagnostics, limits}.scan();
    if (tokens.empty() || tokens.back().kind != gdpp::TokenKind::end_of_file ||
        tokens.size() > limits.max_tokens + 1U ||
        diagnostics.items().size() > limits.max_diagnostics + 1U) {
        reject_invariant();
    }
    const auto script = gdpp::Parser{tokens, diagnostics, limits}.parse_script();
    if (diagnostics.items().size() > limits.max_diagnostics + 1U)
        reject_invariant();

    Digest digest;
    hash_integer(digest, tokens.size());
    for (const auto& token : tokens) {
        hash_integer(digest, token.kind);
        hash_bytes(digest, token.lexeme);
        inspect_span(digest, token.span, bytes.size());
    }
    hash_integer(digest, diagnostics.items().size());
    for (const auto& diagnostic : diagnostics.items()) {
        hash_integer(digest, diagnostic.severity);
        hash_bytes(digest, diagnostic.code);
        hash_bytes(digest, diagnostic.message);
        inspect_span(digest, diagnostic.span, bytes.size());
    }
    ++digest.nodes;
    inspect_span(digest, script.span, bytes.size());
    hash_bytes(digest, script.base_type.value_or(""));
    hash_bytes(digest, script.class_name.value_or(""));
    hash_integer(digest, script.tool);
    for (const auto& annotation : script.annotations)
        inspect_annotation(digest, annotation, bytes.size());
    for (const auto& variable : script.variables)
        inspect_variable(digest, variable, bytes.size());
    for (const auto& enumeration : script.enums)
        inspect_enum(digest, enumeration, bytes.size());
    for (const auto& signal : script.signals)
        inspect_signal(digest, signal, bytes.size());
    for (const auto& function : script.functions)
        inspect_function(digest, function, bytes.size());
    for (const auto& declaration : script.classes)
        inspect_class(digest, declaration, bytes.size());
    if (digest.nodes > tokens.size() * 16U + 1024U)
        reject_invariant();
    return digest;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (size > 64U * 1024U)
        return 0;
    const std::string bytes{reinterpret_cast<const char*>(data), size};
    const auto first = run_frontend(bytes);
    const auto second = run_frontend(bytes);
    if (!(first == second))
        reject_invariant();
    return 0;
}
