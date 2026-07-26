#pragma once

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/ir/hir.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace gdpp::mir {

inline constexpr std::uint32_t schema_version = 1;

using FunctionId = std::uint32_t;
using BlockId = std::uint32_t;
using OperationId = std::uint32_t;
using ValueId = std::uint32_t;
inline constexpr FunctionId invalid_function = std::numeric_limits<FunctionId>::max();
inline constexpr BlockId invalid_block = std::numeric_limits<BlockId>::max();
inline constexpr OperationId invalid_operation = std::numeric_limits<OperationId>::max();
inline constexpr ValueId invalid_value = std::numeric_limits<ValueId>::max();

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
    const ir::Statement* source{nullptr};
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
    const ir::Expression* condition{nullptr};
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

// Values are immutable semantic facts copied from HIR. The source pointer remains available only
// while the legacy C++ emitter is migrated; stable IDs and owned payload are the public MIR
// identity and are the only representation emitted by diagnostics or serialization.
struct Value {
    ValueId id{invalid_value};
    ir::ExpressionKind kind{ir::ExpressionKind::literal};
    Type type;
    Type storage_type;
    Type assignment_type;
    OwnershipKind ownership{OwnershipKind::dynamic};
    bool non_null{false};
    ir::LiteralKind literal_kind{ir::LiteralKind::none};
    ir::ResolutionKind resolution{ir::ResolutionKind::none};
    std::string payload;
    std::string resolved_owner;
    std::string getter;
    std::string setter;
    bool direct_access{false};
    bool coroutine_call{false};
    std::int64_t indexed_argument{-1};
    FlowSymbolId symbol_identity{0};
    IntrinsicKind intrinsic{IntrinsicKind::none};
    std::optional<ir::CallContract> call_contract;
    std::vector<ValueId> operands;
    SourceSpan span{};
    const ir::Expression* source{nullptr};
};

struct Function {
    FunctionId id{invalid_function};
    std::string name;
    FunctionRole role{FunctionRole::method};
    BlockId entry{invalid_block};
    std::vector<Value> values;
    std::vector<BasicBlock> blocks;
    bool suspends{false};
    SourceSpan span{};
};

struct Module {
    std::uint32_t format_version{schema_version};
    const ir::Module* hir{nullptr};
    std::vector<Function> functions;
};

} // namespace gdpp::mir

namespace gdpp {

class MirLowerer final {
  public:
    [[nodiscard]] mir::Module lower(const ir::Module& module) const;
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
