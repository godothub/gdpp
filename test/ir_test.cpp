#include "support/test.hpp"

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/parser.hpp"
#include "gdpp/ir/lowering.hpp"
#include "gdpp/ir/mir.hpp"
#include "gdpp/ir/optimizer.hpp"
#include "gdpp/semantic/analyzer.hpp"

#include <algorithm>

namespace {

gdpp::ir::Module
lower_source(const std::string& text, gdpp::DiagnosticBag& diagnostics,
             const gdpp::GodotVersion target_version = gdpp::minimum_godot_version) {
    const gdpp::SourceFile source{"ir_test.gd", text};
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();
    gdpp::SemanticAnalyzer analyzer{diagnostics, gdpp::GodotApi::for_version(target_version)};
    const auto semantic = analyzer.analyze(script);
    const gdpp::IrLowerer lowerer{semantic};
    return lowerer.lower(script);
}

bool contains_await_expression(const gdpp::ir::Expression& expression) {
    if (expression.kind == gdpp::ir::ExpressionKind::await_expression)
        return true;
    return std::any_of(expression.operands.begin(), expression.operands.end(),
                       [](const auto& operand) { return contains_await_expression(*operand); });
}

bool contains_await_expression(const gdpp::ir::Statement& statement) {
    if ((statement.expression && contains_await_expression(*statement.expression)) ||
        (statement.condition && contains_await_expression(*statement.condition))) {
        return true;
    }
    return std::any_of(statement.body.begin(), statement.body.end(),
                       [](const auto& child) { return contains_await_expression(child); }) ||
           std::any_of(statement.else_body.begin(), statement.else_body.end(),
                       [](const auto& child) { return contains_await_expression(child); }) ||
           std::any_of(statement.guard_prefix.begin(), statement.guard_prefix.end(),
                       [](const auto& child) { return contains_await_expression(child); }) ||
           std::any_of(statement.assert_condition_prefix.begin(),
                       statement.assert_condition_prefix.end(),
                       [](const auto& child) { return contains_await_expression(child); }) ||
           std::any_of(statement.assert_message_prefix.begin(),
                       statement.assert_message_prefix.end(),
                       [](const auto& child) { return contains_await_expression(child); });
}

} // namespace

TEST_CASE("typed IR owns resolved declaration and expression types") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("var answer := 40 + 2\n", diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.fields.size(), std::size_t{1});
    REQUIRE_EQ(module.fields.front().type.kind, gdpp::TypeKind::integer);
    REQUIRE_EQ(module.fields.front().initializer->type.kind, gdpp::TypeKind::integer);
    REQUIRE_EQ(module.fields.front().initializer->kind, gdpp::ir::ExpressionKind::binary);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves Godot default argument evaluation contracts") {
    gdpp::DiagnosticBag diagnostics;
    const auto module =
        lower_source("extends Node\n"
                     "var seed: int = 7\n"
                     "func make_value() -> int:\n"
                     "    return seed\n"
                     "func classify(required: int, scalar: int = 1 + 2, "
                     "component: float = Vector2(1, 2).x, wave: float = sin(0.5), "
                     "width: int = len(\"ab\"), runtime: int = make_value(), "
                     "instance_value: int = seed, values: Array = [1, 2]) -> void:\n"
                     "    pass\n",
                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto function =
        std::find_if(module.functions.begin(), module.functions.end(),
                     [](const auto& candidate) { return candidate.name == "classify"; });
    REQUIRE(function != module.functions.end());
    REQUIRE_EQ(function->parameters.size(), std::size_t{8});
    REQUIRE_EQ(function->parameters[0].default_evaluation, gdpp::DefaultArgumentEvaluation::absent);
    for (const auto index : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
        REQUIRE_EQ(function->parameters[index].default_evaluation,
                   gdpp::DefaultArgumentEvaluation::compile_time_constant);
    }
    for (const auto index : {std::size_t{5}, std::size_t{6}, std::size_t{7}}) {
        REQUIRE_EQ(function->parameters[index].default_evaluation,
                   gdpp::DefaultArgumentEvaluation::call_time);
    }

    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR owns function and lambda rest parameters") {
    gdpp::DiagnosticBag diagnostics;
    auto module =
        lower_source("func collect(prefix: String, ...values: Array) -> int:\n"
                     "    var count := func(...parts: Array) -> int: return parts.size()\n"
                     "    return values.size() + count.call(prefix)\n",
                     diagnostics, gdpp::GodotVersion::v4_6);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.size(), std::size_t{1});
    REQUIRE(module.functions.front().rest_parameter.has_value());
    REQUIRE_EQ(module.functions.front().rest_parameter->name, std::string{"values"});
    REQUIRE_EQ(module.functions.front().rest_parameter->type.kind, gdpp::TypeKind::array);
    REQUIRE(!module.functions.front().rest_parameter->default_value);
    const auto& lambda = module.functions.front().body.front().expression->lambda;
    REQUIRE(lambda != nullptr);
    REQUIRE(lambda->rest_parameter.has_value());
    REQUIRE_EQ(lambda->rest_parameter->name, std::string{"parts"});

    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));

    module.functions.front().rest_parameter->type = {gdpp::TypeKind::dictionary, "Dictionary"};
    gdpp::DiagnosticBag invalid_diagnostics;
    gdpp::IrVerifier invalid_verifier{invalid_diagnostics};
    REQUIRE(!invalid_verifier.verify(module));
    REQUIRE(std::any_of(invalid_diagnostics.items().begin(), invalid_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS5043"; }));
}

TEST_CASE("typed IR owns resolved script call contracts") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source(
        "func consume(value: PackedByteArray, optional: int = 1, ...extras: Array) -> void:\n"
        "    value.append(optional)\n"
        "func forward(value) -> void:\n"
        "    consume(value, 2, \"tail\")\n",
        diagnostics, gdpp::GodotVersion::v4_6);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.size(), std::size_t{2});
    auto& call = *module.functions.at(1).body.front().expression;
    REQUIRE_EQ(call.kind, gdpp::ir::ExpressionKind::call);
    REQUIRE(call.call_contract.has_value());
    REQUIRE_EQ(call.call_contract->parameters.size(), std::size_t{2});
    REQUIRE_EQ(call.call_contract->parameters.front().kind, gdpp::TypeKind::builtin);
    REQUIRE_EQ(call.call_contract->parameters.front().name, std::string{"PackedByteArray"});
    REQUIRE_EQ(call.call_contract->parameters.at(1).kind, gdpp::TypeKind::integer);
    REQUIRE_EQ(call.call_contract->required_arguments, std::size_t{1});
    REQUIRE(call.call_contract->is_vararg);

    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));

    call.call_contract->required_arguments = 3;
    gdpp::DiagnosticBag invalid_diagnostics;
    gdpp::IrVerifier invalid_verifier{invalid_diagnostics};
    REQUIRE(!invalid_verifier.verify(module));
    REQUIRE(std::any_of(invalid_diagnostics.items().begin(), invalid_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS5047"; }));
}

TEST_CASE("typed IR preserves shared container ownership through parameters") {
    gdpp::DiagnosticBag diagnostics;
    auto module =
        lower_source("func mutate(bytes: PackedByteArray, values: Array, scalar: int) -> void:\n"
                     "    bytes.append(1)\n"
                     "    values.append(2)\n",
                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.front().parameters.at(0).ownership,
               gdpp::OwnershipKind::shared_container);
    REQUIRE_EQ(module.functions.front().parameters.at(1).ownership,
               gdpp::OwnershipKind::shared_container);
    REQUIRE_EQ(module.functions.front().parameters.at(2).ownership, gdpp::OwnershipKind::value);

    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));

    module.functions.front().parameters.front().ownership = gdpp::OwnershipKind::value;
    REQUIRE(!verifier.verify(module));
    REQUIRE(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS5045"; }));
}

TEST_CASE("typed IR rejects missing default argument evaluation contracts") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func invoke(value: int = 1) -> void:\n"
                               "    pass\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());
    module.functions.front().parameters.front().default_evaluation =
        gdpp::DefaultArgumentEvaluation::absent;

    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(!verifier.verify(module));
    REQUIRE(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS5038"; }));
}

TEST_CASE("typed IR models abstract methods as non-executable contracts") {
    gdpp::ir::Module module;
    module.class_name = "AbstractWorker";
    module.is_abstract = true;
    gdpp::ir::Function function;
    function.name = "execute";
    function.return_type = {gdpp::TypeKind::void_type, "void"};
    function.is_abstract = true;
    module.functions.push_back(std::move(function));

    gdpp::DiagnosticBag diagnostics;
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
    const auto mir = gdpp::MirLowerer{}.lower(module);
    REQUIRE(mir.functions.empty());

    gdpp::ir::Statement body_statement;
    body_statement.kind = gdpp::ir::StatementKind::pass_statement;
    module.functions.front().body.push_back(std::move(body_statement));
    gdpp::DiagnosticBag invalid_diagnostics;
    gdpp::IrVerifier invalid_verifier{invalid_diagnostics};
    REQUIRE(!invalid_verifier.verify(module));
    REQUIRE(std::any_of(invalid_diagnostics.items().begin(), invalid_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS5039"; }));
}

TEST_CASE("typed IR preserves parsed abstract classes and method signatures") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("@abstract\n"
                                     "class_name AbstractWorker\n"
                                     "@abstract\n"
                                     "func execute(value: int) -> String\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(module.is_abstract);
    REQUIRE_EQ(module.functions.size(), std::size_t{1});
    REQUIRE(module.functions.front().is_abstract);
    REQUIRE(module.functions.front().body.empty());
    REQUIRE_EQ(module.functions.front().parameters.size(), std::size_t{1});
    REQUIRE_EQ(module.functions.front().return_type.kind, gdpp::TypeKind::string);

    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
    REQUIRE(gdpp::MirLowerer{}.lower(module).functions.empty());
}

TEST_CASE("typed IR preserves script tool execution mode") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("@tool\n"
                                     "extends Node\n"
                                     "func editor_tick() -> void:\n"
                                     "    pass\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(module.is_tool);
}

TEST_CASE("typed IR preserves script icon metadata") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("@icon(\"icons/custom.svg\")\n"
                                     "class_name IconClass\n"
                                     "extends Node\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(module.icon_path.has_value());
    REQUIRE_EQ(*module.icon_path, std::string{"icons/custom.svg"});
}

TEST_CASE("typed IR preserves flow-proven non-null object reads") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("extends Node\n"
                                     "func guarded_name(value: Node) -> String:\n"
                                     "    if value != null:\n"
                                     "        return value.name\n"
                                     "    return \"\"\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& conditional = module.functions.front().body.front();
    REQUIRE_EQ(conditional.kind, gdpp::ir::StatementKind::if_statement);
    REQUIRE_EQ(conditional.body.front().kind, gdpp::ir::StatementKind::return_statement);
    const auto& member = *conditional.body.front().expression;
    REQUIRE_EQ(member.kind, gdpp::ir::ExpressionKind::member);
    REQUIRE(member.operands.front()->non_null);
}

TEST_CASE("typed IR preserves semantic iteration plans") {
    gdpp::DiagnosticBag diagnostics;
    const auto module =
        lower_source("func visit(values: Array[int], labels: Dictionary[String, int]) -> void:\n"
                     "    for value in values:\n"
                     "        pass\n"
                     "    for key in labels:\n"
                     "        pass\n"
                     "    for character in \"A🙂B\":\n"
                     "        pass\n",
                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.front().body.size(), std::size_t{3});
    REQUIRE_EQ(module.functions.front().body[0].iteration.strategy,
               gdpp::IterationStrategy::indexed_array);
    REQUIRE_EQ(module.functions.front().body[0].iteration.element_type,
               (gdpp::Type{gdpp::TypeKind::integer, "int"}));
    REQUIRE_EQ(module.functions.front().body[1].iteration.strategy,
               gdpp::IterationStrategy::dictionary_protocol);
    REQUIRE_EQ(module.functions.front().body[1].iteration.element_type,
               (gdpp::Type{gdpp::TypeKind::string, "String"}));
    REQUIRE_EQ(module.functions.front().body[2].iteration.strategy,
               gdpp::IterationStrategy::indexed_string);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves Godot mathematical range iteration plans") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source(
        "func visit(count: float, bounds: Vector2, int_bounds: Vector2i, steps: Vector3, "
        "int_steps: Vector3i) -> void:\n"
        "    for value: float in count:\n"
        "        pass\n"
        "    for value: float in bounds:\n"
        "        pass\n"
        "    for value: int in int_bounds:\n"
        "        pass\n"
        "    for value: float in steps:\n"
        "        pass\n"
        "    for value: int in int_steps:\n"
        "        pass\n",
        diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& body = module.functions.front().body;
    REQUIRE_EQ(body.at(0).iteration.strategy, gdpp::IterationStrategy::floating_count);
    REQUIRE_EQ(body.at(1).iteration.strategy, gdpp::IterationStrategy::vector2_range);
    REQUIRE_EQ(body.at(2).iteration.strategy, gdpp::IterationStrategy::vector2i_range);
    REQUIRE_EQ(body.at(3).iteration.strategy, gdpp::IterationStrategy::vector3_range);
    REQUIRE_EQ(body.at(4).iteration.strategy, gdpp::IterationStrategy::vector3i_range);
    REQUIRE_EQ(body.at(0).iteration.element_type, (gdpp::Type{gdpp::TypeKind::floating, "float"}));
    REQUIRE_EQ(body.at(2).iteration.element_type, (gdpp::Type{gdpp::TypeKind::integer, "int"}));
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR resolves statically known object iterator protocols") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("class Iterator:\n"
                                     "    func _iter_init(state: Array) -> bool:\n"
                                     "        return true\n"
                                     "    func _iter_next(state: Array) -> bool:\n"
                                     "        return false\n"
                                     "    func _iter_get(state: Variant) -> StringName:\n"
                                     "        return &\"value\"\n"
                                     "func visit(iterator: Iterator) -> void:\n"
                                     "    for value in iterator:\n"
                                     "        pass\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& loop = module.functions.front().body.front();
    REQUIRE_EQ(loop.iteration.strategy, gdpp::IterationStrategy::object_protocol);
    REQUIRE_EQ(loop.iteration.element_type,
               (gdpp::Type{gdpp::TypeKind::string_name, "StringName"}));
    REQUIRE_EQ(loop.declared_type, (gdpp::Type{gdpp::TypeKind::string_name, "StringName"}));
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("semantic analysis rejects incomplete object iterator protocols") {
    gdpp::DiagnosticBag missing_diagnostics;
    (void)lower_source("func visit(node: Node) -> void:\n"
                       "    for value in node:\n"
                       "        pass\n",
                       missing_diagnostics);
    gdpp::DiagnosticBag signature_diagnostics;
    (void)lower_source("class Iterator:\n"
                       "    func _iter_init() -> bool:\n"
                       "        return true\n"
                       "    func _iter_next(state: Array) -> int:\n"
                       "        return 0\n"
                       "    func _iter_get(state: Variant) -> void:\n"
                       "        pass\n"
                       "func visit(iterator: Iterator) -> void:\n"
                       "    for value in iterator:\n"
                       "        pass\n",
                       signature_diagnostics);

    REQUIRE(missing_diagnostics.has_errors());
    REQUIRE(std::any_of(missing_diagnostics.items().begin(), missing_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS4140"; }));
    REQUIRE(signature_diagnostics.has_errors());
    REQUIRE(std::count_if(signature_diagnostics.items().begin(),
                          signature_diagnostics.items().end(), [](const auto& diagnostic) {
                              return diagnostic.code == "GDS4141";
                          }) >= std::ptrdiff_t{3});
}

TEST_CASE("IR verifier rejects mismatched iteration plans") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func visit(values: Array[int]) -> void:\n"
                               "    for value in values:\n"
                               "        pass\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());
    module.functions.front().body.front().iteration.strategy =
        gdpp::IterationStrategy::indexed_string;

    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(!verifier.verify(module));
    REQUIRE(diagnostics.has_errors());
}

TEST_CASE("IR verifier rejects missing and misplaced iteration plans") {
    gdpp::DiagnosticBag missing_diagnostics;
    auto missing = lower_source("func visit(values: Array[int]) -> void:\n"
                                "    for value in values:\n"
                                "        pass\n",
                                missing_diagnostics);
    REQUIRE(!missing_diagnostics.has_errors());
    missing.functions.front().body.front().iteration = {};
    gdpp::IrVerifier missing_verifier{missing_diagnostics};
    REQUIRE(!missing_verifier.verify(missing));

    gdpp::DiagnosticBag misplaced_diagnostics;
    auto misplaced =
        lower_source("func value() -> void:\n    var answer := 42\n", misplaced_diagnostics);
    REQUIRE(!misplaced_diagnostics.has_errors());
    misplaced.functions.front().body.front().iteration = {gdpp::IterationStrategy::integer_count,
                                                          {gdpp::TypeKind::integer, "int"}};
    gdpp::IrVerifier misplaced_verifier{misplaced_diagnostics};
    REQUIRE(!misplaced_verifier.verify(misplaced));
}

TEST_CASE("typed IR preserves dynamic method and property dispatch") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("func invoke(target: Variant) -> Variant:\n"
                                     "    return target.answer()\n"
                                     "func read(target: Variant) -> Variant:\n"
                                     "    return target.score\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.size(), std::size_t{2});
    const auto& call = *module.functions[0].body.front().expression;
    REQUIRE_EQ(call.kind, gdpp::ir::ExpressionKind::call);
    REQUIRE_EQ(call.operands.front()->resolution, gdpp::ir::ResolutionKind::dynamic_method);
    const auto& property = *module.functions[1].body.front().expression;
    REQUIRE_EQ(property.kind, gdpp::ir::ExpressionKind::member);
    REQUIRE_EQ(property.resolution, gdpp::ir::ResolutionKind::dynamic_property);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR normalizes RPC annotations before code generation") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("extends Node\n"
                                     "@rpc(\"reliable\", \"any_peer\", \"call_local\", 7)\n"
                                     "func synchronize() -> void:\n"
                                     "    pass\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.size(), std::size_t{1});
    REQUIRE(module.functions.front().rpc.has_value());
    REQUIRE_EQ(module.functions.front().rpc->permission, gdpp::RpcPermission::any_peer);
    REQUIRE_EQ(module.functions.front().rpc->transfer_mode, gdpp::RpcTransferMode::reliable);
    REQUIRE(module.functions.front().rpc->call_local);
    REQUIRE_EQ(module.functions.front().rpc->channel, std::int64_t{7});
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR distinguishes local constants from class constants") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("const ROOT = \"res://\"\n"
                                     "func path() -> String:\n"
                                     "    const FILE = ROOT + \"scene.tscn\"\n"
                                     "    return FILE\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.fields.front().initializer->kind, gdpp::ir::ExpressionKind::literal);
    const auto& local_initializer = *module.functions.front().body.front().expression;
    REQUIRE_EQ(local_initializer.operands.front()->resolution,
               gdpp::ir::ResolutionKind::script_constant);
    REQUIRE_EQ(module.functions.front().body.back().expression->resolution,
               gdpp::ir::ResolutionKind::local_constant);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves internal classes and owner-bound lambdas") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("class Payload:\n"
                                     "    var value: int\n"
                                     "    func _init(initial: int) -> void:\n"
                                     "        value = initial\n"
                                     "signal changed(value: int)\n"
                                     "func attach() -> void:\n"
                                     "    changed.connect(\n"
                                     "        func(value: int) -> void:\n"
                                     "            print(value))\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.classes.size(), std::size_t{1});
    REQUIRE_EQ(module.classes.front().name, std::string{"Payload"});
    const auto& call = *module.functions.front().body.front().expression;
    REQUIRE_EQ(call.operands.back()->kind, gdpp::ir::ExpressionKind::lambda);
    REQUIRE(call.operands.back()->lambda != nullptr);
    REQUIRE(call.operands.back()->lambda->owner_bound);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("IR optimizer folds constants and removes unreachable statements") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func answer() -> int:\n"
                               "    return 40 + 2\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());
    gdpp::ir::Statement unreachable;
    unreachable.kind = gdpp::ir::StatementKind::expression;
    unreachable.expression = std::make_unique<gdpp::ir::Expression>();
    unreachable.expression->kind = gdpp::ir::ExpressionKind::literal;
    unreachable.expression->literal_kind = gdpp::ir::LiteralKind::integer;
    unreachable.expression->type = {gdpp::TypeKind::integer, "int"};
    unreachable.expression->value = "7";
    module.functions.front().body.push_back(std::move(unreachable));

    const gdpp::IrOptimizer optimizer;
    const auto stats = optimizer.optimize(module);

    REQUIRE_EQ(stats.constants_folded, std::size_t{1});
    REQUIRE_EQ(stats.statements_removed, std::size_t{1});
    REQUIRE_EQ(module.functions.front().body.size(), std::size_t{1});
    const auto& value = *module.functions.front().body.front().expression;
    REQUIRE_EQ(value.kind, gdpp::ir::ExpressionKind::literal);
    REQUIRE_EQ(value.literal_kind, gdpp::ir::LiteralKind::integer);
    REQUIRE_EQ(value.value, std::string{"42"});
}

TEST_CASE("IR optimizer removes dead literal control-flow bodies before code generation") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func choose() -> int:\n"
                               "    if 20 + 22 == 42:\n"
                               "        return 7\n"
                               "    else:\n"
                               "        print(\"dead-branch\")\n"
                               "        return 9\n"
                               "func skip_loop() -> void:\n"
                               "    while false:\n"
                               "        print(\"dead-loop\")\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());

    const auto stats = gdpp::IrOptimizer{}.optimize(module);

    REQUIRE_EQ(stats.constants_folded, std::size_t{2});
    REQUIRE_EQ(stats.branches_simplified, std::size_t{2});
    REQUIRE_EQ(stats.statements_removed, std::size_t{4});
    REQUIRE(module.functions.front().body.front().else_body.empty());
    REQUIRE(module.functions.back().body.empty());
}

TEST_CASE("IR optimizer preserves exact int64 values above double precision") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func exact() -> int:\n"
                               "    return 9007199254740993 + 2\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());

    const auto stats = gdpp::IrOptimizer{}.optimize(module);

    REQUIRE_EQ(stats.constants_folded, std::size_t{1});
    const auto& value = *module.functions.front().body.front().expression;
    REQUIRE_EQ(value.literal_kind, gdpp::ir::LiteralKind::integer);
    REQUIRE_EQ(value.value, std::string{"9007199254740995"});
}

TEST_CASE("IR optimizer folds wrapped integer arithmetic without host undefined behavior") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func boundaries() -> Array[int]:\n"
                               "    return [9223372036854775807 + 1, -9223372036854775808 - 1, "
                               "9223372036854775807 * 2, -(-9223372036854775808), "
                               "-9223372036854775808 / -1, -9223372036854775808 % -1]\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());

    const auto stats = gdpp::IrOptimizer{}.optimize(module);

    REQUIRE_EQ(stats.constants_folded, std::size_t{12});
    const auto& values = module.functions.front().body.front().expression->operands;
    REQUIRE_EQ(values.at(0)->value, std::string{"-9223372036854775808"});
    REQUIRE_EQ(values.at(1)->value, std::string{"9223372036854775807"});
    REQUIRE_EQ(values.at(2)->value, std::string{"-2"});
    REQUIRE_EQ(values.at(3)->value, std::string{"-9223372036854775808"});
    REQUIRE_EQ(values.at(4)->value, std::string{"-9223372036854775808"});
    REQUIRE_EQ(values.at(5)->value, std::string{"0"});
}

TEST_CASE("IR optimizer folds normalized shifts and signed bit operations") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func bits() -> Array[int]:\n"
                               "    return [1 << 63, 1 << 64, 1 << -1, -9223372036854775808 >> 1, "
                               "9223372036854775807 >> 64, ~0, -1 & 0x55aa, 0x5500 | 0xaa, "
                               "0x55ff ^ 0x55]\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());

    const auto stats = gdpp::IrOptimizer{}.optimize(module);

    REQUIRE_EQ(stats.constants_folded, std::size_t{12});
    const auto& values = module.functions.front().body.front().expression->operands;
    REQUIRE_EQ(values.at(0)->value, std::string{"-9223372036854775808"});
    REQUIRE_EQ(values.at(1)->value, std::string{"1"});
    REQUIRE_EQ(values.at(2)->value, std::string{"-9223372036854775808"});
    REQUIRE_EQ(values.at(3)->value, std::string{"-4611686018427387904"});
    REQUIRE_EQ(values.at(4)->value, std::string{"9223372036854775807"});
    REQUIRE_EQ(values.at(5)->value, std::string{"-1"});
    REQUIRE_EQ(values.at(6)->value, std::string{"21930"});
    REQUIRE_EQ(values.at(7)->value, std::string{"21930"});
    REQUIRE_EQ(values.at(8)->value, std::string{"21930"});
}

TEST_CASE("IR optimizer preserves nonfinite and signed-zero floating contracts") {
    gdpp::DiagnosticBag diagnostics;
    auto module = lower_source("func invalid_difference() -> float:\n"
                               "    return 1e400 - 1e400\n"
                               "func signed_zero() -> float:\n"
                               "    return -0.0\n",
                               diagnostics);
    REQUIRE(!diagnostics.has_errors());

    const auto stats = gdpp::IrOptimizer{}.optimize(module);

    REQUIRE_EQ(stats.constants_folded, std::size_t{2});
    const auto& difference = *module.functions.at(0).body.front().expression;
    const auto& zero = *module.functions.at(1).body.front().expression;
    REQUIRE_EQ(difference.literal_kind, gdpp::ir::LiteralKind::floating);
    REQUIRE_EQ(difference.value, std::string{"nan"});
    REQUIRE_EQ(zero.literal_kind, gdpp::ir::LiteralKind::floating);
    REQUIRE_EQ(zero.value, std::string{"-0.0"});
}

TEST_CASE("IR verifier rejects structurally invalid collection IR") {
    gdpp::ir::Module module;
    gdpp::ir::Field field;
    field.name = "broken";
    field.type = {gdpp::TypeKind::dictionary, "Dictionary"};
    field.initializer = std::make_unique<gdpp::ir::Expression>();
    field.initializer->kind = gdpp::ir::ExpressionKind::dictionary_literal;
    field.initializer->type = field.type;
    field.initializer->operands.push_back(std::make_unique<gdpp::ir::Expression>());
    module.fields.push_back(std::move(field));
    gdpp::DiagnosticBag diagnostics;
    gdpp::IrVerifier verifier{diagnostics};

    REQUIRE(!verifier.verify(module));
    REQUIRE(diagnostics.has_errors());
}

TEST_CASE("typed IR preserves validated export property metadata") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source(
        "@export_range(-5.0, 20.0, 0.25, \"or_greater\") var speed: float = 2.0\n", diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(module.fields.front().property.has_value());
    REQUIRE_EQ(module.fields.front().property->name, std::string{"export_range"});
    REQUIRE_EQ(module.fields.front().property->arguments.size(), std::size_t{4});
    REQUIRE_EQ(module.fields.front().property->arguments.front().value, std::string{"-5.0"});
    REQUIRE_EQ(module.fields.front().property->arguments.back().kind,
               gdpp::ir::PropertyArgumentKind::string);
}

TEST_CASE("IR verifier rejects exported constants") {
    gdpp::ir::Module module;
    gdpp::ir::Field field;
    field.name = "invalid";
    field.type = {gdpp::TypeKind::integer, "int"};
    field.is_constant = true;
    field.property = gdpp::ir::PropertyAnnotation{"export", {}};
    field.initializer = std::make_unique<gdpp::ir::Expression>();
    field.initializer->kind = gdpp::ir::ExpressionKind::literal;
    field.initializer->literal_kind = gdpp::ir::LiteralKind::integer;
    field.initializer->type = field.type;
    field.initializer->value = "1";
    module.fields.push_back(std::move(field));
    gdpp::DiagnosticBag diagnostics;
    gdpp::IrVerifier verifier{diagnostics};

    REQUIRE(!verifier.verify(module));
    REQUIRE(diagnostics.has_errors());
}

TEST_CASE("typed IR owns evaluated enum values and enum export hints") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("enum State { IDLE, WALK = 4, RUN = WALK * 2 }\n"
                                     "@export var state: State = State.RUN\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.enums.size(), std::size_t{1});
    REQUIRE_EQ(module.enums.front().entries.at(0).value, std::int64_t{0});
    REQUIRE_EQ(module.enums.front().entries.at(1).value, std::int64_t{4});
    REQUIRE_EQ(module.enums.front().entries.at(2).value, std::int64_t{8});
    REQUIRE_EQ(module.fields.front().type.kind, gdpp::TypeKind::enumeration);
    REQUIRE_EQ(module.fields.front().enum_hint, std::string{"IDLE:0,WALK:4,RUN:8"});
}

TEST_CASE("typed IR preserves match patterns guards and branch scopes") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("func classify(value: int) -> String:\n"
                                     "    match value:\n"
                                     "        0, 1:\n"
                                     "            return \"small\"\n"
                                     "        var captured when captured > 7:\n"
                                     "            return \"large\"\n"
                                     "        _:\n"
                                     "            return \"other\"\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& match = module.functions.front().body.front();
    REQUIRE_EQ(match.kind, gdpp::ir::StatementKind::match_statement);
    REQUIRE_EQ(match.condition->type.kind, gdpp::TypeKind::integer);
    REQUIRE_EQ(match.body.at(1).patterns.front().kind, gdpp::ir::MatchPatternKind::binding);
    REQUIRE_EQ(match.body.at(1).expression->type.kind, gdpp::TypeKind::boolean);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR recursively owns structural match patterns and binding types") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("func classify(value: Variant) -> int:\n"
                                     "    match value:\n"
                                     "        [1, {\"hp\": var hp, ..}, var tail]:\n"
                                     "            return hp\n"
                                     "        _:\n"
                                     "            return -1\n"
                                     "func empty_match() -> void:\n"
                                     "    match 0:\n"
                                     "        pass\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& match = module.functions.front().body.front();
    const auto& root = match.body.front().patterns.front();
    REQUIRE_EQ(root.kind, gdpp::ir::MatchPatternKind::array);
    REQUIRE_EQ(root.elements.size(), std::size_t{3});
    REQUIRE_EQ(root.elements.at(1).kind, gdpp::ir::MatchPatternKind::dictionary);
    REQUIRE_EQ(root.elements.at(1).keys.size(), std::size_t{2});
    REQUIRE_EQ(root.elements.at(1).elements.front().kind, gdpp::ir::MatchPatternKind::binding);
    REQUIRE_EQ(root.elements.at(1).elements.front().type.kind, gdpp::TypeKind::variant);
    REQUIRE_EQ(root.elements.at(1).elements.back().kind, gdpp::ir::MatchPatternKind::rest);
    REQUIRE(module.functions.at(1).body.front().body.empty());
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves assert condition and optional message") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("func validate(value: int) -> int:\n"
                                     "    assert(value > 0, \"positive value required\")\n"
                                     "    return value\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& assertion = module.functions.front().body.front();
    REQUIRE_EQ(assertion.kind, gdpp::ir::StatementKind::assert_statement);
    REQUIRE_EQ(assertion.condition->type.kind, gdpp::TypeKind::boolean);
    REQUIRE_EQ(assertion.expression->type.kind, gdpp::TypeKind::string);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves debugger breakpoints as observable statements") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("func inspect(value: int) -> int:\n"
                                     "    breakpoint\n"
                                     "    return value\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& breakpoint = module.functions.front().body.front();
    REQUIRE_EQ(breakpoint.kind, gdpp::ir::StatementKind::breakpoint_statement);
    REQUIRE_EQ(breakpoint.span.begin.line, std::size_t{2});
    REQUIRE(!breakpoint.expression);
    REQUIRE(!breakpoint.condition);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR isolates debug-only awaited assert operands") {
    gdpp::DiagnosticBag diagnostics;
    const auto module =
        lower_source("signal condition_ready\n"
                     "signal message_ready\n"
                     "func validate() -> void:\n"
                     "    assert(await condition_ready, str(await message_ready))\n",
                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& assertion = module.functions.front().body.front();
    REQUIRE_EQ(assertion.kind, gdpp::ir::StatementKind::assert_statement);
    REQUIRE(!assertion.assert_condition_prefix.empty());
    REQUIRE(!assertion.assert_message_prefix.empty());
    REQUIRE(std::none_of(
        assertion.assert_condition_prefix.begin(), assertion.assert_condition_prefix.end(),
        [](const auto& statement) { return contains_await_expression(statement); }));
    REQUIRE(
        std::none_of(assertion.assert_message_prefix.begin(), assertion.assert_message_prefix.end(),
                     [](const auto& statement) { return contains_await_expression(statement); }));
    REQUIRE(!contains_await_expression(*assertion.condition));
    REQUIRE(!contains_await_expression(*assertion.expression));
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves signal await suspension points") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("signal resumed\n"
                                     "func run() -> void:\n"
                                     "    await resumed\n"
                                     "    pass\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.front().body.front().kind,
               gdpp::ir::StatementKind::await_statement);
    REQUIRE_EQ(module.functions.front().body.front().expression->type.kind,
               gdpp::TypeKind::builtin);
    REQUIRE_EQ(module.functions.front().body.front().expression->type.name, std::string{"Signal"});
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves coroutine ABI and call-site suspension metadata") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("signal resumed\n"
                                     "func produce() -> int:\n"
                                     "    await resumed\n"
                                     "    return 42\n"
                                     "func consume() -> int:\n"
                                     "    return await produce()\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.size(), std::size_t{2});
    REQUIRE(module.functions.at(0).is_coroutine);
    REQUIRE(module.functions.at(1).is_coroutine);
    const auto& await_result = module.functions.at(1).body.front();
    REQUIRE_EQ(await_result.kind, gdpp::ir::StatementKind::await_variable);
    REQUIRE(await_result.expression != nullptr);
    REQUIRE_EQ(await_result.expression->kind, gdpp::ir::ExpressionKind::call);
    REQUIRE(await_result.expression->coroutine_call);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves a local initialized from an awaited signal") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("signal selected(value: String)\n"
                                     "func run():\n"
                                     "    var value = await selected\n"
                                     "    print(value)\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.functions.front().body.front().kind, gdpp::ir::StatementKind::await_variable);
    REQUIRE_EQ(module.functions.front().body.front().name, std::string{"value"});
    REQUIRE_EQ(module.functions.front().body.front().declared_type.kind, gdpp::TypeKind::variant);
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR A-normalizes await expressions without changing evaluation order") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("signal selected(value: int)\n"
                                     "func side(value: int) -> int:\n"
                                     "    return value\n"
                                     "func run() -> void:\n"
                                     "    var total: int = side(10) + await selected\n"
                                     "    print(side(20), await selected, side(30), total)\n"
                                     "    if await selected:\n"
                                     "        print(total)\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& body = module.functions.at(1).body;
    REQUIRE_EQ(body.at(0).kind, gdpp::ir::StatementKind::variable);
    REQUIRE(body.at(0).name.find("@gdpp-await-value-") == 0);
    REQUIRE_EQ(body.at(1).kind, gdpp::ir::StatementKind::await_variable);
    REQUIRE_EQ(body.at(2).kind, gdpp::ir::StatementKind::variable);
    REQUIRE_EQ(body.at(3).kind, gdpp::ir::StatementKind::variable);
    REQUIRE_EQ(body.at(4).kind, gdpp::ir::StatementKind::await_variable);
    REQUIRE_EQ(body.at(5).kind, gdpp::ir::StatementKind::expression);
    REQUIRE_EQ(body.at(6).kind, gdpp::ir::StatementKind::await_variable);
    REQUIRE_EQ(body.at(7).kind, gdpp::ir::StatementKind::if_statement);
    REQUIRE(std::none_of(body.begin(), body.end(), [](const auto& statement) {
        return contains_await_expression(statement);
    }));
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR preserves lazy await branches and reevaluated loop conditions") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("signal selected(value)\n"
                                     "func run() -> void:\n"
                                     "    var logical = false and await selected\n"
                                     "    var choice = (await selected) if await selected else "
                                     "await selected\n"
                                     "    while await selected:\n"
                                     "        if await selected:\n"
                                     "            continue\n"
                                     "        break\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& body = module.functions.front().body;
    REQUIRE(std::any_of(body.begin(), body.end(), [](const auto& statement) {
        return statement.kind == gdpp::ir::StatementKind::if_statement;
    }));
    const auto loop = std::find_if(body.begin(), body.end(), [](const auto& statement) {
        return statement.kind == gdpp::ir::StatementKind::while_statement;
    });
    REQUIRE(loop != body.end());
    REQUIRE(!loop->body.empty());
    REQUIRE(std::none_of(body.begin(), body.end(), [](const auto& statement) {
        return contains_await_expression(statement);
    }));
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR isolates awaited match guards after pattern binding") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("signal selected\n"
                                     "func run(value: int) -> void:\n"
                                     "    match value:\n"
                                     "        var captured when captured > 0 and await selected:\n"
                                     "            await selected\n"
                                     "        _:\n"
                                     "            pass\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    const auto& match = module.functions.front().body.front();
    REQUIRE_EQ(match.kind, gdpp::ir::StatementKind::match_statement);
    REQUIRE_EQ(match.body.front().kind, gdpp::ir::StatementKind::match_branch);
    REQUIRE(!match.body.front().guard_prefix.empty());
    REQUIRE(match.body.front().expression != nullptr);
    REQUIRE(
        std::none_of(match.body.front().guard_prefix.begin(), match.body.front().guard_prefix.end(),
                     [](const auto& statement) { return contains_await_expression(statement); }));
    REQUIRE(std::any_of(match.body.front().guard_prefix.begin(),
                        match.body.front().guard_prefix.end(), [](const auto& statement) {
                            return statement.kind == gdpp::ir::StatementKind::if_statement ||
                                   statement.kind == gdpp::ir::StatementKind::await_variable;
                        }));
    gdpp::IrVerifier verifier{diagnostics};
    REQUIRE(verifier.verify(module));
}

TEST_CASE("typed IR distinguishes property accessors from their backing field") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("var health: int = 100:\n"
                                     "    set(value):\n"
                                     "        health = value\n"
                                     "    get:\n"
                                     "        return health\n"
                                     "func update(value: int) -> int:\n"
                                     "    health = value\n"
                                     "    return health\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(module.fields.front().getter.has_value());
    REQUIRE(module.fields.front().setter.has_value());
    const auto& setter_target = *module.fields.front().setter->body.front().condition;
    REQUIRE_EQ(setter_target.resolution, gdpp::ir::ResolutionKind::none);
    const auto& function_target = *module.functions.front().body.front().condition;
    REQUIRE_EQ(function_target.resolution, gdpp::ir::ResolutionKind::script_property);
    const auto& function_read = *module.functions.front().body.back().expression;
    REQUIRE_EQ(function_read.resolution, gdpp::ir::ResolutionKind::script_property);
}

TEST_CASE("typed IR preserves bound accessors and direct backing access in their methods") {
    gdpp::DiagnosticBag diagnostics;
    const auto module = lower_source("var active: bool = true: set = set_active, get = is_active\n"
                                     "func set_active(value: bool) -> void:\n"
                                     "    active = value\n"
                                     "func is_active() -> bool:\n"
                                     "    return active\n"
                                     "func disable() -> void:\n"
                                     "    active = false\n",
                                     diagnostics);

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(module.fields.front().setter->method, std::string{"set_active"});
    REQUIRE_EQ(module.fields.front().getter->method, std::string{"is_active"});
    REQUIRE_EQ(module.functions.at(0).body.front().condition->resolution,
               gdpp::ir::ResolutionKind::none);
    REQUIRE_EQ(module.functions.at(1).body.front().expression->resolution,
               gdpp::ir::ResolutionKind::none);
    REQUIRE_EQ(module.functions.at(2).body.front().condition->resolution,
               gdpp::ir::ResolutionKind::script_property);
}
