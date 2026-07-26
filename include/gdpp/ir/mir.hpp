#pragma once

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/ir/typed_program.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdpp::mir {

inline constexpr std::uint32_t schema_version = 2;

using FunctionId = std::uint32_t;
using BlockId = std::uint32_t;
using OperationId = std::uint32_t;
using ValueId = std::uint32_t;
using SourceStatementId = std::uint32_t;
using SourceExpressionId = std::uint32_t;
inline constexpr FunctionId invalid_function = std::numeric_limits<FunctionId>::max();
inline constexpr BlockId invalid_block = std::numeric_limits<BlockId>::max();
inline constexpr OperationId invalid_operation = std::numeric_limits<OperationId>::max();
inline constexpr ValueId invalid_value = std::numeric_limits<ValueId>::max();
inline constexpr SourceStatementId invalid_source_statement =
    std::numeric_limits<SourceStatementId>::max();
inline constexpr SourceExpressionId invalid_source_expression =
    std::numeric_limits<SourceExpressionId>::max();

enum class InstructionKind : std::uint8_t {
    evaluate,
    declare_variable,
    assign,
    assert_condition,
    debug_breakpoint,
    loop_test,
    match_test,
    suspend_value,
};

enum class Effect : std::uint8_t {
    none = 0,
    reads_state = 1U << 0U,
    writes_state = 1U << 1U,
    may_fail = 1U << 2U,
    may_allocate = 1U << 3U,
    suspends = 1U << 4U,
    observes_debugger = 1U << 5U,
};

constexpr Effect operator|(Effect left, Effect right) noexcept {
    return static_cast<Effect>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool has_effect(Effect effects, Effect expected) noexcept {
    return (static_cast<std::uint8_t>(effects) & static_cast<std::uint8_t>(expected)) != 0;
}

struct Instruction {
    OperationId id{invalid_operation};
    InstructionKind kind{InstructionKind::evaluate};
    Effect effects{Effect::none};
    SourceStatementId source_statement{invalid_source_statement};
    std::vector<ValueId> inputs;
    SourceSpan span{};
};

enum class TerminatorKind : std::uint8_t {
    invalid,
    jump,
    branch,
    return_value,
    stop,
    suspend,
};

// A branch target pair is not sufficient to describe why control splits. Match-pattern and
// iterator-protocol branches do not evaluate `condition` as a truthy GDScript expression, so
// optimization passes must not infer their semantics from the expression payload.
enum class BranchRole : std::uint8_t {
    none,
    condition,
    iterator_protocol,
    match_pattern,
    match_guard,
    assertion,
};

struct Terminator {
    OperationId id{invalid_operation};
    TerminatorKind kind{TerminatorKind::invalid};
    ValueId condition_value{invalid_value};
    std::vector<BlockId> targets;
    SourceSpan span{};
    BranchRole branch_role{BranchRole::none};
};

struct BasicBlock {
    BlockId id{invalid_block};
    std::vector<Instruction> instructions;
    Terminator terminator;
    std::vector<BlockId> predecessors;
};

enum class FunctionRole : std::uint8_t { method, getter, setter, lambda };

// Values are immutable semantic facts copied from the typed program. Stable source IDs preserve
// traceability without coupling MIR lifetime to process addresses or external object storage.
struct Value {
    ValueId id{invalid_value};
    typed::ExpressionKind kind{typed::ExpressionKind::literal};
    Type type;
    Type storage_type;
    Type assignment_type;
    OwnershipKind ownership{OwnershipKind::dynamic};
    bool non_null{false};
    typed::LiteralKind literal_kind{typed::LiteralKind::none};
    typed::ResolutionKind resolution{typed::ResolutionKind::none};
    std::string payload;
    std::string resolved_owner;
    std::string getter;
    std::string setter;
    bool direct_access{false};
    bool coroutine_call{false};
    std::int64_t indexed_argument{-1};
    FlowSymbolId symbol_identity{0};
    IntrinsicKind intrinsic{IntrinsicKind::none};
    std::optional<typed::CallContract> call_contract;
    std::vector<ValueId> operands;
    SourceSpan span{};
    SourceExpressionId source_expression{invalid_source_expression};
};

struct ControlFlowFunction {
    FunctionId id{invalid_function};
    std::string name;
    FunctionRole role{FunctionRole::method};
    BlockId entry{invalid_block};
    std::vector<Value> values;
    std::vector<BasicBlock> blocks;
    std::uint32_t source_statement_count{0};
    std::uint32_t source_expression_count{0};
    bool suspends{false};
    SourceSpan span{};
};

struct Module {
    std::uint32_t format_version{schema_version};
    typed::Module program;
    std::vector<ControlFlowFunction> functions;
};

} // namespace gdpp::mir

namespace gdpp {

// A deterministic, function-local view of source nodes. The index is intentionally ephemeral:
// MIR stores only its stable IDs, while lowering and code generation resolve those IDs against the
// typed program owned by mir::Module.
class MirSourceIndex final {
  public:
    explicit MirSourceIndex(const std::vector<typed::Statement>& statements);

    [[nodiscard]] mir::SourceStatementId
    statement_id(const typed::Statement& statement) const noexcept;
    [[nodiscard]] mir::SourceExpressionId
    expression_id(const typed::Expression& expression) const noexcept;
    [[nodiscard]] const typed::Statement* statement(mir::SourceStatementId id) const noexcept;
    [[nodiscard]] const typed::Expression* expression(mir::SourceExpressionId id) const noexcept;
    [[nodiscard]] std::size_t statement_count() const noexcept;
    [[nodiscard]] std::size_t expression_count() const noexcept;

  private:
    void index_statements(const std::vector<typed::Statement>& statements);
    void index_statement(const typed::Statement& statement);
    void index_pattern(const typed::MatchPattern& pattern);
    void index_expression(const typed::Expression& expression);

    std::vector<const typed::Statement*> statements_;
    std::vector<const typed::Expression*> expressions_;
    std::unordered_map<const typed::Statement*, mir::SourceStatementId> statement_ids_;
    std::unordered_map<const typed::Expression*, mir::SourceExpressionId> expression_ids_;
};

class MirLowerer final {
  public:
    [[nodiscard]] mir::Module lower(typed::Module module) const;
};

class MirVerifier final {
  public:
    explicit MirVerifier(DiagnosticBag& diagnostics) : diagnostics_(diagnostics) {}
    [[nodiscard]] bool verify(const mir::Module& module) const;

  private:
    DiagnosticBag& diagnostics_;
};

class MirSerializer final {
  public:
    // Returns the versioned diagnostic/snapshot form. It never serializes process addresses,
    // source text, native paths, or implementation-specific enum ordinals.
    [[nodiscard]] std::string serialize(const mir::Module& module) const;
};

} // namespace gdpp
