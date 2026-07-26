#include "gdpp/compiler/compiler.hpp"

#include "gdpp/core/source.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/parser.hpp"
#include "gdpp/ir/lowering.hpp"
#include "gdpp/ir/mir.hpp"
#include "gdpp/ir/optimizer.hpp"
#include "gdpp/semantic/analyzer.hpp"

#include <chrono>
#include <cstddef>
#include <utility>

namespace gdpp {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point begin, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

void count_ast_expression(const ast::Expression& expression, CompileResult::Metrics& metrics);

void count_ast_pattern(const ast::MatchPattern& pattern, CompileResult::Metrics& metrics) {
    if (pattern.expression())
        count_ast_expression(*pattern.expression(), metrics);
    for (const auto& key : pattern.keys) {
        if (key)
            count_ast_expression(*key, metrics);
    }
    for (const auto& element : pattern.elements)
        count_ast_pattern(*element, metrics);
}

void count_ast_statements(const ast::Block& statements, CompileResult::Metrics& metrics) {
    for (const auto& statement : statements) {
        ++metrics.ast_statement_count;
        if (statement.expression())
            count_ast_expression(*statement.expression(), metrics);
        if (statement.condition())
            count_ast_expression(*statement.condition(), metrics);
        count_ast_statements(statement.body(), metrics);
        count_ast_statements(statement.else_body(), metrics);
        for (const auto& branch : statement.match_branches()) {
            if (branch.guard)
                count_ast_expression(*branch.guard, metrics);
            for (const auto& pattern : branch.patterns)
                count_ast_pattern(pattern, metrics);
            count_ast_statements(branch.body, metrics);
        }
    }
}

void count_ast_expression(const ast::Expression& expression, CompileResult::Metrics& metrics) {
    ++metrics.ast_expression_count;
    for (std::size_t index = 0; index < expression.operand_count(); ++index)
        count_ast_expression(*expression.operand(index), metrics);
    if (const auto* lambda = expression.lambda())
        count_ast_statements(lambda->body, metrics);
}

void count_ast(const ast::Script& script, CompileResult::Metrics& metrics) {
    for (const auto& field : script.variables) {
        if (field.initializer)
            count_ast_expression(*field.initializer, metrics);
        if (field.getter)
            count_ast_statements(field.getter->body, metrics);
        if (field.setter)
            count_ast_statements(field.setter->body, metrics);
    }
    for (const auto& function : script.functions)
        count_ast_statements(function.body, metrics);
    for (const auto& enumeration : script.enums) {
        for (const auto& entry : enumeration.entries) {
            if (entry.value)
                count_ast_expression(*entry.value, metrics);
        }
    }
}

void count_hir_expression(const ir::Expression& expression, CompileResult::Metrics& metrics);

void count_hir_pattern(const ir::MatchPattern& pattern, CompileResult::Metrics& metrics) {
    if (pattern.expression)
        count_hir_expression(*pattern.expression, metrics);
    for (const auto& key : pattern.keys) {
        if (key)
            count_hir_expression(*key, metrics);
    }
    for (const auto& element : pattern.elements)
        count_hir_pattern(element, metrics);
}

void count_hir_statements(const std::vector<ir::Statement>& statements,
                          CompileResult::Metrics& metrics) {
    for (const auto& statement : statements) {
        ++metrics.hir_statement_count;
        if (statement.expression)
            count_hir_expression(*statement.expression, metrics);
        if (statement.condition)
            count_hir_expression(*statement.condition, metrics);
        for (const auto& pattern : statement.patterns)
            count_hir_pattern(pattern, metrics);
        count_hir_statements(statement.body, metrics);
        count_hir_statements(statement.else_body, metrics);
        count_hir_statements(statement.guard_prefix, metrics);
        count_hir_statements(statement.assert_condition_prefix, metrics);
        count_hir_statements(statement.assert_message_prefix, metrics);
    }
}

void count_hir_expression(const ir::Expression& expression, CompileResult::Metrics& metrics) {
    ++metrics.hir_expression_count;
    for (const auto& operand : expression.operands)
        count_hir_expression(*operand, metrics);
    if (expression.lambda)
        count_hir_statements(expression.lambda->body, metrics);
}

void count_hir(const ir::Module& module, CompileResult::Metrics& metrics) {
    for (const auto& field : module.fields) {
        if (field.initializer)
            count_hir_expression(*field.initializer, metrics);
        if (field.getter)
            count_hir_statements(field.getter->body, metrics);
        if (field.setter)
            count_hir_statements(field.setter->body, metrics);
    }
    for (const auto& function : module.functions)
        count_hir_statements(function.body, metrics);
}

} // namespace

CompileResult Compiler::compile(std::string path, std::string source_text,
                                const CompileOptions& options) const {
    const auto total_begin = Clock::now();
    const SourceFile source{std::move(path), std::move(source_text)};
    DiagnosticBag diagnostics{options.frontend_limits.max_diagnostics};
    CompileResult result;
    const auto lex_begin = Clock::now();
    Lexer lexer{source, diagnostics, options.frontend_limits};
    const auto tokens = lexer.scan();
    const auto lex_end = Clock::now();
    result.metrics.lex_ns = elapsed_ns(lex_begin, lex_end);
    result.metrics.token_count = tokens.size();
    const auto parse_begin = Clock::now();
    Parser parser{tokens, diagnostics, options.frontend_limits};
    const auto script = parser.parse_script();
    const auto parse_end = Clock::now();
    result.metrics.parse_ns = elapsed_ns(parse_begin, parse_end);
    count_ast(script, result.metrics);

    if (!diagnostics.has_errors()) {
        const auto semantic_begin = Clock::now();
        SemanticAnalyzer analyzer{diagnostics, GodotApi::for_version(options.target_version),
                                  options.semantic_base_type, options.script_symbols,
                                  options.current_script_path};
        const auto semantic = analyzer.analyze(script);
        const auto semantic_end = Clock::now();
        result.metrics.semantic_ns = elapsed_ns(semantic_begin, semantic_end);
        if (diagnostics.has_errors()) {
            result.success = false;
            result.diagnostics = diagnostics.items();
            result.metrics.total_ns = elapsed_ns(total_begin, Clock::now());
            return result;
        }
        const auto hir_begin = Clock::now();
        IrLowerer lowerer{semantic};
        auto module = lowerer.lower(script);
        IrVerifier verifier{diagnostics};
        (void)verifier.verify(module);
        const auto hir_end = Clock::now();
        result.metrics.hir_lower_ns = elapsed_ns(hir_begin, hir_end);
        count_hir(module, result.metrics);
        if (!diagnostics.has_errors() && options.optimize) {
            const auto optimize_begin = Clock::now();
            const IrOptimizer optimizer;
            result.optimization = optimizer.optimize(module);
            (void)verifier.verify(module);
            result.metrics.optimize_ns = elapsed_ns(optimize_begin, Clock::now());
        }
        const auto mir_begin = Clock::now();
        auto mir = MirLowerer{}.lower(std::move(module));
        MirVerifier mir_verifier{diagnostics};
        (void)mir_verifier.verify(mir);
        result.metrics.mir_lower_verify_ns = elapsed_ns(mir_begin, Clock::now());
        if (!diagnostics.has_errors() && options.optimize) {
            const auto mir_optimize_begin = Clock::now();
            result.mir_optimization = MirOptimizer{}.optimize(mir, diagnostics);
            result.metrics.mir_optimize_ns = elapsed_ns(mir_optimize_begin, Clock::now());
        }
        result.metrics.mir_function_count = mir.functions.size();
        for (const auto& function : mir.functions) {
            result.metrics.mir_block_count += function.blocks.size();
            for (const auto& block : function.blocks)
                result.metrics.mir_instruction_count += block.instructions.size();
        }
        CodeGenerator generator{diagnostics, GodotApi::for_version(options.target_version),
                                options.script_symbols};
        if (!diagnostics.has_errors()) {
            const auto codegen_begin = Clock::now();
            result.unit = generator.generate(
                mir, source.path(), options.native_class_suffix, options.native_base_class,
                options.native_base_header, options.attached_script, options.attached_native_base,
                options.attached_base_script_path, options.script_contract_hash);
            result.metrics.codegen_ns = elapsed_ns(codegen_begin, Clock::now());
        }
        if (diagnostics.has_errors()) {
            result.unit = {};
        }
    }
    result.success = !diagnostics.has_errors();
    result.diagnostics = diagnostics.items();
    result.metrics.total_ns = elapsed_ns(total_begin, Clock::now());
    return result;
}

} // namespace gdpp
