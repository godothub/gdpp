#include "gdpp/ir/mir.hpp"

#include <algorithm>
#include <functional>
#include <string_view>
#include <unordered_set>

namespace gdpp {
namespace {

bool await_can_suspend(const ir::Statement& statement) {
    if (!statement.expression)
        return false;
    const auto& type = statement.expression->type;
    return statement.expression->coroutine_call || type.is_dynamic() ||
           (type.kind == TypeKind::builtin && type.name == "Signal");
}

class FunctionBuilder final {
  public:
    FunctionBuilder(std::string name, mir::FunctionRole role, SourceSpan span)
        : function_{std::move(name), role, mir::invalid_block, {}, false, span} {
        function_.entry = add_block();
    }

    [[nodiscard]] mir::Function build(const std::vector<ir::Statement>& statements) {
        const auto end = lower_statements(statements, function_.entry, {});
        if (open(end))
            terminate(end, mir::TerminatorKind::stop, {}, nullptr, function_.span);
        prune_unreachable();
        rebuild_predecessors();
        return std::move(function_);
    }

  private:
    struct LoopTargets {
        mir::BlockId break_target{mir::invalid_block};
        mir::BlockId continue_target{mir::invalid_block};
    };

    [[nodiscard]] mir::BlockId add_block() {
        const auto id = static_cast<mir::BlockId>(function_.blocks.size());
        function_.blocks.push_back({id, {}, {}, {}});
        return id;
    }

    [[nodiscard]] bool open(mir::BlockId block) const {
        return block != mir::invalid_block &&
               function_.blocks[block].terminator.kind == mir::TerminatorKind::invalid;
    }

    void terminate(mir::BlockId block, mir::TerminatorKind kind, std::vector<mir::BlockId> targets,
                   const ir::Expression* condition, SourceSpan span,
                   mir::BranchRole branch_role = mir::BranchRole::none) {
        function_.blocks[block].terminator = {kind, condition, std::move(targets), span,
                                              branch_role};
    }

    void append(mir::BlockId block, mir::InstructionKind kind, mir::Effect effects,
                const ir::Statement& statement) {
        function_.blocks[block].instructions.push_back({kind, effects, &statement, statement.span});
    }

    [[nodiscard]] mir::BlockId lower_statements(const std::vector<ir::Statement>& statements,
                                                mir::BlockId current, LoopTargets loop) {
        for (const auto& statement : statements) {
            if (!open(current))
                break;
            current = lower_statement(statement, current, loop);
        }
        return current;
    }

    [[nodiscard]] mir::BlockId lower_if(const ir::Statement& statement, mir::BlockId current,
                                        LoopTargets loop) {
        const auto then_block = add_block();
        const auto else_block = add_block();
        const auto join_block = add_block();
        terminate(current, mir::TerminatorKind::branch, {then_block, else_block},
                  statement.condition.get(), statement.span, mir::BranchRole::condition);
        const auto then_end = lower_statements(statement.body, then_block, loop);
        if (open(then_end))
            terminate(then_end, mir::TerminatorKind::jump, {join_block}, nullptr, statement.span);
        const auto else_end = lower_statements(statement.else_body, else_block, loop);
        if (open(else_end))
            terminate(else_end, mir::TerminatorKind::jump, {join_block}, nullptr, statement.span);
        return join_block;
    }

    [[nodiscard]] mir::BlockId lower_loop(const ir::Statement& statement, mir::BlockId current,
                                          bool iterator_loop) {
        const auto condition_block = add_block();
        const auto body_block = add_block();
        const auto after_block = add_block();
        terminate(current, mir::TerminatorKind::jump, {condition_block}, nullptr, statement.span);
        if (iterator_loop) {
            append(condition_block, mir::InstructionKind::loop_test,
                   mir::Effect::reads_state | mir::Effect::writes_state | mir::Effect::may_fail,
                   statement);
        }
        terminate(condition_block, mir::TerminatorKind::branch, {body_block, after_block},
                  statement.condition.get(), statement.span,
                  iterator_loop ? mir::BranchRole::iterator_protocol : mir::BranchRole::condition);
        const auto body_end =
            lower_statements(statement.body, body_block, {after_block, condition_block});
        if (open(body_end))
            terminate(body_end, mir::TerminatorKind::jump, {condition_block}, nullptr,
                      statement.span);
        return after_block;
    }

    [[nodiscard]] mir::BlockId lower_match(const ir::Statement& statement, mir::BlockId current,
                                           LoopTargets loop) {
        if (statement.body.empty()) {
            append(current, mir::InstructionKind::match_test,
                   mir::Effect::reads_state | mir::Effect::may_fail, statement);
            return current;
        }
        const auto join_block = add_block();
        auto test_block = current;
        for (std::size_t index = 0; index < statement.body.size(); ++index) {
            const auto& branch = statement.body[index];
            const auto pattern_block = add_block();
            const auto fallback = index + 1 < statement.body.size() ? add_block() : join_block;
            append(test_block, mir::InstructionKind::match_test,
                   mir::Effect::reads_state | mir::Effect::may_fail, branch);
            terminate(test_block, mir::TerminatorKind::branch, {pattern_block, fallback},
                      statement.condition.get(), branch.span, mir::BranchRole::match_pattern);
            auto guard_end = lower_statements(branch.guard_prefix, pattern_block, loop);
            auto branch_block = guard_end;
            if (open(guard_end) && branch.expression) {
                branch_block = add_block();
                terminate(guard_end, mir::TerminatorKind::branch, {branch_block, fallback},
                          branch.expression.get(), branch.span, mir::BranchRole::match_guard);
            }
            const auto branch_end = lower_statements(branch.body, branch_block, loop);
            if (open(branch_end))
                terminate(branch_end, mir::TerminatorKind::jump, {join_block}, nullptr,
                          branch.span);
            test_block = fallback;
        }
        return join_block;
    }

    [[nodiscard]] mir::BlockId lower_statement(const ir::Statement& statement, mir::BlockId current,
                                               LoopTargets loop) {
        switch (statement.kind) {
        case ir::StatementKind::expression:
            append(current, mir::InstructionKind::evaluate,
                   mir::Effect::reads_state | mir::Effect::may_fail, statement);
            return current;
        case ir::StatementKind::variable:
            append(current, mir::InstructionKind::declare_variable,
                   mir::Effect::writes_state | mir::Effect::may_allocate, statement);
            return current;
        case ir::StatementKind::assignment:
            append(current, mir::InstructionKind::assign,
                   mir::Effect::reads_state | mir::Effect::writes_state | mir::Effect::may_fail,
                   statement);
            return current;
        case ir::StatementKind::assert_statement:
            if (!statement.assert_condition_prefix.empty() ||
                !statement.assert_message_prefix.empty()) {
                const auto condition_end =
                    lower_statements(statement.assert_condition_prefix, current, loop);
                if (!open(condition_end))
                    return mir::invalid_block;
                const auto success = add_block();
                const auto failure = add_block();
                terminate(condition_end, mir::TerminatorKind::branch, {success, failure},
                          statement.condition.get(), statement.span, mir::BranchRole::assertion);
                const auto message_end =
                    lower_statements(statement.assert_message_prefix, failure, loop);
                if (open(message_end)) {
                    append(message_end, mir::InstructionKind::assert_condition,
                           mir::Effect::reads_state | mir::Effect::may_fail, statement);
                    terminate(message_end, mir::TerminatorKind::stop, {}, nullptr, statement.span);
                }
                return success;
            }
            append(current, mir::InstructionKind::assert_condition,
                   mir::Effect::reads_state | mir::Effect::may_fail, statement);
            return current;
        case ir::StatementKind::breakpoint_statement:
            append(current, mir::InstructionKind::debug_breakpoint,
                   mir::Effect::reads_state | mir::Effect::observes_debugger, statement);
            return current;
        case ir::StatementKind::return_statement:
            terminate(current, mir::TerminatorKind::return_value, {}, statement.expression.get(),
                      statement.span);
            return mir::invalid_block;
        case ir::StatementKind::break_statement:
            terminate(current, mir::TerminatorKind::jump, {loop.break_target}, nullptr,
                      statement.span);
            return mir::invalid_block;
        case ir::StatementKind::continue_statement:
            terminate(current, mir::TerminatorKind::jump, {loop.continue_target}, nullptr,
                      statement.span);
            return mir::invalid_block;
        case ir::StatementKind::if_statement:
            return lower_if(statement, current, loop);
        case ir::StatementKind::while_statement:
            return lower_loop(statement, current, false);
        case ir::StatementKind::for_statement:
            return lower_loop(statement, current, true);
        case ir::StatementKind::match_statement:
            return lower_match(statement, current, loop);
        case ir::StatementKind::await_statement:
        case ir::StatementKind::await_variable: {
            if (!await_can_suspend(statement)) {
                append(current,
                       statement.kind == ir::StatementKind::await_variable
                           ? mir::InstructionKind::declare_variable
                           : mir::InstructionKind::evaluate,
                       mir::Effect::reads_state | mir::Effect::may_fail, statement);
                return current;
            }
            append(current, mir::InstructionKind::suspend_value,
                   mir::Effect::reads_state | mir::Effect::writes_state |
                       mir::Effect::may_allocate | mir::Effect::suspends,
                   statement);
            const auto resume = add_block();
            terminate(current, mir::TerminatorKind::suspend, {resume}, statement.expression.get(),
                      statement.span);
            function_.suspends = true;
            return resume;
        }
        case ir::StatementKind::pass_statement:
            return current;
        case ir::StatementKind::match_branch:
            append(current, mir::InstructionKind::match_test,
                   mir::Effect::reads_state | mir::Effect::may_fail, statement);
            return lower_statements(statement.body, current, loop);
        }
        return current;
    }

    void rebuild_predecessors() {
        for (auto& block : function_.blocks)
            block.predecessors.clear();
        for (const auto& block : function_.blocks) {
            for (const auto target : block.terminator.targets) {
                if (target < function_.blocks.size())
                    function_.blocks[target].predecessors.push_back(block.id);
            }
        }
        for (auto& block : function_.blocks) {
            std::sort(block.predecessors.begin(), block.predecessors.end());
            block.predecessors.erase(
                std::unique(block.predecessors.begin(), block.predecessors.end()),
                block.predecessors.end());
        }
    }

    void prune_unreachable() {
        std::vector<bool> reachable(function_.blocks.size(), false);
        std::vector<mir::BlockId> worklist{function_.entry};
        while (!worklist.empty()) {
            const auto block = worklist.back();
            worklist.pop_back();
            if (block >= function_.blocks.size() || reachable[block])
                continue;
            reachable[block] = true;
            for (const auto target : function_.blocks[block].terminator.targets)
                worklist.push_back(target);
        }
        std::vector<mir::BlockId> remap(function_.blocks.size(), mir::invalid_block);
        std::vector<mir::BasicBlock> retained;
        retained.reserve(function_.blocks.size());
        for (std::size_t index = 0; index < function_.blocks.size(); ++index) {
            if (!reachable[index])
                continue;
            remap[index] = static_cast<mir::BlockId>(retained.size());
            retained.push_back(std::move(function_.blocks[index]));
        }
        for (auto& block : retained) {
            block.id = remap[block.id];
            for (auto& target : block.terminator.targets) {
                if (target < remap.size())
                    target = remap[target];
            }
        }
        function_.entry = remap[function_.entry];
        function_.blocks = std::move(retained);
    }

    mir::Function function_;
};

void collect_expression_lambdas(const ir::Expression& expression, std::string_view owner,
                                std::size_t& lambda_index, std::vector<mir::Function>& output);

void collect_pattern_lambdas(const ir::MatchPattern& pattern, std::string_view owner,
                             std::size_t& lambda_index, std::vector<mir::Function>& output) {
    if (pattern.expression)
        collect_expression_lambdas(*pattern.expression, owner, lambda_index, output);
    for (const auto& key : pattern.keys) {
        if (key)
            collect_expression_lambdas(*key, owner, lambda_index, output);
    }
    for (const auto& element : pattern.elements)
        collect_pattern_lambdas(element, owner, lambda_index, output);
}

void collect_statement_lambdas(const std::vector<ir::Statement>& statements, std::string_view owner,
                               std::size_t& lambda_index, std::vector<mir::Function>& output) {
    for (const auto& statement : statements) {
        if (statement.expression)
            collect_expression_lambdas(*statement.expression, owner, lambda_index, output);
        if (statement.condition)
            collect_expression_lambdas(*statement.condition, owner, lambda_index, output);
        for (const auto& pattern : statement.patterns)
            collect_pattern_lambdas(pattern, owner, lambda_index, output);
        collect_statement_lambdas(statement.body, owner, lambda_index, output);
        collect_statement_lambdas(statement.else_body, owner, lambda_index, output);
        collect_statement_lambdas(statement.guard_prefix, owner, lambda_index, output);
        collect_statement_lambdas(statement.assert_condition_prefix, owner, lambda_index, output);
        collect_statement_lambdas(statement.assert_message_prefix, owner, lambda_index, output);
    }
}

void collect_expression_lambdas(const ir::Expression& expression, std::string_view owner,
                                std::size_t& lambda_index, std::vector<mir::Function>& output) {
    for (const auto& operand : expression.operands)
        collect_expression_lambdas(*operand, owner, lambda_index, output);
    if (!expression.lambda)
        return;
    const auto name = std::string{owner} + "::<lambda#" + std::to_string(lambda_index++) + ">";
    output.push_back(
        FunctionBuilder{name, mir::FunctionRole::lambda, expression.lambda->span}.build(
            expression.lambda->body));
    collect_statement_lambdas(expression.lambda->body, name, lambda_index, output);
}

void lower_functions(const std::vector<ir::Function>& functions, std::string_view owner,
                     std::vector<mir::Function>& output) {
    for (const auto& function : functions) {
        if (function.is_abstract)
            continue;
        const auto name = std::string{owner} + "::" + function.name;
        output.push_back(
            FunctionBuilder{name, mir::FunctionRole::method, function.span}.build(function.body));
        std::size_t lambda_index = 0;
        collect_statement_lambdas(function.body, name, lambda_index, output);
    }
}

void lower_fields(const std::vector<ir::Field>& fields, std::string_view owner,
                  std::vector<mir::Function>& output) {
    for (const auto& field : fields) {
        if (field.getter && field.getter->method.empty()) {
            output.push_back(FunctionBuilder{std::string{owner} + "::<get:" + field.name + ">",
                                             mir::FunctionRole::getter, field.getter->span}
                                 .build(field.getter->body));
        }
        if (field.setter && field.setter->method.empty()) {
            output.push_back(FunctionBuilder{std::string{owner} + "::<set:" + field.name + ">",
                                             mir::FunctionRole::setter, field.setter->span}
                                 .build(field.setter->body));
        }
    }
}

void lower_class(const ir::Class& declaration, std::string_view owner,
                 std::vector<mir::Function>& output) {
    const auto name = std::string{owner} + "::" + declaration.name;
    lower_fields(declaration.fields, name, output);
    lower_functions(declaration.functions, name, output);
    for (const auto& child : declaration.classes)
        lower_class(child, name, output);
}

std::vector<mir::BlockId> expected_predecessors(const mir::Function& function,
                                                mir::BlockId target) {
    std::vector<mir::BlockId> result;
    for (const auto& block : function.blocks) {
        if (std::find(block.terminator.targets.begin(), block.terminator.targets.end(), target) !=
            block.terminator.targets.end()) {
            result.push_back(block.id);
        }
    }
    return result;
}

} // namespace

mir::Module MirLowerer::lower(const ir::Module& module) const {
    mir::Module lowered;
    lowered.hir = &module;
    const auto owner = module.class_name.value_or("<script>");
    lower_fields(module.fields, owner, lowered.functions);
    lower_functions(module.functions, owner, lowered.functions);
    for (const auto& declaration : module.classes)
        lower_class(declaration, owner, lowered.functions);
    return lowered;
}

bool MirVerifier::verify(const mir::Module& module) const {
    if (!module.hir) {
        diagnostics_.error("GDS5100", "MIR module is detached from its typed HIR", {});
        return false;
    }
    bool valid = true;
    for (const auto& function : module.functions) {
        if (function.entry >= function.blocks.size()) {
            diagnostics_.error("GDS5101", "MIR function has an invalid entry block", function.span);
            valid = false;
            continue;
        }
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
        for (std::size_t index = 0; index < function.blocks.size(); ++index) {
            const auto& block = function.blocks[index];
            if (block.id != index) {
                diagnostics_.error("GDS5102", "MIR block IDs are not dense and deterministic",
                                   function.span);
                valid = false;
            }
            const auto target_count = block.terminator.targets.size();
            const bool valid_target_count =
                (block.terminator.kind == mir::TerminatorKind::jump && target_count == 1) ||
                (block.terminator.kind == mir::TerminatorKind::branch && target_count == 2) ||
                (block.terminator.kind == mir::TerminatorKind::suspend && target_count == 1) ||
                ((block.terminator.kind == mir::TerminatorKind::return_value ||
                  block.terminator.kind == mir::TerminatorKind::stop) &&
                 target_count == 0);
            if (!valid_target_count) {
                diagnostics_.error("GDS5103", "MIR block has an invalid or unterminated edge set",
                                   block.terminator.span);
                valid = false;
            }
            const bool branch_contract_valid =
                block.terminator.kind == mir::TerminatorKind::branch
                    ? block.terminator.condition != nullptr &&
                          block.terminator.branch_role != mir::BranchRole::none
                    : block.terminator.branch_role == mir::BranchRole::none;
            if (!branch_contract_valid) {
                diagnostics_.error("GDS5108", "MIR branch has an invalid semantic role",
                                   block.terminator.span);
                valid = false;
            }
            for (const auto target : block.terminator.targets) {
                if (target >= function.blocks.size()) {
                    diagnostics_.error("GDS5104", "MIR edge targets an unknown basic block",
                                       block.terminator.span);
                    valid = false;
                }
            }
            auto expected = expected_predecessors(function, block.id);
            if (expected != block.predecessors) {
                diagnostics_.error("GDS5105",
                                   "MIR predecessor list does not match terminator edges",
                                   function.span);
                valid = false;
            }
            if (!reachable[index]) {
                diagnostics_.error("GDS5106", "MIR contains an unreachable basic block",
                                   function.span);
                valid = false;
            }
            for (const auto& instruction : block.instructions) {
                if (!instruction.source) {
                    diagnostics_.error("GDS5107", "MIR instruction has no typed HIR source",
                                       instruction.span);
                    valid = false;
                }
            }
        }
    }
    return valid;
}

} // namespace gdpp
