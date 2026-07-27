#pragma once

#include "gdpp/codegen/cpp_generator.hpp"
#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/godot_version.hpp"
#include "gdpp/frontend/limits.hpp"
#include "gdpp/ir/mir_optimizer.hpp"
#include "gdpp/ir/optimizer.hpp"
#include "gdpp/semantic/script_symbols.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gdpp {

struct CompileOptions {
    bool emit_debug_comments{false};
    bool optimize{true};
    GodotVersion target_version{minimum_godot_version};
    FrontendLimits frontend_limits{};
    std::string native_class_suffix;
    // Project compilation resolves script inheritance before invoking the single-file pipeline.
    // These values keep the core compiler independent from project filesystem discovery.
    std::string semantic_base_type;
    std::string native_base_class;
    std::string native_base_header;
    bool attached_script{false};
    std::string attached_native_base;
    std::string attached_base_script_path;
    // Full public ABI digest shared by the editor reflection descriptor and the target runtime
    // descriptor. Attached script replacement fails closed when these contracts diverge.
    std::string script_contract_hash;
    const ScriptSymbolTable* script_symbols{nullptr};
    std::string current_script_path;
};

struct CompileResult {
    bool success{false};
    GeneratedUnit unit;
    OptimizationStats optimization;
    MirOptimizationStats mir_optimization;
    std::vector<Diagnostic> diagnostics;
    struct Metrics {
        std::uint64_t total_ns{0};
        std::uint64_t lex_ns{0};
        std::uint64_t parse_ns{0};
        std::uint64_t semantic_ns{0};
        std::uint64_t hir_lower_ns{0};
        std::uint64_t optimize_ns{0};
        std::uint64_t mir_lower_verify_ns{0};
        std::uint64_t mir_optimize_ns{0};
        std::uint64_t codegen_ns{0};
        std::size_t token_count{0};
        std::size_t ast_expression_count{0};
        std::size_t ast_statement_count{0};
        std::size_t hir_expression_count{0};
        std::size_t hir_statement_count{0};
        std::size_t mir_function_count{0};
        std::size_t mir_block_count{0};
        std::size_t mir_instruction_count{0};
    } metrics;
};

class Compiler final {
  public:
    [[nodiscard]] CompileResult compile(std::string path, std::string source,
                                        const CompileOptions& options = {}) const;

  private:
    [[nodiscard]] CompileResult compile_impl(std::string path, std::string source,
                                             const CompileOptions& options) const;
};

} // namespace gdpp
