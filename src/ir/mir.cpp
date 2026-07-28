#include "gdpp/ir/mir.hpp"

#include <algorithm>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace gdpp {

MirSourceIndex::MirSourceIndex(const std::vector<typed::Statement>& statements) {
    index_statements(statements);
}

mir::SourceStatementId
MirSourceIndex::statement_id(const typed::Statement& statement) const noexcept {
    const auto found = statement_ids_.find(&statement);
    return found == statement_ids_.end() ? mir::invalid_source_statement : found->second;
}

mir::SourceExpressionId
MirSourceIndex::expression_id(const typed::Expression& expression) const noexcept {
    const auto found = expression_ids_.find(&expression);
    return found == expression_ids_.end() ? mir::invalid_source_expression : found->second;
}

const typed::Statement* MirSourceIndex::statement(const mir::SourceStatementId id) const noexcept {
    return id < statements_.size() ? statements_[id] : nullptr;
}

const typed::Expression*
MirSourceIndex::expression(const mir::SourceExpressionId id) const noexcept {
    return id < expressions_.size() ? expressions_[id] : nullptr;
}

std::size_t MirSourceIndex::statement_count() const noexcept { return statements_.size(); }

std::size_t MirSourceIndex::expression_count() const noexcept { return expressions_.size(); }

void mir::canonicalize_operation_ids(ControlFlowFunction& function) noexcept {
    OperationId next{0};
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions)
            instruction.id = next++;
        block.terminator.id = next++;
    }
}

void MirSourceIndex::index_statements(const std::vector<typed::Statement>& statements) {
    for (const auto& statement : statements)
        index_statement(statement);
}

void MirSourceIndex::index_statement(const typed::Statement& statement) {
    if (statement_ids_.find(&statement) != statement_ids_.end())
        return;
    const auto id = static_cast<mir::SourceStatementId>(statements_.size());
    statements_.push_back(&statement);
    statement_ids_.emplace(&statement, id);

    if (statement.condition)
        index_expression(*statement.condition);
    if (statement.expression)
        index_expression(*statement.expression);
    for (const auto& pattern : statement.patterns)
        index_pattern(pattern);
    index_statements(statement.guard_prefix);
    index_statements(statement.assert_condition_prefix);
    index_statements(statement.assert_message_prefix);
    index_statements(statement.body);
    index_statements(statement.else_body);
}

void MirSourceIndex::index_pattern(const typed::MatchPattern& pattern) {
    if (pattern.expression)
        index_expression(*pattern.expression);
    for (const auto& key : pattern.keys) {
        if (key)
            index_expression(*key);
    }
    for (const auto& element : pattern.elements)
        index_pattern(element);
}

void MirSourceIndex::index_expression(const typed::Expression& expression) {
    if (expression_ids_.find(&expression) != expression_ids_.end())
        return;
    const auto id = static_cast<mir::SourceExpressionId>(expressions_.size());
    expressions_.push_back(&expression);
    expression_ids_.emplace(&expression, id);
    for (const auto& operand : expression.operands) {
        if (operand)
            index_expression(*operand);
    }
}

namespace {

bool await_can_suspend(const typed::Statement& statement) {
    if (!statement.expression)
        return false;
    const auto& type = statement.expression->type;
    return statement.expression->coroutine_call || type.is_dynamic() ||
           (type.kind == TypeKind::builtin && type.name == "Signal");
}

class FunctionBuilder final {
  public:
    FunctionBuilder(std::string name, mir::FunctionRole role, SourceSpan span) : function_{} {
        function_.name = std::move(name);
        function_.role = role;
        function_.span = span;
        function_.entry = add_block();
    }

    [[nodiscard]] mir::ControlFlowFunction build(const std::vector<typed::Statement>& statements) {
        MirSourceIndex source_index{statements};
        source_index_ = &source_index;
        function_.source_statement_count =
            static_cast<std::uint32_t>(source_index.statement_count());
        function_.source_expression_count =
            static_cast<std::uint32_t>(source_index.expression_count());
        const auto end = lower_statements(statements, function_.entry, {});
        if (open(end))
            terminate(end, mir::TerminatorKind::stop, {}, nullptr, function_.span);
        prune_unreachable();
        rebuild_predecessors();
        mir::canonicalize_operation_ids(function_);
        source_index_ = nullptr;
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
                   const typed::Expression* condition, SourceSpan span,
                   mir::BranchRole branch_role = mir::BranchRole::none) {
        auto& terminator = function_.blocks[block].terminator;
        terminator.kind = kind;
        terminator.condition_value = register_expression(condition);
        terminator.targets = std::move(targets);
        terminator.span = span;
        terminator.branch_role = branch_role;
    }

    void append(mir::BlockId block, mir::InstructionKind kind, mir::Effect effects,
                const typed::Statement& statement) {
        mir::Instruction instruction;
        instruction.kind = kind;
        instruction.effects = effects;
        instruction.source_statement = source_index_->statement_id(statement);
        append_statement_inputs(statement, instruction.inputs);
        instruction.span = statement.span;
        function_.blocks[block].instructions.push_back(std::move(instruction));
    }

    [[nodiscard]] mir::ValueId register_expression(const typed::Expression* expression) {
        if (!expression)
            return mir::invalid_value;
        if (const auto existing = value_ids_.find(expression); existing != value_ids_.end())
            return existing->second;

        const auto id = static_cast<mir::ValueId>(function_.values.size());
        value_ids_.emplace(expression, id);
        mir::Value value;
        value.id = id;
        value.kind = expression->kind;
        value.type = expression->type;
        value.storage_type = expression->storage_type;
        value.assignment_type = expression->assignment_type;
        value.ownership = expression->type.ownership();
        value.non_null = expression->non_null;
        value.literal_kind = expression->literal_kind;
        value.resolution = expression->resolution;
        value.payload = expression->value;
        value.resolved_owner = expression->resolved_owner;
        value.getter = expression->getter;
        value.setter = expression->setter;
        value.direct_access = expression->direct_access;
        value.callable_required_arguments = expression->callable_required_arguments;
        value.callable_maximum_arguments = expression->callable_maximum_arguments;
        value.callable_is_vararg = expression->callable_is_vararg;
        value.coroutine_call = expression->coroutine_call;
        value.indexed_argument = expression->indexed_argument;
        value.symbol_identity = expression->symbol_identity;
        value.intrinsic = expression->intrinsic;
        value.call_contract = expression->call_contract;
        value.span = expression->span;
        value.source_expression = source_index_->expression_id(*expression);
        function_.values.push_back(std::move(value));

        std::vector<mir::ValueId> operands;
        operands.reserve(expression->operands.size());
        for (const auto& operand : expression->operands)
            operands.push_back(register_expression(operand.get()));
        function_.values[id].operands = std::move(operands);
        return id;
    }

    void append_pattern_inputs(const typed::MatchPattern& pattern,
                               std::vector<mir::ValueId>& inputs) {
        if (pattern.expression)
            inputs.push_back(register_expression(pattern.expression.get()));
        for (const auto& key : pattern.keys) {
            if (key)
                inputs.push_back(register_expression(key.get()));
        }
        for (const auto& element : pattern.elements)
            append_pattern_inputs(element, inputs);
    }

    void append_statement_inputs(const typed::Statement& statement,
                                 std::vector<mir::ValueId>& inputs) {
        if (statement.condition)
            inputs.push_back(register_expression(statement.condition.get()));
        if (statement.expression)
            inputs.push_back(register_expression(statement.expression.get()));
        for (const auto& pattern : statement.patterns)
            append_pattern_inputs(pattern, inputs);
    }

    [[nodiscard]] mir::BlockId lower_statements(const std::vector<typed::Statement>& statements,
                                                mir::BlockId current, LoopTargets loop) {
        for (const auto& statement : statements) {
            if (!open(current))
                break;
            current = lower_statement(statement, current, loop);
        }
        return current;
    }

    [[nodiscard]] mir::BlockId lower_if(const typed::Statement& statement, mir::BlockId current,
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

    [[nodiscard]] mir::BlockId lower_loop(const typed::Statement& statement, mir::BlockId current,
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

    [[nodiscard]] mir::BlockId lower_match(const typed::Statement& statement, mir::BlockId current,
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

    [[nodiscard]] mir::BlockId lower_statement(const typed::Statement& statement,
                                               mir::BlockId current, LoopTargets loop) {
        switch (statement.kind) {
        case typed::StatementKind::expression:
            append(current, mir::InstructionKind::evaluate,
                   mir::Effect::reads_state | mir::Effect::may_fail, statement);
            return current;
        case typed::StatementKind::variable:
            append(current, mir::InstructionKind::declare_variable,
                   mir::Effect::writes_state | mir::Effect::may_allocate, statement);
            return current;
        case typed::StatementKind::assignment:
            append(current, mir::InstructionKind::assign,
                   mir::Effect::reads_state | mir::Effect::writes_state | mir::Effect::may_fail,
                   statement);
            return current;
        case typed::StatementKind::assert_statement:
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
        case typed::StatementKind::breakpoint_statement:
            append(current, mir::InstructionKind::debug_breakpoint,
                   mir::Effect::reads_state | mir::Effect::observes_debugger, statement);
            return current;
        case typed::StatementKind::return_statement:
            terminate(current, mir::TerminatorKind::return_value, {}, statement.expression.get(),
                      statement.span);
            return mir::invalid_block;
        case typed::StatementKind::break_statement:
            terminate(current, mir::TerminatorKind::jump, {loop.break_target}, nullptr,
                      statement.span);
            return mir::invalid_block;
        case typed::StatementKind::continue_statement:
            terminate(current, mir::TerminatorKind::jump, {loop.continue_target}, nullptr,
                      statement.span);
            return mir::invalid_block;
        case typed::StatementKind::if_statement:
            return lower_if(statement, current, loop);
        case typed::StatementKind::while_statement:
            return lower_loop(statement, current, false);
        case typed::StatementKind::for_statement:
            return lower_loop(statement, current, true);
        case typed::StatementKind::match_statement:
            return lower_match(statement, current, loop);
        case typed::StatementKind::await_statement:
        case typed::StatementKind::await_variable: {
            if (!await_can_suspend(statement)) {
                append(current,
                       statement.kind == typed::StatementKind::await_variable
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
        case typed::StatementKind::pass_statement:
            return current;
        case typed::StatementKind::match_branch:
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

    mir::ControlFlowFunction function_;
    std::unordered_map<const typed::Expression*, mir::ValueId> value_ids_;
    const MirSourceIndex* source_index_{nullptr};
};

void collect_expression_lambdas(const typed::Expression& expression, std::string_view owner,
                                std::size_t& lambda_index,
                                std::vector<mir::ControlFlowFunction>& output);

void collect_pattern_lambdas(const typed::MatchPattern& pattern, std::string_view owner,
                             std::size_t& lambda_index,
                             std::vector<mir::ControlFlowFunction>& output) {
    if (pattern.expression)
        collect_expression_lambdas(*pattern.expression, owner, lambda_index, output);
    for (const auto& key : pattern.keys) {
        if (key)
            collect_expression_lambdas(*key, owner, lambda_index, output);
    }
    for (const auto& element : pattern.elements)
        collect_pattern_lambdas(element, owner, lambda_index, output);
}

void collect_statement_lambdas(const std::vector<typed::Statement>& statements,
                               std::string_view owner, std::size_t& lambda_index,
                               std::vector<mir::ControlFlowFunction>& output) {
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

void collect_expression_lambdas(const typed::Expression& expression, std::string_view owner,
                                std::size_t& lambda_index,
                                std::vector<mir::ControlFlowFunction>& output) {
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

void lower_functions(const std::vector<typed::Function>& functions, std::string_view owner,
                     std::vector<mir::ControlFlowFunction>& output) {
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

void lower_fields(const std::vector<typed::Field>& fields, std::string_view owner,
                  std::vector<mir::ControlFlowFunction>& output) {
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

void lower_class(const typed::Class& declaration, std::string_view owner,
                 std::vector<mir::ControlFlowFunction>& output) {
    const auto name = std::string{owner} + "::" + declaration.name;
    lower_fields(declaration.fields, name, output);
    lower_functions(declaration.functions, name, output);
    for (const auto& child : declaration.classes)
        lower_class(child, name, output);
}

std::vector<mir::BlockId> expected_predecessors(const mir::ControlFlowFunction& function,
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

mir::Module MirLowerer::lower(typed::Module module) const {
    mir::Module lowered;
    const auto owner = module.class_name.value_or("<script>");
    lower_fields(module.fields, owner, lowered.functions);
    lower_functions(module.functions, owner, lowered.functions);
    for (const auto& declaration : module.classes)
        lower_class(declaration, owner, lowered.functions);
    for (std::size_t index = 0; index < lowered.functions.size(); ++index)
        lowered.functions[index].id = static_cast<mir::FunctionId>(index);
    lowered.program = std::move(module);
    return lowered;
}

bool MirVerifier::verify(const mir::Module& module) const {
    if (module.format_version != mir::schema_version) {
        diagnostics_.error("GDS5109", "MIR module has an unsupported schema version", {});
        return false;
    }
    bool valid = true;
    for (std::size_t function_index = 0; function_index < module.functions.size();
         ++function_index) {
        const auto& function = module.functions[function_index];
        if (function.id != function_index) {
            diagnostics_.error("GDS5110", "MIR function IDs are not dense and deterministic",
                               function.span);
            valid = false;
        }
        if (function.entry >= function.blocks.size()) {
            diagnostics_.error("GDS5101", "MIR function has an invalid entry block", function.span);
            valid = false;
            continue;
        }
        for (std::size_t value_index = 0; value_index < function.values.size(); ++value_index) {
            const auto& value = function.values[value_index];
            if (value.id != value_index) {
                diagnostics_.error("GDS5111", "MIR value IDs are not dense and deterministic",
                                   value.span);
                valid = false;
            }
            if (value.source_expression >= function.source_expression_count) {
                diagnostics_.error("GDS5112", "MIR value has an invalid typed source identity",
                                   value.span);
                valid = false;
            }
            if (value.ownership != value.type.ownership()) {
                diagnostics_.error("GDS5113", "MIR value ownership contradicts its semantic type",
                                   value.span);
                valid = false;
            }
            for (const auto operand : value.operands) {
                if (operand >= function.values.size()) {
                    diagnostics_.error("GDS5114", "MIR value references an unknown operand",
                                       value.span);
                    valid = false;
                }
            }
        }
        std::size_t operation_count = 0;
        for (const auto& block : function.blocks)
            operation_count += block.instructions.size() + 1U;
        std::vector<bool> operation_ids(operation_count, false);
        std::size_t expected_operation = 0;
        bool operation_order_valid = true;
        const auto record_operation = [&](const mir::OperationId operation, const SourceSpan span) {
            if (operation == mir::invalid_operation) {
                diagnostics_.error("GDS5115", "MIR operation has no stable identity", span);
                valid = false;
                operation_order_valid = false;
                ++expected_operation;
                return;
            }
            if (operation >= operation_ids.size()) {
                operation_order_valid = false;
                ++expected_operation;
                return;
            }
            if (operation_ids[operation]) {
                diagnostics_.error("GDS5116", "MIR operation identity is duplicated", span);
                valid = false;
            }
            operation_ids[operation] = true;
            if (operation != expected_operation)
                operation_order_valid = false;
            ++expected_operation;
        };
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
                    ? block.terminator.condition_value < function.values.size() &&
                          block.terminator.branch_role != mir::BranchRole::none
                    : block.terminator.branch_role == mir::BranchRole::none &&
                          (block.terminator.condition_value == mir::invalid_value ||
                           block.terminator.condition_value < function.values.size());
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
                record_operation(instruction.id, instruction.span);
                if (instruction.source_statement >= function.source_statement_count) {
                    diagnostics_.error("GDS5107",
                                       "MIR instruction has an invalid typed source identity",
                                       instruction.span);
                    valid = false;
                }
                for (const auto input : instruction.inputs) {
                    if (input >= function.values.size()) {
                        diagnostics_.error("GDS5117", "MIR instruction references an unknown value",
                                           instruction.span);
                        valid = false;
                    }
                }
            }
            record_operation(block.terminator.id, block.terminator.span);
        }
        if (!operation_order_valid ||
            std::find(operation_ids.begin(), operation_ids.end(), false) != operation_ids.end()) {
            diagnostics_.error("GDS5118", "MIR operation IDs are not dense and deterministic",
                               function.span);
            valid = false;
        }
    }
    return valid;
}

} // namespace gdpp
