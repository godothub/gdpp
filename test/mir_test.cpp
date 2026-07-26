#include "support/test.hpp"

#include "gdpp/ir/mir.hpp"
#include "gdpp/ir/mir_optimizer.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

gdpp::typed::ExpressionPtr literal(std::string value = "true") {
    auto result = std::make_unique<gdpp::typed::Expression>();
    result->kind = gdpp::typed::ExpressionKind::literal;
    result->literal_kind = value == "true" || value == "false" ? gdpp::typed::LiteralKind::boolean
                                                               : gdpp::typed::LiteralKind::integer;
    result->value = std::move(value);
    return result;
}

gdpp::typed::Statement marker(gdpp::typed::StatementKind kind) {
    gdpp::typed::Statement result;
    result.kind = kind;
    return result;
}

gdpp::typed::ExpressionPtr nested_binary(std::size_t depth) {
    if (depth == 0)
        return literal("1");
    auto result = std::make_unique<gdpp::typed::Expression>();
    result->kind = gdpp::typed::ExpressionKind::binary;
    result->type = {gdpp::TypeKind::integer, "int"};
    result->storage_type = result->type;
    result->assignment_type = result->type;
    result->value = "+";
    result->operands.push_back(nested_binary(depth - 1U));
    result->operands.push_back(literal("1"));
    return result;
}

std::vector<gdpp::mir::SourceStatementId>
observable_trace(const gdpp::mir::ControlFlowFunction& function) {
    std::vector<gdpp::mir::SourceStatementId> trace;
    auto block = function.entry;
    std::size_t remaining = function.blocks.size() * 4U + 1U;
    while (block < function.blocks.size() && remaining-- != 0U) {
        const auto& current = function.blocks[block];
        for (const auto& instruction : current.instructions)
            trace.push_back(instruction.source_statement);
        const auto& terminator = current.terminator;
        if (terminator.kind == gdpp::mir::TerminatorKind::jump ||
            terminator.kind == gdpp::mir::TerminatorKind::suspend) {
            block = terminator.targets.front();
            continue;
        }
        if (terminator.kind == gdpp::mir::TerminatorKind::branch) {
            REQUIRE(terminator.condition_value < function.values.size());
            const auto& value = function.values[terminator.condition_value];
            REQUIRE_EQ(value.kind, gdpp::typed::ExpressionKind::literal);
            REQUIRE_EQ(value.literal_kind, gdpp::typed::LiteralKind::boolean);
            block = terminator.targets[value.payload == "true" ? 0U : 1U];
            continue;
        }
        break;
    }
    REQUIRE(remaining != 0U);
    return trace;
}

} // namespace

TEST_CASE("MIR builds explicit branches loops returns and suspension edges") {
    gdpp::typed::Module hir;
    hir.class_name = "ControlFlow";
    gdpp::typed::Function function;
    function.name = "run";

    gdpp::typed::Statement conditional;
    conditional.kind = gdpp::typed::StatementKind::if_statement;
    conditional.condition = literal();
    conditional.body.push_back(marker(gdpp::typed::StatementKind::return_statement));
    conditional.else_body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    function.body.push_back(std::move(conditional));

    gdpp::typed::Statement loop;
    loop.kind = gdpp::typed::StatementKind::while_statement;
    loop.condition = literal();
    loop.body.push_back(marker(gdpp::typed::StatementKind::continue_statement));
    function.body.push_back(std::move(loop));

    gdpp::typed::Statement await;
    await.kind = gdpp::typed::StatementKind::await_statement;
    await.expression = literal();
    function.body.push_back(std::move(await));
    hir.functions.push_back(std::move(function));

    const auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(mir.functions.size(), std::size_t{1});
    REQUIRE_EQ(mir.program.class_name.value_or(""), std::string{"ControlFlow"});
    REQUIRE_EQ(mir.functions.front().source_statement_count, std::uint32_t{6});
    REQUIRE_EQ(mir.functions.front().source_expression_count, std::uint32_t{3});
    REQUIRE(mir.functions.front().suspends);
    REQUIRE(std::any_of(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                        [](const gdpp::mir::BasicBlock& block) {
                            return block.terminator.kind == gdpp::mir::TerminatorKind::branch;
                        }));
    REQUIRE(std::any_of(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                        [](const gdpp::mir::BasicBlock& block) {
                            return block.terminator.kind == gdpp::mir::TerminatorKind::suspend;
                        }));
}

TEST_CASE("MIR optimizer simplifies typed boolean branches and rebuilds dense CFG metadata") {
    gdpp::typed::Module hir;
    hir.class_name = "OptimizedControlFlow";
    gdpp::typed::Function function;
    function.name = "run";

    gdpp::typed::Statement conditional;
    conditional.kind = gdpp::typed::StatementKind::if_statement;
    conditional.condition = literal("true");
    conditional.body.push_back(marker(gdpp::typed::StatementKind::expression));
    conditional.else_body.push_back(marker(gdpp::typed::StatementKind::expression));
    function.body.push_back(std::move(conditional));
    hir.functions.push_back(std::move(function));

    auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    const auto original_blocks = mir.functions.front().blocks.size();
    const auto stats = gdpp::MirOptimizer{}.optimize(mir);
    gdpp::DiagnosticBag diagnostics;

    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE_EQ(stats.branches_simplified, std::size_t{1});
    REQUIRE_EQ(stats.blocks_removed, std::size_t{1});
    REQUIRE_EQ(stats.instructions_removed, std::size_t{1});
    REQUIRE(stats.precondition_verified);
    REQUIRE(stats.postcondition_verified);
    REQUIRE(!stats.budget_exhausted);
    REQUIRE_EQ(stats.functions_visited, std::size_t{1});
    REQUIRE_EQ(stats.blocks_before, original_blocks);
    REQUIRE_EQ(stats.blocks_after, original_blocks - 1U);
    REQUIRE_EQ(stats.values_before, std::size_t{1});
    REQUIRE_EQ(stats.values_after, std::size_t{0});
    REQUIRE_EQ(stats.values_removed, std::size_t{1});
    REQUIRE_EQ(mir.functions.front().blocks.size(), original_blocks - 1U);
    REQUIRE(std::none_of(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                         [](const auto& block) {
                             return block.terminator.kind == gdpp::mir::TerminatorKind::branch;
                         }));

    const auto second = gdpp::MirOptimizer{}.optimize(mir);
    REQUIRE_EQ(second.branches_simplified, std::size_t{0});
    REQUIRE_EQ(second.blocks_removed, std::size_t{0});
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
}

TEST_CASE("MIR optimizer removes dead values and densely remaps live operand graphs") {
    gdpp::typed::Module program;
    gdpp::typed::Function function;
    function.name = "value_liveness";
    gdpp::typed::Statement conditional;
    conditional.kind = gdpp::typed::StatementKind::if_statement;
    conditional.condition = literal("true");
    gdpp::typed::Statement live;
    live.kind = gdpp::typed::StatementKind::expression;
    live.expression = nested_binary(2);
    conditional.body.push_back(std::move(live));
    gdpp::typed::Statement dead;
    dead.kind = gdpp::typed::StatementKind::expression;
    dead.expression = nested_binary(8);
    conditional.else_body.push_back(std::move(dead));
    function.body.push_back(std::move(conditional));
    program.functions.push_back(std::move(function));

    auto mir = gdpp::MirLowerer{}.lower(std::move(program));
    const auto values_before = mir.functions.front().values.size();
    gdpp::DiagnosticBag diagnostics;
    const auto stats = gdpp::MirOptimizer{}.optimize(mir, diagnostics);

    REQUIRE(stats.precondition_verified);
    REQUIRE(stats.postcondition_verified);
    REQUIRE_EQ(stats.values_before, values_before);
    REQUIRE(stats.values_removed > std::size_t{10});
    REQUIRE_EQ(stats.values_after, std::size_t{5});
    REQUIRE_EQ(mir.functions.front().values.size(), std::size_t{5});
    for (std::size_t index = 0; index < mir.functions.front().values.size(); ++index)
        REQUIRE_EQ(mir.functions.front().values[index].id, static_cast<gdpp::mir::ValueId>(index));
    const auto& instruction = mir.functions.front().blocks[1].instructions.front();
    REQUIRE_EQ(instruction.inputs, std::vector<gdpp::mir::ValueId>{0});
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(!diagnostics.has_errors());
}

TEST_CASE("MIR optimizer never treats match selectors as truthy branch conditions") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "match_value";
    gdpp::typed::Statement match;
    match.kind = gdpp::typed::StatementKind::match_statement;
    match.condition = literal("true");
    gdpp::typed::Statement branch;
    branch.kind = gdpp::typed::StatementKind::match_branch;
    gdpp::typed::MatchPattern wildcard;
    wildcard.kind = gdpp::typed::MatchPatternKind::wildcard;
    branch.patterns.push_back(std::move(wildcard));
    branch.body.push_back(marker(gdpp::typed::StatementKind::expression));
    match.body.push_back(std::move(branch));
    function.body.push_back(std::move(match));
    hir.functions.push_back(std::move(function));

    auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    const auto stats = gdpp::MirOptimizer{}.optimize(mir);
    gdpp::DiagnosticBag diagnostics;

    REQUIRE_EQ(stats.branches_simplified, std::size_t{0});
    REQUIRE_EQ(stats.blocks_removed, std::size_t{0});
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(std::any_of(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                        [](const auto& block) {
                            return block.terminator.branch_role ==
                                   gdpp::mir::BranchRole::match_pattern;
                        }));
}

TEST_CASE("MIR suspends for a typed coroutine call with a scalar logical result") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "consume";
    function.is_coroutine = true;

    gdpp::typed::Statement await;
    await.kind = gdpp::typed::StatementKind::await_variable;
    await.name = "value";
    await.declared_type = {gdpp::TypeKind::integer, "int"};
    await.expression = std::make_unique<gdpp::typed::Expression>();
    await.expression->kind = gdpp::typed::ExpressionKind::call;
    await.expression->type = {gdpp::TypeKind::integer, "int"};
    await.expression->coroutine_call = true;
    function.body.push_back(std::move(await));
    hir.functions.push_back(std::move(function));

    const auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(mir.functions.front().suspends);
    REQUIRE(std::any_of(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                        [](const gdpp::mir::BasicBlock& block) {
                            return block.terminator.kind == gdpp::mir::TerminatorKind::suspend;
                        }));
}

TEST_CASE("MIR covers accessors internal methods and lambdas") {
    gdpp::typed::Module hir;
    hir.class_name = "Owners";
    gdpp::typed::Field field;
    field.name = "value";
    field.getter = gdpp::typed::PropertyAccessor{};
    field.getter->body.push_back(marker(gdpp::typed::StatementKind::return_statement));
    hir.fields.push_back(std::move(field));

    gdpp::typed::Function function;
    function.name = "make";
    gdpp::typed::Statement expression_statement;
    expression_statement.kind = gdpp::typed::StatementKind::expression;
    expression_statement.expression = std::make_unique<gdpp::typed::Expression>();
    expression_statement.expression->kind = gdpp::typed::ExpressionKind::lambda;
    expression_statement.expression->lambda = std::make_unique<gdpp::typed::LambdaExpression>();
    expression_statement.expression->lambda->body.push_back(
        marker(gdpp::typed::StatementKind::return_statement));
    function.body.push_back(std::move(expression_statement));
    hir.functions.push_back(std::move(function));

    gdpp::typed::Class inner;
    inner.name = "Inner";
    gdpp::typed::Function inner_method;
    inner_method.name = "call";
    inner_method.body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    inner.functions.push_back(std::move(inner_method));
    hir.classes.push_back(std::move(inner));

    const auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE_EQ(mir.functions.size(), std::size_t{4});
    REQUIRE(std::any_of(mir.functions.begin(), mir.functions.end(), [](const auto& item) {
        return item.role == gdpp::mir::FunctionRole::lambda;
    }));
}

TEST_CASE("MIR places awaited match guards between pattern tests and branch bodies") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "run";

    gdpp::typed::Statement match;
    match.kind = gdpp::typed::StatementKind::match_statement;
    match.condition = literal("1");
    gdpp::typed::Statement branch;
    branch.kind = gdpp::typed::StatementKind::match_branch;
    gdpp::typed::MatchPattern wildcard;
    wildcard.kind = gdpp::typed::MatchPatternKind::wildcard;
    branch.patterns.push_back(std::move(wildcard));
    branch.expression = literal();
    gdpp::typed::Statement await;
    await.kind = gdpp::typed::StatementKind::await_variable;
    await.name = "guard";
    await.expression = literal();
    await.expression->type = {gdpp::TypeKind::builtin, "Signal"};
    branch.guard_prefix.push_back(std::move(await));
    branch.body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    match.body.push_back(std::move(branch));
    function.body.push_back(std::move(match));
    hir.functions.push_back(std::move(function));

    const auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(mir.functions.front().suspends);
    REQUIRE(std::any_of(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                        [](const gdpp::mir::BasicBlock& block) {
                            return block.terminator.kind == gdpp::mir::TerminatorKind::suspend;
                        }));
    REQUIRE(std::count_if(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                          [](const gdpp::mir::BasicBlock& block) {
                              return block.terminator.kind == gdpp::mir::TerminatorKind::branch;
                          }) >= 2);
}

TEST_CASE("MIR keeps awaited assert messages on the failure-only edge") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "validate";

    gdpp::typed::Statement assertion;
    assertion.kind = gdpp::typed::StatementKind::assert_statement;
    assertion.condition = literal();
    assertion.expression = literal();
    gdpp::typed::Statement condition_await;
    condition_await.kind = gdpp::typed::StatementKind::await_variable;
    condition_await.name = "condition";
    condition_await.expression = literal();
    condition_await.expression->type = {gdpp::TypeKind::builtin, "Signal"};
    assertion.assert_condition_prefix.push_back(std::move(condition_await));
    gdpp::typed::Statement message_await;
    message_await.kind = gdpp::typed::StatementKind::await_variable;
    message_await.name = "message";
    message_await.expression = literal();
    message_await.expression->type = {gdpp::TypeKind::builtin, "Signal"};
    assertion.assert_message_prefix.push_back(std::move(message_await));
    function.body.push_back(std::move(assertion));
    function.body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    hir.functions.push_back(std::move(function));

    const auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(mir.functions.front().suspends);
    REQUIRE_EQ(static_cast<std::size_t>(std::count_if(
                   mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                   [](const gdpp::mir::BasicBlock& block) {
                       return block.terminator.kind == gdpp::mir::TerminatorKind::suspend;
                   })),
               std::size_t{2});
    REQUIRE(std::any_of(mir.functions.front().blocks.begin(), mir.functions.front().blocks.end(),
                        [](const gdpp::mir::BasicBlock& block) {
                            return block.terminator.kind == gdpp::mir::TerminatorKind::stop;
                        }));
}

TEST_CASE("MIR keeps breakpoints as ordered debugger-observing instructions") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "inspect";
    function.body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    function.body.push_back(marker(gdpp::typed::StatementKind::breakpoint_statement));
    function.body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    hir.functions.push_back(std::move(function));

    const auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    const auto& instructions = mir.functions.front().blocks.front().instructions;
    REQUIRE_EQ(instructions.size(), std::size_t{1});
    REQUIRE_EQ(instructions.front().kind, gdpp::mir::InstructionKind::debug_breakpoint);
    REQUIRE(
        gdpp::mir::has_effect(instructions.front().effects, gdpp::mir::Effect::observes_debugger));
    REQUIRE(!gdpp::mir::has_effect(instructions.front().effects, gdpp::mir::Effect::writes_state));
}

TEST_CASE("MIR verifier rejects corrupt edge and predecessor metadata") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "broken";
    function.body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    hir.functions.push_back(std::move(function));
    auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    mir.functions.front().blocks.front().terminator.targets.push_back(999);

    gdpp::DiagnosticBag diagnostics;
    REQUIRE(!gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(diagnostics.has_errors());
}

TEST_CASE("MIR optimization budgets preserve verified unoptimized input") {
    gdpp::typed::Module program;
    gdpp::typed::Function function;
    function.name = "budget";
    gdpp::typed::Statement conditional;
    conditional.kind = gdpp::typed::StatementKind::if_statement;
    conditional.condition = literal("true");
    conditional.body.push_back(marker(gdpp::typed::StatementKind::expression));
    conditional.else_body.push_back(marker(gdpp::typed::StatementKind::expression));
    function.body.push_back(std::move(conditional));
    program.functions.push_back(std::move(function));

    auto mir = gdpp::MirLowerer{}.lower(std::move(program));
    const auto before = gdpp::MirSerializer{}.serialize(mir);
    gdpp::DiagnosticBag diagnostics;
    gdpp::MirOptimizationBudget budget;
    budget.max_functions = 0;
    const auto stats = gdpp::MirOptimizer{}.optimize(mir, diagnostics, budget);

    REQUIRE(stats.precondition_verified);
    REQUIRE(stats.postcondition_verified);
    REQUIRE(stats.budget_exhausted);
    REQUIRE_EQ(stats.functions_visited, std::size_t{0});
    REQUIRE_EQ(stats.values_after, stats.values_before);
    REQUIRE_EQ(gdpp::MirSerializer{}.serialize(mir), before);
    REQUIRE(!diagnostics.has_errors());
}

TEST_CASE("MIR optimizer rejects corrupt input without mutating it") {
    gdpp::typed::Module program;
    gdpp::typed::Function function;
    function.name = "invalid_input";
    function.body.push_back(marker(gdpp::typed::StatementKind::pass_statement));
    program.functions.push_back(std::move(function));

    auto mir = gdpp::MirLowerer{}.lower(std::move(program));
    mir.functions.front().blocks.front().terminator.targets.push_back(999);
    const auto before = gdpp::MirSerializer{}.serialize(mir);
    gdpp::DiagnosticBag diagnostics;
    const auto stats = gdpp::MirOptimizer{}.optimize(mir, diagnostics);

    REQUIRE(!stats.precondition_verified);
    REQUIRE(!stats.postcondition_verified);
    REQUIRE_EQ(stats.functions_visited, std::size_t{0});
    REQUIRE_EQ(gdpp::MirSerializer{}.serialize(mir), before);
    REQUIRE(diagnostics.has_errors());
}

TEST_CASE("MIR constant branch optimization preserves deterministic observable traces") {
    std::uint32_t state = 0x9e3779b9U;
    for (std::size_t sample = 0; sample < 256U; ++sample) {
        gdpp::typed::Module program;
        gdpp::typed::Function function;
        function.name = "equivalence_" + std::to_string(sample);
        for (std::size_t branch = 0; branch < 12U; ++branch) {
            state = state * 1664525U + 1013904223U;
            gdpp::typed::Statement conditional;
            conditional.kind = gdpp::typed::StatementKind::if_statement;
            conditional.condition = literal((state & 1U) != 0U ? "true" : "false");
            conditional.body.push_back(marker(gdpp::typed::StatementKind::expression));
            conditional.else_body.push_back(marker(gdpp::typed::StatementKind::expression));
            function.body.push_back(std::move(conditional));
        }
        program.functions.push_back(std::move(function));

        auto mir = gdpp::MirLowerer{}.lower(std::move(program));
        const auto before = observable_trace(mir.functions.front());
        gdpp::DiagnosticBag diagnostics;
        const auto stats = gdpp::MirOptimizer{}.optimize(mir, diagnostics);
        REQUIRE(stats.precondition_verified);
        REQUIRE(stats.postcondition_verified);
        REQUIRE(!diagnostics.has_errors());
        REQUIRE_EQ(observable_trace(mir.functions.front()), before);
    }
}

TEST_CASE("MIR has stable value and operation identities with address-free serialization") {
    const auto make_hir = [] {
        gdpp::typed::Module hir;
        hir.class_name = "Deterministic";
        gdpp::typed::Function function;
        function.name = "compute";

        gdpp::typed::Statement variable;
        variable.kind = gdpp::typed::StatementKind::variable;
        variable.symbol_identity = 17;
        variable.expression = literal("41");
        variable.expression->type = {gdpp::TypeKind::integer, "int"};
        variable.expression->storage_type = variable.expression->type;
        variable.expression->assignment_type = variable.expression->type;
        variable.expression->symbol_identity = 17;
        variable.expression->span = {{12, 2, 9}, {14, 2, 11}};
        variable.span = {{4, 2, 1}, {14, 2, 11}};
        function.body.push_back(std::move(variable));

        gdpp::typed::Statement returned;
        returned.kind = gdpp::typed::StatementKind::return_statement;
        returned.expression = literal("41");
        returned.expression->type = {gdpp::TypeKind::integer, "int"};
        returned.expression->storage_type = returned.expression->type;
        returned.expression->assignment_type = returned.expression->type;
        returned.expression->span = {{23, 3, 9}, {25, 3, 11}};
        returned.span = {{16, 3, 1}, {25, 3, 11}};
        function.body.push_back(std::move(returned));
        hir.functions.push_back(std::move(function));
        return hir;
    };

    const auto first = gdpp::MirLowerer{}.lower(make_hir());
    const auto second = gdpp::MirLowerer{}.lower(make_hir());
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(first));
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(second));
    REQUIRE_EQ(first.functions.front().id, gdpp::mir::FunctionId{0});
    REQUIRE_EQ(first.functions.front().values.size(), std::size_t{2});
    REQUIRE_EQ(first.functions.front().values[0].id, gdpp::mir::ValueId{0});
    REQUIRE_EQ(first.functions.front().values[0].ownership, gdpp::OwnershipKind::value);

    const auto first_snapshot = gdpp::MirSerializer{}.serialize(first);
    const auto second_snapshot = gdpp::MirSerializer{}.serialize(second);
    REQUIRE_EQ(first_snapshot, second_snapshot);
    REQUIRE(first_snapshot.rfind("GDPP_MIR 2\n", 0) == 0);
    REQUIRE(first_snapshot.find("function f0 role method name \"Deterministic::compute\"") !=
            std::string::npos);
    REQUIRE(first_snapshot.find("value v0 kind literal literal integer type integer:\"int\"") !=
            std::string::npos);
    REQUIRE(first_snapshot.find("source_statements 2 source_expressions 2") != std::string::npos);
    REQUIRE(first_snapshot.find("source e0") != std::string::npos);
    REQUIRE(first_snapshot.find("source s0") != std::string::npos);
    REQUIRE(first_snapshot.find("operation o0 instruction declare_variable") != std::string::npos);
    REQUIRE(first_snapshot.find("0x") == std::string::npos);
}

TEST_CASE("MIR verifier rejects corrupt stable identities and value ownership") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "broken_identity";
    gdpp::typed::Statement statement;
    statement.kind = gdpp::typed::StatementKind::expression;
    statement.expression = literal("7");
    statement.expression->type = {gdpp::TypeKind::integer, "int"};
    function.body.push_back(std::move(statement));
    hir.functions.push_back(std::move(function));

    auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    mir.functions.front().values.front().ownership = gdpp::OwnershipKind::object_reference;
    mir.functions.front().blocks.front().instructions.front().id =
        mir.functions.front().blocks.front().terminator.id;

    gdpp::DiagnosticBag diagnostics;
    REQUIRE(!gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE(diagnostics.has_errors());
}

TEST_CASE("MIR verifier rejects source identities outside the owned typed program") {
    gdpp::typed::Module program;
    gdpp::typed::Function function;
    function.name = "source_identity";
    gdpp::typed::Statement statement;
    statement.kind = gdpp::typed::StatementKind::expression;
    statement.expression = literal("7");
    function.body.push_back(std::move(statement));
    program.functions.push_back(std::move(function));

    auto invalid_statement = gdpp::MirLowerer{}.lower(std::move(program));
    auto& control_flow = invalid_statement.functions.front();
    control_flow.blocks.front().instructions.front().source_statement =
        control_flow.source_statement_count;
    gdpp::DiagnosticBag statement_diagnostics;
    REQUIRE(!gdpp::MirVerifier{statement_diagnostics}.verify(invalid_statement));
    REQUIRE(std::any_of(statement_diagnostics.items().begin(), statement_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS5107"; }));

    gdpp::typed::Module expression_program;
    gdpp::typed::Function expression_function;
    expression_function.name = "source_expression";
    gdpp::typed::Statement returned;
    returned.kind = gdpp::typed::StatementKind::return_statement;
    returned.expression = literal("9");
    expression_function.body.push_back(std::move(returned));
    expression_program.functions.push_back(std::move(expression_function));

    auto invalid_expression = gdpp::MirLowerer{}.lower(std::move(expression_program));
    invalid_expression.functions.front().values.front().source_expression =
        invalid_expression.functions.front().source_expression_count;
    gdpp::DiagnosticBag expression_diagnostics;
    REQUIRE(!gdpp::MirVerifier{expression_diagnostics}.verify(invalid_expression));
    REQUIRE(std::any_of(expression_diagnostics.items().begin(),
                        expression_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS5112"; }));
}

TEST_CASE("MIR value registration survives recursive storage growth") {
    gdpp::typed::Module hir;
    gdpp::typed::Function function;
    function.name = "deep_values";
    gdpp::typed::Statement returned;
    returned.kind = gdpp::typed::StatementKind::return_statement;
    returned.expression = nested_binary(128);
    function.body.push_back(std::move(returned));
    hir.functions.push_back(std::move(function));

    const auto mir = gdpp::MirLowerer{}.lower(std::move(hir));
    gdpp::DiagnosticBag diagnostics;
    REQUIRE(gdpp::MirVerifier{diagnostics}.verify(mir));
    REQUIRE_EQ(mir.functions.front().values.size(), std::size_t{257});
    REQUIRE_EQ(mir.functions.front().values.front().operands.size(), std::size_t{2});
    REQUIRE_EQ(mir.functions.front().blocks.front().terminator.condition_value,
               gdpp::mir::ValueId{0});
    REQUIRE(gdpp::MirSerializer{}.serialize(mir).find("value v256") != std::string::npos);
}
