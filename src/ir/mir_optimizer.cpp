#include "gdpp/ir/mir_optimizer.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace gdpp {
namespace {

std::optional<bool> constant_branch_value(const mir::Terminator& terminator) {
    if (terminator.kind != mir::TerminatorKind::branch || !terminator.condition ||
        terminator.targets.size() != 2) {
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
    const auto& expression = *terminator.condition;
    if (expression.kind != ir::ExpressionKind::literal ||
        expression.literal_kind != ir::LiteralKind::boolean) {
        return std::nullopt;
    }
    return expression.value == "true";
}

void rebuild_predecessors(mir::Function& function) {
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

void rebuild_operation_ids(mir::Function& function) {
    mir::OperationId next{0};
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions)
            instruction.id = next++;
        block.terminator.id = next++;
    }
}

void prune_unreachable(mir::Function& function, MirOptimizationStats& stats) {
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
    rebuild_operation_ids(function);
}

} // namespace

MirOptimizationStats MirOptimizer::optimize(mir::Module& module) const {
    MirOptimizationStats stats;
    for (auto& function : module.functions) {
        for (auto& block : function.blocks) {
            const auto value = constant_branch_value(block.terminator);
            if (!value)
                continue;
            const auto target = block.terminator.targets[*value ? 0U : 1U];
            block.terminator.kind = mir::TerminatorKind::jump;
            block.terminator.condition = nullptr;
            block.terminator.condition_value = mir::invalid_value;
            block.terminator.targets = {target};
            block.terminator.branch_role = mir::BranchRole::none;
            ++stats.branches_simplified;
        }
        prune_unreachable(function, stats);
    }
    return stats;
}

} // namespace gdpp
