#pragma once

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/ir/hir.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace gdpp::mir {

using BlockId = std::uint32_t;
inline constexpr BlockId invalid_block = std::numeric_limits<BlockId>::max();

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
    InstructionKind kind{InstructionKind::evaluate};
    Effect effects{Effect::none};
    const ir::Statement* source{nullptr};
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
    TerminatorKind kind{TerminatorKind::invalid};
    const ir::Expression* condition{nullptr};
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

struct Function {
    std::string name;
    FunctionRole role{FunctionRole::method};
    BlockId entry{invalid_block};
    std::vector<BasicBlock> blocks;
    bool suspends{false};
    SourceSpan span{};
};

struct Module {
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

} // namespace gdpp
