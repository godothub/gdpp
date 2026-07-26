#include "support/test.hpp"

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/parser.hpp"

#include <algorithm>
#include <string>

TEST_CASE("parser builds declarations and structured function body") {
    const gdpp::SourceFile source{"player.gd", "extends Node\n"
                                               "class_name Player\n"
                                               "signal damaged(amount: int)\n"
                                               "var health: int = 100\n"
                                               "func take_damage(amount: int) -> void:\n"
                                               "    health -= amount\n"
                                               "    if health < 0:\n"
                                               "        health = 0\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(*script.base_type, std::string{"Node"});
    REQUIRE_EQ(*script.class_name, std::string{"Player"});
    REQUIRE_EQ(script.signals.size(), std::size_t{1});
    REQUIRE_EQ(script.variables.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.front().body.size(), std::size_t{2});
    REQUIRE_EQ(script.functions.front().body.back().kind(), gdpp::ast::StatementKind::if_statement);
}

TEST_CASE("parser accepts latest script annotations directives and single-line suites") {
    const gdpp::SourceFile source{
        "latest.gd", "@tool\n"
                     "@icon(\"res://icon.svg\")\n"
                     "@abstract\n"
                     "class_name LatestScript extends Node\n"
                     "@warning_ignore_start(\"unused_signal\")\n"
                     "signal changed()\n"
                     "@warning_ignore_restore(\"unused_signal\")\n"
                     "func classify(value: int) -> String: return \"yes\" if value else \"no\"\n"
                     "func reset() -> void: pass; return\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(script.tool);
    REQUIRE_EQ(script.annotations.size(), std::size_t{3});
    REQUIRE_EQ(*script.class_name, std::string{"LatestScript"});
    REQUIRE_EQ(*script.base_type, std::string{"Node"});
    REQUIRE_EQ(script.signals.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.front().body.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.back().body.size(), std::size_t{2});
}

TEST_CASE("parser preserves bodyless abstract function signatures") {
    const gdpp::SourceFile source{"abstract.gd", "@abstract\n"
                                                 "class_name AbstractWorker\n"
                                                 "@abstract\n"
                                                 "func execute(value: int) -> String\n"
                                                 "func concrete() -> int:\n"
                                                 "    return 42\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.size(), std::size_t{2});
    REQUIRE(script.functions.front().is_abstract);
    REQUIRE(!script.functions.front().has_body);
    REQUIRE(script.functions.front().body.empty());
    REQUIRE(!script.functions.back().is_abstract);
    REQUIRE(script.functions.back().has_body);
    REQUIRE_EQ(script.functions.back().body.size(), std::size_t{1});
}

TEST_CASE("parser leaves bodyless function validity to semantic analysis") {
    const gdpp::SourceFile source{"bodyless.gd", "func missing_body() -> void\n"
                                                 "func recovered() -> void:\n"
                                                 "    pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.size(), std::size_t{2});
    REQUIRE(!script.functions.front().has_body);
    REQUIRE(script.functions.back().has_body);
}

TEST_CASE("parser disambiguates inline abstract class annotations from the script") {
    const gdpp::SourceFile source{"inline_abstract.gd",
                                  "@abstract class Contract:\n"
                                  "    @abstract func execute()\n"
                                  "class EmptyImplementation extends Contract:\n"
                                  "    pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(script.annotations.empty());
    REQUIRE_EQ(script.classes.size(), std::size_t{2});
    REQUIRE_EQ(script.classes.front().annotations.size(), std::size_t{1});
    REQUIRE_EQ(script.classes.front().annotations.front().name, std::string{"abstract"});
    REQUIRE(script.classes.front().functions.front().is_abstract);
    REQUIRE(!script.classes.front().functions.front().has_body);
    REQUIRE(script.classes.back().functions.empty());
}

TEST_CASE("parser preserves the bounded Godot RPC annotation contract") {
    const gdpp::SourceFile valid{"rpc.gd", "extends Node\n"
                                           "@rpc(\"any_peer\", \"call_local\", \"reliable\", 3)\n"
                                           "func synchronize() -> void:\n"
                                           "    pass\n"};
    gdpp::DiagnosticBag valid_diagnostics;
    const auto valid_tokens = gdpp::Lexer{valid, valid_diagnostics}.scan();
    const auto script = gdpp::Parser{valid_tokens, valid_diagnostics}.parse_script();

    REQUIRE(!valid_diagnostics.has_errors());
    REQUIRE_EQ(script.functions.front().annotations.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.front().annotations.front().arguments.size(), std::size_t{4});

    const gdpp::SourceFile excessive{"invalid_rpc.gd",
                                     "@rpc(\"authority\", \"call_remote\", \"unreliable\", 0, 1)\n"
                                     "func synchronize() -> void:\n"
                                     "    pass\n"};
    gdpp::DiagnosticBag excessive_diagnostics;
    const auto excessive_tokens = gdpp::Lexer{excessive, excessive_diagnostics}.scan();
    (void)gdpp::Parser{excessive_tokens, excessive_diagnostics}.parse_script();

    REQUIRE(std::any_of(excessive_diagnostics.items().begin(), excessive_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS2020"; }));
}

TEST_CASE("parser rejects misplaced duplicate script annotations and warning ranges") {
    const gdpp::SourceFile source{"invalid_directives.gd",
                                  "@tool\n"
                                  "@tool\n"
                                  "class_name Invalid\n"
                                  "@icon(\"res://late.svg\")\n"
                                  "@warning_ignore_start(\"unused_variable\")\n"
                                  "@warning_ignore_start(\"unused_variable\")\n"
                                  "@warning_ignore_restore(\"not_active\")\n"
                                  "func test() -> void:\n"
                                  "    pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    (void)gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(diagnostics.has_errors());
    REQUIRE(std::count_if(diagnostics.items().begin(), diagnostics.items().end(),
                          [](const auto& diagnostic) { return diagnostic.code == "GDS2028"; }) ==
            std::ptrdiff_t{1});
    REQUIRE(std::count_if(diagnostics.items().begin(), diagnostics.items().end(),
                          [](const auto& diagnostic) { return diagnostic.code == "GDS2029"; }) ==
            std::ptrdiff_t{2});
    REQUIRE(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS2027"; }));
}

TEST_CASE("parser builds internal classes and multiline lambda expressions") {
    const gdpp::SourceFile source{"modern.gd", "class Payload:\n"
                                               "    var value: int\n"
                                               "    func _init(initial: int) -> void:\n"
                                               "        value = initial\n"
                                               "func attach(changed: Signal) -> void:\n"
                                               "    changed.connect(\n"
                                               "        func(next: int) -> void:\n"
                                               "            print(next))\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.classes.size(), std::size_t{1});
    REQUIRE_EQ(script.classes.front().name, std::string{"Payload"});
    REQUIRE_EQ(script.classes.front().variables.size(), std::size_t{1});
    REQUIRE_EQ(script.classes.front().functions.size(), std::size_t{1});
    const auto& call = *script.functions.front().body.front().expression();
    REQUIRE_EQ(call.kind(), gdpp::ast::ExpressionKind::call);
    REQUIRE_EQ(call.operand_count(), std::size_t{2});
    REQUIRE_EQ(call.operand(1)->kind(), gdpp::ast::ExpressionKind::lambda);
    REQUIRE(call.operand(1)->lambda() != nullptr);
    REQUIRE_EQ(call.operand(1)->lambda()->parameters.front().name, std::string{"next"});
    REQUIRE_EQ(call.operand(1)->lambda()->body.size(), std::size_t{1});
}

TEST_CASE("parser builds assert statements with optional messages") {
    const gdpp::SourceFile source{"assert.gd", "func validate(value: int) -> int:\n"
                                               "    assert(value > 0)\n"
                                               "    assert(value < 100, \"value is too large\")\n"
                                               "    return value\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.front().body.at(0).kind(),
               gdpp::ast::StatementKind::assert_statement);
    REQUIRE(script.functions.front().body.at(0).condition() != nullptr);
    REQUIRE(script.functions.front().body.at(0).expression() == nullptr);
    REQUIRE_EQ(script.functions.front().body.at(1).kind(),
               gdpp::ast::StatementKind::assert_statement);
    REQUIRE_EQ(script.functions.front().body.at(1).expression()->literal_kind(),
               gdpp::ast::LiteralKind::string);
}

TEST_CASE("parser preserves standalone breakpoint statements in every suite form") {
    const gdpp::SourceFile source{"breakpoint.gd", "func inspect(value: int) -> int:\n"
                                                   "    breakpoint\n"
                                                   "    if value > 0: breakpoint; value += 1\n"
                                                   "    return value\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.front().body.front().kind(),
               gdpp::ast::StatementKind::breakpoint_statement);
    const auto& conditional = script.functions.front().body.at(1);
    REQUIRE_EQ(conditional.body().front().kind(), gdpp::ast::StatementKind::breakpoint_statement);
    REQUIRE_EQ(conditional.body().at(1).kind(), gdpp::ast::StatementKind::assignment);
}

TEST_CASE("parser builds await as a precedence-aware expression") {
    const gdpp::SourceFile source{"await.gd", "func wait_for(timer: Timer) -> void:\n"
                                              "    await timer.timeout\n"
                                              "    var value = 1 + await timer.timeout\n"
                                              "    print(await timer.timeout)\n"
                                              "    pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.front().body.front().kind(), gdpp::ast::StatementKind::expression);
    REQUIRE_EQ(script.functions.front().body.front().expression()->kind(),
               gdpp::ast::ExpressionKind::await_expression);
    REQUIRE_EQ(script.functions.front().body.front().expression()->operand(0)->kind(),
               gdpp::ast::ExpressionKind::member);
    const auto& initializer = *script.functions.front().body.at(1).expression();
    REQUIRE_EQ(initializer.kind(), gdpp::ast::ExpressionKind::binary);
    REQUIRE_EQ(initializer.operand(1)->kind(), gdpp::ast::ExpressionKind::await_expression);
    const auto& call = *script.functions.front().body.at(2).expression();
    REQUIRE_EQ(call.kind(), gdpp::ast::ExpressionKind::call);
    REQUIRE_EQ(call.operand(1)->kind(), gdpp::ast::ExpressionKind::await_expression);
}

TEST_CASE("parser preserves collection literals inferred types for loops and elif") {
    const gdpp::SourceFile source{"collections.gd", "func visit() -> void:\n"
                                                    "    var values := [1, 2, 3]\n"
                                                    "    var labels := {\"one\": 1}\n"
                                                    "    for value in values:\n"
                                                    "        if value == 1:\n"
                                                    "            print(labels[\"one\"])\n"
                                                    "        elif value == 2:\n"
                                                    "            pass\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.size(), std::size_t{1});
    REQUIRE(script.functions.front().body.front().infer_type());
    REQUIRE_EQ(script.functions.front().body.front().expression()->kind(),
               gdpp::ast::ExpressionKind::array_literal);
    REQUIRE_EQ(script.functions.front().body.at(1).expression()->kind(),
               gdpp::ast::ExpressionKind::dictionary_literal);
    REQUIRE_EQ(script.functions.front().body.at(2).kind(), gdpp::ast::StatementKind::for_statement);
    REQUIRE_EQ(script.functions.front().body.at(2).body().front().else_body().front().kind(),
               gdpp::ast::StatementKind::if_statement);
}

TEST_CASE("parser accepts current Godot local constants separators and Lua dictionaries") {
    const gdpp::SourceFile source{"official_surface.gd",
                                  "func exercise() -> void:\n"
                                  "    const LIMIT : = 123_\n"
                                  "    var match : = {score = LIMIT, \"label\" = \"ok\",}\n"
                                  "    var when := [match.score,]\n"
                                  "    assert(when[0] == LIMIT,)\n"
                                  "    match.return = LIMIT\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.front().body.size(), std::size_t{5});
    REQUIRE(script.functions.front().body.front().is_constant());
    REQUIRE(script.functions.front().body.front().infer_type());
    const auto& dictionary = *script.functions.front().body.at(1).expression();
    REQUIRE_EQ(dictionary.kind(), gdpp::ast::ExpressionKind::dictionary_literal);
    const auto& entries = dictionary.get_if<gdpp::ast::DictionaryExpression>()->entries;
    REQUIRE_EQ(entries.size(), std::size_t{2});
    REQUIRE_EQ(entries.front().key->literal_kind(), gdpp::ast::LiteralKind::string);
    REQUIRE_EQ(entries.front().key->value(), std::string{"score"});
    REQUIRE_EQ(script.functions.front().body.at(3).kind(),
               gdpp::ast::StatementKind::assert_statement);
    REQUIRE_EQ(script.functions.front().body.back().condition()->value(), std::string{"return"});
}

TEST_CASE("parser rejects non-Godot multiple variable declarations without cascading") {
    const gdpp::SourceFile source{"multiple_variables.gd", "var first = 1, second = 2\n"
                                                           "func test() -> int:\n"
                                                           "    var local = 3, other = 4\n"
                                                           "    var recovered := 5\n"
                                                           "    return recovered\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(diagnostics.has_errors());
    REQUIRE_EQ(std::count_if(diagnostics.items().begin(), diagnostics.items().end(),
                             [](const auto& diagnostic) { return diagnostic.code == "GDS2032"; }),
               std::ptrdiff_t{2});
    REQUIRE_EQ(script.variables.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.front().body.size(), std::size_t{3});
    REQUIRE_EQ(script.functions.front().body.at(1).name(), std::string{"recovered"});
    REQUIRE_EQ(script.functions.front().body.back().kind(),
               gdpp::ast::StatementKind::return_statement);
}

TEST_CASE("parser attaches export annotations to fields across line layouts") {
    const gdpp::SourceFile source{"exports.gd", "@export var title: String = \"Player\"\n"
                                                "@export_range(-10.0, 10.0, 0.5, \"or_greater\")\n"
                                                "var speed: float = 1.0\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.variables.size(), std::size_t{2});
    REQUIRE_EQ(script.variables.front().annotations.front().name, std::string{"export"});
    REQUIRE_EQ(script.variables.back().annotations.front().name, std::string{"export_range"});
    REQUIRE_EQ(script.variables.back().annotations.front().arguments.size(), std::size_t{4});
    REQUIRE_EQ(script.variables.back().annotations.front().arguments.front()->kind(),
               gdpp::ast::ExpressionKind::unary);
}

TEST_CASE("parser preserves onready fields and typed iterators") {
    const gdpp::SourceFile source{"modern_fields.gd",
                                  "@onready var label: Label = $Label\n"
                                  "func collect(values: Dictionary[String, int]) -> void:\n"
                                  "    for key: String in values:\n"
                                  "        pass\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(script.variables.front().onready);
    REQUIRE_EQ(*script.functions.front().parameters.front().type,
               std::string{"Dictionary[String, int]"});
    REQUIRE_EQ(*script.functions.front().body.front().type(), std::string{"String"});
    const auto* loop = script.functions.front().body.front().get_if<gdpp::ast::ForStatement>();
    REQUIRE(loop != nullptr);
    REQUIRE_EQ(loop->iterator_span.begin.line, std::size_t{3});
    REQUIRE_EQ(loop->iterator_span.begin.column, std::size_t{9});
    REQUIRE(loop->type_span.has_value());
    REQUIRE_EQ(loop->type_span->begin.column, std::size_t{14});
    REQUIRE_EQ(loop->type_span->end.column, std::size_t{20});
}

TEST_CASE("parser distinguishes static fields from static functions") {
    const gdpp::SourceFile source{"static_members.gd",
                                  "static var cache: Dictionary[String, int] = {}\n"
                                  "static func clear() -> void:\n"
                                  "    cache.clear()\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(script.variables.front().is_static);
    REQUIRE(script.functions.front().is_static);
}

TEST_CASE("parser validates warning annotations without changing statements") {
    const gdpp::SourceFile source{"warnings.gd", "func divide(value: int) -> int:\n"
                                                 "    @warning_ignore(\"integer_division\")\n"
                                                 "    var result := value / 2\n"
                                                 "    return result\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.front().body.size(), std::size_t{2});
    REQUIRE_EQ(script.functions.front().body.front().kind(), gdpp::ast::StatementKind::variable);
}

TEST_CASE("parser validates the complete warning directive namespace") {
    const gdpp::SourceFile valid{"warning_names.gd",
                                 "func inspect() -> void:\n"
                                 "    @warning_ignore(\"unassigned_variable\", "
                                 "\"function_used_as_property\")\n"
                                 "    pass\n"
                                 "    @warning_ignore_start(\"redundant_await\")\n"
                                 "    await 42\n"
                                 "    @warning_ignore_restore(\"redundant_await\")\n"};
    gdpp::DiagnosticBag valid_diagnostics;
    const auto valid_tokens = gdpp::Lexer{valid, valid_diagnostics}.scan();
    const auto valid_script = gdpp::Parser{valid_tokens, valid_diagnostics}.parse_script();

    REQUIRE(!valid_diagnostics.has_errors());
    REQUIRE_EQ(valid_script.functions.front().body.size(), std::size_t{3});

    const gdpp::SourceFile invalid{"invalid_warning_names.gd",
                                   "@warning_ignore(\"not_a_godot_warning\")\n"
                                   "func invalid() -> void:\n"
                                   "    pass\n"
                                   "func non_literal() -> void:\n"
                                   "    @warning_ignore(unreachable_code)\n"
                                   "    pass\n"};
    gdpp::DiagnosticBag invalid_diagnostics;
    const auto invalid_tokens = gdpp::Lexer{invalid, invalid_diagnostics}.scan();
    (void)gdpp::Parser{invalid_tokens, invalid_diagnostics}.parse_script();

    REQUIRE(std::any_of(invalid_diagnostics.items().begin(), invalid_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS2034"; }));
    REQUIRE(std::any_of(invalid_diagnostics.items().begin(), invalid_diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS2016"; }));
}

TEST_CASE("parser builds named and anonymous enum declarations") {
    const gdpp::SourceFile source{"enums.gd",
                                  "enum State { IDLE, WALK = 4, RUN = WALK * 2, }\n"
                                  "enum { DEFAULT_LIVES = 3, MAX_LIVES = DEFAULT_LIVES + 2 }\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.enums.size(), std::size_t{2});
    REQUIRE_EQ(*script.enums.front().name, std::string{"State"});
    REQUIRE_EQ(script.enums.front().entries.size(), std::size_t{3});
    REQUIRE_EQ(script.enums.front().entries.back().value->kind(),
               gdpp::ast::ExpressionKind::binary);
    REQUIRE(!script.enums.back().name.has_value());
}

TEST_CASE("parser preserves qualified and nested type names in every annotation position") {
    const gdpp::SourceFile source{"qualified-types.gd",
                                  "var mode: Shared.State\n"
                                  "func roundtrip(values: Array[Shared.State]) -> Shared.State:\n"
                                  "    return Shared.State.IDLE\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(*script.variables.front().type, std::string{"Shared.State"});
    REQUIRE_EQ(*script.functions.front().parameters.front().type,
               std::string{"Array[Shared.State]"});
    REQUIRE_EQ(*script.functions.front().return_type, std::string{"Shared.State"});
}

TEST_CASE("parser builds match alternatives bindings guards and wildcard branches") {
    const gdpp::SourceFile source{"match.gd", "func classify(value: int) -> String:\n"
                                              "    match value:\n"
                                              "        0, 1:\n"
                                              "            return \"small\"\n"
                                              "        var captured when captured > 7:\n"
                                              "            return \"large\"\n"
                                              "        _:\n"
                                              "            return \"other\"\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    const auto& match = script.functions.front().body.front();
    REQUIRE_EQ(match.kind(), gdpp::ast::StatementKind::match_statement);
    REQUIRE_EQ(match.match_branches().size(), std::size_t{3});
    REQUIRE_EQ(match.match_branches().front().patterns.size(), std::size_t{2});
    REQUIRE_EQ(match.match_branches().at(1).patterns.front().kind(),
               gdpp::ast::MatchPatternKind::binding);
    REQUIRE(match.match_branches().at(1).guard != nullptr);
    REQUIRE_EQ(match.match_branches().back().patterns.front().kind(),
               gdpp::ast::MatchPatternKind::wildcard);
}

TEST_CASE("parser preserves recursive array dictionary rest and binding match patterns") {
    const gdpp::SourceFile source{
        "structured_match.gd", "func classify(value):\n"
                               "    match [1, value, 3]:\n"
                               "        [1, {\"hp\": var hp, ..}, [var first, ..]] when hp > 0:\n"
                               "            return first\n"
                               "    match 0:\n"
                               "        pass\n"
                               "func callback():\n"
                               "    var operation := func named_operation(input):\n"
                               "        return input\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    const auto& statement = script.functions.front().body.front();
    REQUIRE_EQ(statement.condition()->kind(), gdpp::ast::ExpressionKind::array_literal);
    const auto& root = statement.match_branches().front().patterns.front();
    REQUIRE_EQ(root.kind(), gdpp::ast::MatchPatternKind::array);
    REQUIRE_EQ(root.elements.size(), std::size_t{3});
    REQUIRE_EQ(root.elements.at(1)->kind(), gdpp::ast::MatchPatternKind::dictionary);
    REQUIRE_EQ(root.elements.at(1)->keys.size(), std::size_t{2});
    REQUIRE_EQ(root.elements.at(1)->elements.front()->kind(), gdpp::ast::MatchPatternKind::binding);
    REQUIRE_EQ(root.elements.at(1)->elements.back()->kind(), gdpp::ast::MatchPatternKind::rest);
    REQUIRE_EQ(root.elements.at(2)->elements.back()->kind(), gdpp::ast::MatchPatternKind::rest);
    REQUIRE(script.functions.front().body.at(1).match_branches().empty());
    REQUIRE_EQ(script.functions.back().body.front().expression()->lambda()->name,
               std::string{"named_operation"});
}

TEST_CASE("parser rejects standalone lambdas while preserving assigned named lambdas") {
    const gdpp::SourceFile source{"standalone_lambda.gd",
                                  "func invalid():\n"
                                  "    func inaccessible(): pass\n"
                                  "func valid():\n"
                                  "    var callable := func accessible(): pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(diagnostics.has_errors());
    REQUIRE_EQ(script.functions.size(), std::size_t{2});
    REQUIRE_EQ(script.functions.back().body.front().expression()->lambda()->name,
               std::string{"accessible"});
}

TEST_CASE("parser attaches Godot 4 property getter and setter blocks") {
    const gdpp::SourceFile source{"property.gd", "@export var health: int = 100:\n"
                                                 "    set(value):\n"
                                                 "        health = value\n"
                                                 "    get:\n"
                                                 "        return health\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(script.variables.front().getter.has_value());
    REQUIRE(script.variables.front().setter.has_value());
    REQUIRE_EQ(script.variables.front().setter->parameter, std::string{"value"});
    REQUIRE_EQ(script.variables.front().getter->body.front().kind(),
               gdpp::ast::StatementKind::return_statement);
    REQUIRE_EQ(script.variables.front().setter->body.front().kind(),
               gdpp::ast::StatementKind::assignment);
}

TEST_CASE("parser preserves bound property accessor methods in both layouts") {
    const gdpp::SourceFile source{"bound_property.gd",
                                  "var label: get = read_label, set = write_label\n"
                                  "var count: int = 1:\n"
                                  "    set = write_count,\n"
                                  "    get = read_count\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.variables.size(), std::size_t{2});
    REQUIRE_EQ(script.variables.front().getter->method, std::string{"read_label"});
    REQUIRE_EQ(script.variables.front().setter->method, std::string{"write_label"});
    REQUIRE(script.variables.front().getter->body.empty());
    REQUIRE_EQ(script.variables.back().getter->method, std::string{"read_count"});
    REQUIRE_EQ(script.variables.back().setter->method, std::string{"write_count"});
}

TEST_CASE("parser builds casts conditional expressions power and node references") {
    const gdpp::SourceFile source{"modern.gd",
                                  "func choose(enabled: bool) -> Node3D:\n"
                                  "    var node := $Root/Child as Node3D\n"
                                  "    var typed := [1] as Array[int]\n"
                                  "    var chart := 1 as ChartData.Chart\n"
                                  "    var power := 2 ** 3 ** 2\n"
                                  "    return node if enabled else %Fallback as Node3D\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    const auto& node = *script.functions.front().body.at(0).expression();
    REQUIRE_EQ(node.kind(), gdpp::ast::ExpressionKind::binary);
    REQUIRE_EQ(node.value(), std::string{"as"});
    REQUIRE_EQ(node.operand(0)->kind(), gdpp::ast::ExpressionKind::node_reference);
    const auto& typed = *script.functions.front().body.at(1).expression();
    REQUIRE_EQ(typed.operand(1)->value(), std::string{"Array[int]"});
    const auto& chart = *script.functions.front().body.at(2).expression();
    REQUIRE_EQ(chart.operand(1)->value(), std::string{"ChartData.Chart"});
    const auto& power = *script.functions.front().body.at(3).expression();
    REQUIRE_EQ(power.value(), std::string{"**"});
    REQUIRE_EQ(power.operand(1)->value(), std::string{"**"});
    REQUIRE_EQ(script.functions.front().body.at(4).expression()->kind(),
               gdpp::ast::ExpressionKind::conditional);
}

TEST_CASE("parser applies GDScript not and is-not precedence") {
    const gdpp::SourceFile source{
        "not_precedence.gd", "func check(node: Node) -> bool:\n"
                             "    return not node.name == \"Player\" and node is not Node2D\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    const auto& conjunction = *script.functions.front().body.front().expression();
    REQUIRE_EQ(conjunction.value(), std::string{"and"});
    REQUIRE_EQ(conjunction.operand(0)->kind(), gdpp::ast::ExpressionKind::unary);
    REQUIRE_EQ(conjunction.operand(0)->operand(0)->value(), std::string{"=="});
    REQUIRE_EQ(conjunction.operand(1)->value(), std::string{"is not"});
}

TEST_CASE("parser preserves not-in membership and compact modulo") {
    const gdpp::SourceFile source{"membership.gd",
                                  "func check(value: Variant, items: Array) -> bool:\n"
                                  "    var index := randi()%items.size()\n"
                                  "    return value not in items and index >= 0\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.front().body.front().expression()->value(), std::string{"%"});
    const auto& conjunction = *script.functions.front().body.back().expression();
    REQUIRE_EQ(conjunction.value(), std::string{"and"});
    REQUIRE_EQ(conjunction.operand(0)->value(), std::string{"not in"});
}

TEST_CASE("parser recovery always advances past unexpected indentation") {
    const gdpp::SourceFile source{"recovery.gd", "var first := 1\n"
                                                 "    + 2\n"
                                                 "var second := 3\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(diagnostics.has_errors());
    REQUIRE_EQ(script.variables.size(), std::size_t{2});
    REQUIRE_EQ(script.variables.back().name, std::string{"second"});
}

TEST_CASE("parser balances large logical chains without changing source order") {
    std::string text{"func validate() -> bool:\n    return true"};
    constexpr std::size_t operand_count = 1024;
    for (std::size_t index = 1; index < operand_count; ++index)
        text += " and true";
    text += '\n';

    const gdpp::SourceFile source{"large_logical_chain.gd", std::move(text)};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    const auto* expression = script.functions.front().body.front().expression().get();
    std::size_t left_depth = 0;
    while (expression->kind() == gdpp::ast::ExpressionKind::binary &&
           expression->value() == "and") {
        ++left_depth;
        expression = expression->operand(0).get();
    }
    REQUIRE(left_depth <= std::size_t{10});
}

TEST_CASE("parser accepts commercial script headers semicolons and inline lambdas") {
    const gdpp::SourceFile source{"commercial.gd",
                                  "@tool\n"
                                  "class_name CommercialResource extends Resource\n"
                                  "var count = 1;\n"
                                  "func choose(delay := .2):\n"
                                  "    var values = [1, 2]\n"
                                  "    return values.filter(func(value): return value > 1);\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(script.tool);
    REQUIRE_EQ(script.class_name, std::optional<std::string>{"CommercialResource"});
    REQUIRE_EQ(script.base_type, std::optional<std::string>{"Resource"});
    REQUIRE_EQ(script.variables.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.size(), std::size_t{1});
    REQUIRE(script.functions.front().parameters.front().infer_type);
    REQUIRE(script.functions.front().body.back().expression()->operand(1)->lambda() != nullptr);
    REQUIRE_EQ(script.functions.front().body.back().expression()->operand(1)->lambda()->body.size(),
               std::size_t{1});
}

TEST_CASE("parser enforces callable parameter ordering and unique names") {
    const gdpp::SourceFile source{
        "invalid_parameters.gd",
        "signal changed(value: int = 1)\n"
        "func invalid_order(optional = 1, mandatory, optional_two = 2):\n"
        "    var operation = func(duplicate, duplicate): return duplicate\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(diagnostics.has_errors());
    REQUIRE(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS2033"; }));
    REQUIRE(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS2034"; }));
    REQUIRE(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
                        [](const auto& diagnostic) { return diagnostic.code == "GDS2035"; }));
    REQUIRE_EQ(script.signals.front().parameters.size(), std::size_t{1});
    REQUIRE_EQ(script.functions.front().parameters.size(), std::size_t{2});
    const auto* lambda = script.functions.front().body.front().expression()->lambda();
    REQUIRE(lambda != nullptr);
    REQUIRE_EQ(lambda->parameters.size(), std::size_t{1});
}

TEST_CASE("parser preserves function and lambda rest parameters") {
    const gdpp::SourceFile source{
        "rest_parameters.gd",
        "func collect(required: int, optional = 2, ...values: Array) -> Array:\n"
        "    return values\n"
        "static func static_collect(...values):\n"
        "    pass\n"
        "func make_callable():\n"
        "    return func named(prefix: String, ...parts: Array): return parts\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.size(), std::size_t{3});
    REQUIRE_EQ(script.functions.front().parameters.size(), std::size_t{2});
    REQUIRE(script.functions.front().rest_parameter.has_value());
    REQUIRE_EQ(script.functions.front().rest_parameter->name, std::string{"values"});
    REQUIRE_EQ(script.functions.front().rest_parameter->type, std::optional<std::string>{"Array"});
    REQUIRE(script.functions.at(1).is_static);
    REQUIRE(script.functions.at(1).rest_parameter.has_value());
    const auto* lambda = script.functions.back().body.front().expression()->lambda();
    REQUIRE(lambda != nullptr);
    REQUIRE_EQ(lambda->name, std::string{"named"});
    REQUIRE_EQ(lambda->parameters.size(), std::size_t{1});
    REQUIRE(lambda->rest_parameter.has_value());
    REQUIRE_EQ(lambda->rest_parameter->name, std::string{"parts"});
}

TEST_CASE("parser rejects invalid rest parameter placement and defaults") {
    const gdpp::SourceFile source{"invalid_rest_parameters.gd",
                                  "signal changed(...values)\n"
                                  "func after_rest(...values, trailing):\n"
                                  "    pass\n"
                                  "func default_rest(...values = []):\n"
                                  "    pass\n"
                                  "func duplicate(value, ...value):\n"
                                  "    pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(diagnostics.has_errors());
    const auto has = [&](const std::string& code) {
        return std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
                           [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    REQUIRE(has("GDS2035"));
    REQUIRE(has("GDS2036"));
    REQUIRE(has("GDS2037"));
    REQUIRE(has("GDS2038"));
    REQUIRE(script.signals.front().parameters.empty());
    REQUIRE(script.functions.front().rest_parameter.has_value());
    REQUIRE(script.functions.front().parameters.empty());
    REQUIRE(!script.functions.at(1).rest_parameter.has_value());
}

TEST_CASE("parser accepts inline match and conditional suites") {
    const gdpp::SourceFile source{"inline_suites.gd", "func describe(value: int):\n"
                                                      "    match value:\n"
                                                      "        1: return \"one\"\n"
                                                      "        _: return \"other\"\n"
                                                      "func clamp_positive(value: int):\n"
                                                      "    if value > 0: return value\n"
                                                      "    else: return 0\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.functions.size(), std::size_t{2});
    REQUIRE_EQ(script.functions.front().body.front().match_branches().size(), std::size_t{2});
    REQUIRE_EQ(script.functions.back().body.front().body().size(), std::size_t{1});
    REQUIRE_EQ(script.functions.back().body.front().else_body().size(), std::size_t{1});
}
