#include "gdpp/ir/mir_optimizer.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace gdpp {
namespace {

struct MirSize {
    std::size_t functions{0};
    std::size_t blocks{0};
    std::size_t operations{0};
    std::size_t values{0};
};

MirSize measure(const std::vector<mir::ControlFlowFunction>& functions) {
    MirSize result;
    result.functions = functions.size();
    for (const auto& function : functions) {
        result.blocks += function.blocks.size();
        result.values += function.values.size();
        for (const auto& block : function.blocks)
            result.operations += block.instructions.size() + 1U;
    }
    return result;
}

bool within_budget(const MirSize& size, const MirOptimizationBudget& budget) {
    return size.functions <= budget.max_functions && size.blocks <= budget.max_blocks &&
           size.operations <= budget.max_operations;
}

std::optional<bool> constant_branch_value(const mir::ControlFlowFunction& function,
                                          const mir::Terminator& terminator) {
    if (terminator.kind != mir::TerminatorKind::branch || terminator.targets.size() != 2 ||
        terminator.condition_value >= function.values.size()) {
        return std::nullopt;
    }
    switch (terminator.branch_role) {
    case mir::BranchRole::condition:
    case mir::BranchRole::match_guard:
    case mir::BranchRole::assertion:
        break;
    case mir::BranchRole::none:
    case mir::BranchRole::iterator_protocol:
    case mir::BranchRole::match_pattern:
        return std::nullopt;
    }
    const auto& value = function.values[terminator.condition_value];
    if (value.kind != typed::ExpressionKind::literal ||
        value.literal_kind != typed::LiteralKind::boolean) {
        return std::nullopt;
    }
    return value.payload == "true";
}

void rebuild_predecessors(mir::ControlFlowFunction& function) {
    for (auto& block : function.blocks)
        block.predecessors.clear();
    for (const auto& block : function.blocks) {
        for (const auto target : block.terminator.targets) {
            if (target < function.blocks.size())
                function.blocks[target].predecessors.push_back(block.id);
        }
    }
    for (auto& block : function.blocks) {
        std::sort(block.predecessors.begin(), block.predecessors.end());
        block.predecessors.erase(std::unique(block.predecessors.begin(), block.predecessors.end()),
                                 block.predecessors.end());
    }
}

void prune_dead_values(mir::ControlFlowFunction& function, MirOptimizationStats& stats) {
    std::vector<bool> live(function.values.size(), false);
    std::vector<mir::ValueId> worklist;
    const auto mark = [&](const mir::ValueId value) {
        if (value < function.values.size())
            worklist.push_back(value);
    };
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            for (const auto input : instruction.inputs)
                mark(input);
        }
        mark(block.terminator.condition_value);
    }
    while (!worklist.empty()) {
        const auto value = worklist.back();
        worklist.pop_back();
        if (live[value])
            continue;
        live[value] = true;
        for (const auto operand : function.values[value].operands)
            mark(operand);
    }

    std::vector<mir::ValueId> remap(function.values.size(), mir::invalid_value);
    std::vector<mir::Value> retained;
    retained.reserve(function.values.size());
    for (std::size_t index = 0; index < function.values.size(); ++index) {
        if (!live[index]) {
            ++stats.values_removed;
            continue;
        }
        remap[index] = static_cast<mir::ValueId>(retained.size());
        retained.push_back(std::move(function.values[index]));
    }
    for (auto& value : retained) {
        value.id = remap[value.id];
        for (auto& operand : value.operands)
            operand = remap[operand];
    }
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            for (auto& input : instruction.inputs)
                input = remap[input];
        }
        if (block.terminator.condition_value != mir::invalid_value)
            block.terminator.condition_value = remap[block.terminator.condition_value];
    }
    function.values = std::move(retained);
}

void prune_unreachable(mir::ControlFlowFunction& function, MirOptimizationStats& stats) {
    std::vector<bool> reachable(function.blocks.size(), false);
    std::vector<mir::BlockId> worklist{function.entry};
    while (!worklist.empty()) {
        const auto block = worklist.back();
        worklist.pop_back();
        if (block >= function.blocks.size() || reachable[block])
            continue;
        reachable[block] = true;
        for (const auto target : function.blocks[block].terminator.targets)
            worklist.push_back(target);
    }

    std::vector<mir::BlockId> remap(function.blocks.size(), mir::invalid_block);
    std::vector<mir::BasicBlock> retained;
    retained.reserve(function.blocks.size());
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        if (!reachable[index]) {
            ++stats.blocks_removed;
            stats.instructions_removed += function.blocks[index].instructions.size();
            continue;
        }
        remap[index] = static_cast<mir::BlockId>(retained.size());
        retained.push_back(std::move(function.blocks[index]));
    }
    for (auto& block : retained) {
        block.id = remap[block.id];
        for (auto& target : block.terminator.targets)
            target = remap[target];
    }
    function.entry = remap[function.entry];
    function.blocks = std::move(retained);
    function.suspends =
        std::any_of(function.blocks.begin(), function.blocks.end(), [](const auto& block) {
            return block.terminator.kind == mir::TerminatorKind::suspend;
        });
    rebuild_predecessors(function);
    mir::canonicalize_operation_ids(function);
    prune_dead_values(function, stats);
}

} // namespace

MirOptimizationStats MirOptimizer::optimize(mir::Module& module) const {
    DiagnosticBag diagnostics;
    return optimize(module, diagnostics);
}

MirOptimizationStats MirOptimizer::optimize(mir::Module& module, DiagnosticBag& diagnostics,
                                            const MirOptimizationBudget& budget) const {
    MirOptimizationStats stats;
    stats.precondition_verified = MirVerifier{diagnostics}.verify(module);
    if (!stats.precondition_verified)
        return stats;

    const auto before = measure(module.functions);
    stats.blocks_before = before.blocks;
    stats.operations_before = before.operations;
    stats.values_before = before.values;
    if (!within_budget(before, budget)) {
        stats.budget_exhausted = true;
        stats.blocks_after = before.blocks;
        stats.operations_after = before.operations;
        stats.values_after = before.values;
        stats.postcondition_verified = true;
        return stats;
    }

    auto original = module.functions;
    for (auto& function : module.functions) {
        ++stats.functions_visited;
        for (auto& block : function.blocks) {
            const auto value = constant_branch_value(function, block.terminator);
            if (!value)
                continue;
            const auto target = block.terminator.targets[*value ? 0U : 1U];
            block.terminator.kind = mir::TerminatorKind::jump;
            block.terminator.condition_value = mir::invalid_value;
            block.terminator.targets = {target};
            block.terminator.branch_role = mir::BranchRole::none;
            ++stats.branches_simplified;
        }
        prune_unreachable(function, stats);
    }

    const auto after = measure(module.functions);
    stats.blocks_after = after.blocks;
    stats.operations_after = after.operations;
    stats.values_after = after.values;
    stats.postcondition_verified = MirVerifier{diagnostics}.verify(module);
    if (!stats.postcondition_verified) {
        module.functions = std::move(original);
        stats.blocks_after = before.blocks;
        stats.operations_after = before.operations;
        stats.values_after = before.values;
        stats.branches_simplified = 0;
        stats.blocks_removed = 0;
        stats.instructions_removed = 0;
        stats.values_removed = 0;
    }
    return stats;
}

} // namespace gdpp
