#include "support/test.hpp"

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/token.hpp"
#include "gdpp/frontend/unicode.hpp"

#include <algorithm>

TEST_CASE("lexer emits structural indentation tokens") {
    const gdpp::SourceFile source{"memory.gd", "func ready() -> void:\n"
                                               "    var score: int = 41\n"
                                               "    score += 1\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(tokens.front().kind, gdpp::TokenKind::kw_func);
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::indent;
            }) != tokens.end());
    REQUIRE_EQ(tokens[tokens.size() - 2].kind, gdpp::TokenKind::dedent);
    REQUIRE_EQ(tokens.back().kind, gdpp::TokenKind::end_of_file);
}

TEST_CASE("lexer decodes strings and ignores comments") {
    const gdpp::SourceFile source{"memory.gd", "var text = \"hello\\nworld\" # comment\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    const auto string_token =
        std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
            return token.kind == gdpp::TokenKind::string;
        });

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(string_token != tokens.end());
    REQUIRE_EQ(string_token->lexeme, std::string{"hello\nworld"});
}

TEST_CASE("lexer distinguishes an empty string from an omitted token lexeme") {
    const gdpp::SourceFile source{"memory.gd", "var text := \"\"\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    const auto string_token =
        std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
            return token.kind == gdpp::TokenKind::string;
        });

    REQUIRE(!diagnostics.has_errors());
    REQUIRE(string_token != tokens.end());
    REQUIRE(string_token->lexeme.empty());
}

TEST_CASE("lexer recognizes GDScript annotations") {
    const gdpp::SourceFile source{"exported.gd", "@export_range(0, 100) var health: int = 100\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(tokens.front().kind, gdpp::TokenKind::at_sign);
    REQUIRE_EQ(tokens.at(1).lexeme, std::string{"export_range"});
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::kw_var;
            }) != tokens.end());
}

TEST_CASE("Unicode security skeleton matches Godot keyword confusable behavior") {
    const auto ascii_as = gdpp::unicode_confusable_skeleton("as");
    const auto cyrillic_as = gdpp::unicode_confusable_skeleton(u8"аs");
    const auto ascii_var = gdpp::unicode_confusable_skeleton("var");
    const auto greek_var = gdpp::unicode_confusable_skeleton(u8"νar");
    const auto composed = gdpp::unicode_confusable_skeleton(u8"é");
    const auto decomposed = gdpp::unicode_confusable_skeleton(u8"é");
    const auto ignored_joiner = gdpp::unicode_confusable_skeleton(u8"a‍s");

    REQUIRE(ascii_as.has_value());
    REQUIRE_EQ(cyrillic_as, ascii_as);
    REQUIRE_EQ(greek_var, ascii_var);
    REQUIRE_EQ(composed, decomposed);
    REQUIRE_EQ(ignored_joiner, ascii_as);
}

TEST_CASE("lexer rejects non-ASCII identifiers confusable with every keyword spelling") {
    const gdpp::SourceFile source{
        "confusable.gd", u8"var аs = 1\nvar νar = 2\nvar сontinue = 3\nvar ordinary变量 = 4\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(diagnostics.has_errors());
    REQUIRE_EQ(std::count_if(diagnostics.items().begin(), diagnostics.items().end(),
                             [](const gdpp::Diagnostic& diagnostic) {
                                 return diagnostic.severity == gdpp::DiagnosticSeverity::error;
                             }),
               std::ptrdiff_t{3});
    REQUIRE(std::any_of(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
        return token.kind == gdpp::TokenKind::identifier && token.lexeme == u8"ordinary变量";
    }));
}

TEST_CASE("lexer recognizes enum declarations") {
    const gdpp::SourceFile source{"state.gd", "enum State { IDLE, RUN = 4 }\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(tokens.front().kind, gdpp::TokenKind::kw_enum);
    REQUIRE_EQ(tokens.at(1).lexeme, std::string{"State"});
}

TEST_CASE("lexer recognizes assert as a statement keyword") {
    const gdpp::SourceFile source{"assert.gd", "assert(true, \"ready\")\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(tokens.front().kind, gdpp::TokenKind::kw_assert);
}

TEST_CASE("lexer recognizes breakpoint as a persistent debugger keyword") {
    const gdpp::SourceFile source{"breakpoint.gd", "breakpoint\nstate.breakpoint = true\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(tokens.front().kind, gdpp::TokenKind::kw_breakpoint);
    REQUIRE_EQ(std::string{gdpp::token_kind_name(tokens.front().kind)},
               std::string{"kw_breakpoint"});
    REQUIRE(std::any_of(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
        return token.kind == gdpp::TokenKind::dot;
    }));
}

TEST_CASE("lexer recognizes modern literals node shorthand and symbolic operators") {
    const gdpp::SourceFile source{"modern.gd", "var mask := 0xff_00 | 0b1010\n"
                                               "var name := &\"player\"\n"
                                               "var path := ^\"Root/Child\"\n"
                                               "var node := $Root/Child\n"
                                               "var unique := %Hud\n"
                                               "var value := 2 ** 3\n"
                                               "value **= 2\n"
                                               "value %= 3\n"
                                               "var ok := true && !false || false\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    const auto has = [&](gdpp::TokenKind kind) {
        return std::find_if(tokens.begin(), tokens.end(), [kind](const gdpp::Token& token) {
                   return token.kind == kind;
               }) != tokens.end();
    };
    REQUIRE(has(gdpp::TokenKind::string_name));
    REQUIRE(has(gdpp::TokenKind::node_path));
    REQUIRE(has(gdpp::TokenKind::node_reference));
    REQUIRE(has(gdpp::TokenKind::power));
    REQUIRE(has(gdpp::TokenKind::power_equal));
    REQUIRE(has(gdpp::TokenKind::percent_equal));
}

TEST_CASE("node shorthand stops before member access") {
    const gdpp::SourceFile source{"nodes.gd", "$Animation.play(&\"take\")\n%Hud.show()\n"};
    gdpp::DiagnosticBag diagnostics;
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(tokens.at(0).kind, gdpp::TokenKind::node_reference);
    REQUIRE_EQ(tokens.at(0).lexeme, std::string{"$Animation"});
    REQUIRE_EQ(tokens.at(1).kind, gdpp::TokenKind::dot);
    REQUIRE_EQ(tokens.at(7).kind, gdpp::TokenKind::node_reference);
    REQUIRE_EQ(tokens.at(7).lexeme, std::string{"%Hud"});
}

TEST_CASE("lexer distinguishes compact modulo from unique-node shorthand") {
    const gdpp::SourceFile source{"commercial_nodes.gd",
                                  "var index := randi()%len($\"../../..\".enemy_kind)\n"
                                  "var volume := %\"全局音量条\".value\n"
                                  "var compact := value%divisor\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::percent; }),
               std::ptrdiff_t{2});
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::node_reference && token.lexeme == "$../../..";
            }) != tokens.end());
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::node_reference &&
                       token.lexeme == "%全局音量条";
            }) != tokens.end());
}

TEST_CASE("lexer accepts compact floats and compound unique-node paths") {
    const gdpp::SourceFile source{"commercial_literals.gd",
                                  "var leading := .2\n"
                                  "var trailing := 4.\n"
                                  "var exponent := .5e+2\n"
                                  "var unique := $%Hud\n"
                                  "var nested := $%BomberMesh/%ParticleSparks\n"
                                  "var relative := $../Camera\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(std::count_if(tokens.begin(), tokens.end(),
                             [](const gdpp::Token& token) {
                                 return token.kind == gdpp::TokenKind::floating;
                             }),
               std::ptrdiff_t{3});
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::node_reference && token.lexeme == "$%Hud";
            }) != tokens.end());
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::node_reference &&
                       token.lexeme == "$%BomberMesh/%ParticleSparks";
            }) != tokens.end());
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::node_reference &&
                       token.lexeme == "$../Camera";
            }) != tokens.end());
}

TEST_CASE("lexer distinguishes member dots match rest and parameter ellipsis") {
    const gdpp::SourceFile source{"periods.gd", "var compact := .5\n"
                                                "var member := owner.value\n"
                                                "match values:\n"
                                                "    [var first, ..]: pass\n"
                                                "func collect(...args):\n"
                                                "    pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(
        std::count_if(tokens.begin(), tokens.end(),
                      [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::dot; }),
        std::ptrdiff_t{1});
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::dot_dot; }),
               std::ptrdiff_t{1});
    REQUIRE_EQ(std::count_if(tokens.begin(), tokens.end(),
                             [](const gdpp::Token& token) {
                                 return token.kind == gdpp::TokenKind::ellipsis;
                             }),
               std::ptrdiff_t{1});
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::floating && token.lexeme == "0.5";
            }) != tokens.end());
    REQUIRE_EQ(std::string{gdpp::token_kind_name(gdpp::TokenKind::dot_dot)},
               std::string{"dot_dot"});
    REQUIRE_EQ(std::string{gdpp::token_kind_name(gdpp::TokenKind::ellipsis)},
               std::string{"ellipsis"});
}

TEST_CASE("node shorthand validates and joins quoted unique path segments") {
    const gdpp::SourceFile valid{"node_segments.gd", "var first := $%\"Hey\"/%\"Howdy\"\n"
                                                     "var second := %\"Hey\"/%\"Howdy\"\n"
                                                     "var relative := $../../Camera\n"};
    gdpp::DiagnosticBag valid_diagnostics;
    const auto tokens = gdpp::Lexer{valid, valid_diagnostics}.scan();

    REQUIRE(!valid_diagnostics.has_errors());
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const auto& token) {
                return token.kind == gdpp::TokenKind::node_reference &&
                       token.lexeme == "$%Hey/%Howdy";
            }) != tokens.end());
    REQUIRE(std::find_if(tokens.begin(), tokens.end(), [](const auto& token) {
                return token.kind == gdpp::TokenKind::node_reference &&
                       token.lexeme == "%Hey/%Howdy";
            }) != tokens.end());

    const gdpp::SourceFile invalid{"invalid_node_segments.gd", "$23\n$Root/23\n"};
    gdpp::DiagnosticBag invalid_diagnostics;
    (void)gdpp::Lexer{invalid, invalid_diagnostics}.scan();
    REQUIRE(invalid_diagnostics.has_errors());
}

TEST_CASE("lexer canonicalizes every current GDScript numeric literal form") {
    const gdpp::SourceFile source{"numeric_literals.gd", "var decimal := 12_345_678\n"
                                                         "var hexadecimal := 0x7fff_ffff\n"
                                                         "var binary := 0b1010_0101\n"
                                                         "var leading := .5\n"
                                                         "var trailing := 4.\n"
                                                         "var exponent := 1_2.5_0e+1_0\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    std::vector<std::string> integers;
    std::vector<std::string> floating;
    for (const auto& token : tokens) {
        if (token.kind == gdpp::TokenKind::integer)
            integers.push_back(token.lexeme);
        else if (token.kind == gdpp::TokenKind::floating)
            floating.push_back(token.lexeme);
    }
    REQUIRE_EQ(integers.size(), std::size_t{3});
    REQUIRE_EQ(integers.at(0), std::string{"12345678"});
    REQUIRE_EQ(integers.at(1), std::string{"2147483647"});
    REQUIRE_EQ(integers.at(2), std::string{"165"});
    REQUIRE_EQ(floating.size(), std::size_t{3});
    REQUIRE_EQ(floating.at(0), std::string{"0.5"});
    REQUIRE_EQ(floating.at(1), std::string{"4.0"});
    REQUIRE_EQ(floating.at(2), std::string{"12.50e+10"});
}

TEST_CASE("lexer matches Godot trailing numeric separator rules") {
    const gdpp::SourceFile accepted{"trailing_separators.gd", "var decimal := 123_\n"
                                                              "var hexadecimal := 0x12_f_\n"
                                                              "var binary := 0b10_1_\n"
                                                              "var fraction := 1_2_.4_\n"
                                                              "var exponent := 1.5e0_3_\n"};
    gdpp::DiagnosticBag accepted_diagnostics;
    const auto accepted_tokens = gdpp::Lexer{accepted, accepted_diagnostics}.scan();

    REQUIRE(!accepted_diagnostics.has_errors());
    REQUIRE(std::find_if(accepted_tokens.begin(), accepted_tokens.end(), [](const auto& token) {
                return token.kind == gdpp::TokenKind::integer && token.lexeme == "123";
            }) != accepted_tokens.end());
    REQUIRE(std::find_if(accepted_tokens.begin(), accepted_tokens.end(), [](const auto& token) {
                return token.kind == gdpp::TokenKind::integer && token.lexeme == "303";
            }) != accepted_tokens.end());

    const gdpp::SourceFile rejected{"repeated_separators.gd", "var value := 1__2\n"};
    gdpp::DiagnosticBag rejected_diagnostics;
    (void)gdpp::Lexer{rejected, rejected_diagnostics}.scan();
    REQUIRE(rejected_diagnostics.has_errors());
}

TEST_CASE("lexer decodes raw triple and Unicode string contracts") {
    const gdpp::SourceFile source{"string_literals.gd",
                                  R"gd(var escaped := "\a\b\f\v\u0041\U01F600\uD83D\uDE00"
var continued := "left\
right"
var raw := r"\n\"quoted\"\\path"
var triple := """first
"quoted"
last"""
var raw_triple := r'''raw\n
"quoted"'''
var name := &"""alpha
beta"""
)gd"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    std::vector<std::string> strings;
    std::string string_name;
    for (const auto& token : tokens) {
        if (token.kind == gdpp::TokenKind::string)
            strings.push_back(token.lexeme);
        else if (token.kind == gdpp::TokenKind::string_name)
            string_name = token.lexeme;
    }
    REQUIRE_EQ(strings.size(), std::size_t{5});
    const std::string emoji{"\xf0\x9f\x98\x80"};
    REQUIRE_EQ(strings.at(0), std::string{"\a\b\f\vA"} + emoji + emoji);
    REQUIRE_EQ(strings.at(1), std::string{"leftright"});
    REQUIRE_EQ(strings.at(2), std::string{"\\n\\\"quoted\\\"\\\\path"});
    REQUIRE_EQ(strings.at(3), std::string{"first\n\"quoted\"\nlast"});
    REQUIRE_EQ(strings.at(4), std::string{"raw\\n\n\"quoted\""});
    REQUIRE_EQ(string_name, std::string{"alpha\nbeta"});
}

TEST_CASE("lexer matches Godot replacement and range behavior at literal boundaries") {
    const gdpp::SourceFile source{"literal_boundaries.gd",
                                  "var nul := \"A\\u0000B\"\nvar long_nul := \"A\\U000000B\"\n"
                                  "var maximum := 1.7976931348623157e308\n"
                                  "var rounded_overflow := 1.7976931348623158e308\n"
                                  "var overflow := 1e400\nvar underflow := 1e-4000\n"
                                  "var subnormal := 1e-309\nvar zero_overflow := 0e400\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    std::vector<std::string> strings;
    std::vector<std::string> floating;
    for (const auto& token : tokens) {
        if (token.kind == gdpp::TokenKind::string)
            strings.push_back(token.lexeme);
        else if (token.kind == gdpp::TokenKind::floating)
            floating.push_back(token.lexeme);
    }
    const std::string replacement{"\xef\xbf\xbd"};
    REQUIRE_EQ(strings.size(), std::size_t{2});
    REQUIRE_EQ(strings.at(0), std::string{"A"} + replacement + "B");
    REQUIRE_EQ(strings.at(1), std::string{"A"} + replacement + "B");
    REQUIRE_EQ(floating, (std::vector<std::string>{"1.7976931348623157e308", "inf", "inf", "0.0",
                                                   "0.0", "nan"}));
    REQUIRE_EQ(std::count_if(
                   diagnostics.items().begin(), diagnostics.items().end(),
                   [](const gdpp::Diagnostic& diagnostic) { return diagnostic.code == "GDS1008"; }),
               std::ptrdiff_t{2});
    REQUIRE_EQ(std::count_if(
                   diagnostics.items().begin(), diagnostics.items().end(),
                   [](const gdpp::Diagnostic& diagnostic) { return diagnostic.code == "GDS1007"; }),
               std::ptrdiff_t{1});
}

TEST_CASE("lexer rejects malformed literals before parsing") {
    const std::vector<std::string> invalid_sources{
        "var value := 0o77\n",
        "var value := 0x_FF\n",
        "var value := 0b102\n",
        "var value := 1__2\n",
        "var value := 1._2\n",
        "var value := 1e_2\n",
        "var value := 0x8000_0000_0000_0000\n",
        "var value := 9_223_372_036_854_775_809\n",
        "var value := \"\\q\"\n",
        "var value := \"\\uD800\"\n",
        "var value := \"\\uDC00\"\n",
        "var value := \"\\U110000\"\n",
        R"gd(var value := r"ends\"
)gd",
    };

    for (const auto& text : invalid_sources) {
        const gdpp::SourceFile source{"invalid_literal.gd", text};
        gdpp::DiagnosticBag diagnostics;
        (void)gdpp::Lexer{source, diagnostics}.scan();
        REQUIRE(diagnostics.has_errors());
    }
}

TEST_CASE("lexer preserves lambda blocks inside continued call arguments") {
    const gdpp::SourceFile source{"lambda.gd", "func ready(signal_value: Signal) -> void:\n"
                                               "    signal_value.connect(\n"
                                               "        func(value: int) -> void:\n"
                                               "            print(value)\n"
                                               "            )\n"
                                               "    signal_value.connect(\n"
                                               "        func() -> void:\n"
                                               "            print(1))\n"
                                               "    pass\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::kw_func; }),
               std::ptrdiff_t{3});
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::indent; }),
               std::ptrdiff_t{3});
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::dedent; }),
               std::ptrdiff_t{3});
}

TEST_CASE("lexer handles explicit line continuation and rejects malformed separators") {
    const gdpp::SourceFile continued{"continued.gd", R"(var value := 1 + \
        2
)"};
    gdpp::DiagnosticBag continued_diagnostics;
    const auto tokens = gdpp::Lexer{continued, continued_diagnostics}.scan();

    REQUIRE(!continued_diagnostics.has_errors());
    REQUIRE(std::count_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
                return token.kind == gdpp::TokenKind::newline;
            }) == 1);

    const gdpp::SourceFile malformed{"malformed.gd", "var value := 1__2\n"};
    gdpp::DiagnosticBag malformed_diagnostics;
    (void)gdpp::Lexer{malformed, malformed_diagnostics}.scan();
    REQUIRE(malformed_diagnostics.has_errors());
}

TEST_CASE("lexer ignores UTF-8 BOM and preserves multiline strings") {
    const gdpp::SourceFile source{"utf8.gd", "\xef\xbb\xbf"
                                             "extends Node\n"
                                             "var message = \"first\nsecond\"\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(tokens.front().kind, gdpp::TokenKind::kw_extends);
    const auto string_token =
        std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
            return token.kind == gdpp::TokenKind::string;
        });
    REQUIRE(string_token != tokens.end());
    REQUIRE_EQ(string_token->lexeme, std::string{"first\nsecond"});
}

TEST_CASE("lexer preserves nested lambda layout and suppresses collection layout") {
    const gdpp::SourceFile source{"nested_lambdas.gd", "func run() -> void:\n"
                                                       "    visit(func(item):\n"
                                                       "        call(func(value):\n"
                                                       "            print(value)\n"
                                                       "        , {\n"
                                                       "            \"item\": item,\n"
                                                       "            \"value\": 1\n"
                                                       "        })\n"
                                                       "    , \"done\")\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::indent; }),
               std::ptrdiff_t{3});
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::dedent; }),
               std::ptrdiff_t{3});
}

TEST_CASE("lexer carries explicit continuation across comment-only lines") {
    const gdpp::SourceFile source{"continued_comments.gd", "func run() -> void:\n"
                                                           "    var enabled = \\\n"
                                                           "        # first condition\n"
                                                           "        true || \\\n"
                                                           "        # second condition\n"
                                                           "        false\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(std::count_if(
                   tokens.begin(), tokens.end(),
                   [](const gdpp::Token& token) { return token.kind == gdpp::TokenKind::newline; }),
               std::ptrdiff_t{2});
}

TEST_CASE("lexer accepts the exact Godot 4.7 Unicode identifier contract") {
    const gdpp::SourceFile source{"unicode_identifiers.gd", "var Հայերեն := 1\n"
                                                            "var العربية := 2\n"
                                                            "var বাংলা := 3\n"
                                                            "var русский := 4\n"
                                                            "var हिन्दी := 5\n"
                                                            "var 한국어 := 6\n"
                                                            "var 中文 := 7\n"
                                                            "var 日本語 := 8\n"
                                                            "var മലയാളം := 9\n"
                                                            "var ไทย := 10\n"
                                                            "var Ελληνικά := 11\n"
                                                            "var π := 12\n"
                                                            "var ㄥ := 13\n"
                                                            "var é := 14\n"
                                                            "var é := 15\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    const auto has_identifier = [&](const std::string& value) {
        return std::find_if(tokens.begin(), tokens.end(), [&](const gdpp::Token& token) {
                   return token.kind == gdpp::TokenKind::identifier && token.lexeme == value;
               }) != tokens.end();
    };
    REQUIRE(has_identifier("Հայերեն"));
    REQUIRE(has_identifier("العربية"));
    REQUIRE(has_identifier("বাংলা"));
    REQUIRE(has_identifier("русский"));
    REQUIRE(has_identifier("हिन्दी"));
    REQUIRE(has_identifier("한국어"));
    REQUIRE(has_identifier("中文"));
    REQUIRE(has_identifier("日本語"));
    REQUIRE(has_identifier("മലയാളം"));
    REQUIRE(has_identifier("ไทย"));
    REQUIRE(has_identifier("Ελληνικά"));
    REQUIRE(has_identifier("π"));
    REQUIRE(has_identifier("ㄥ"));
    REQUIRE(has_identifier("é"));
    REQUIRE(has_identifier("é"));
}

TEST_CASE("lexer rejects invalid Unicode starts malformed UTF-8 and source NUL") {
    const std::vector<std::string> invalid_sources{
        "var 😀 := 1\n",
        "var ́value := 1\n",
        "var ٠value := 1\n",
        std::string{"var \xc0\xaf := 1\n"},
        std::string{"var \x80 := 1\n"},
        std::string{"var \xe2\x82 := 1\n"},
        std::string{"var \xed\xa0\x80 := 1\n"},
        std::string{"var \xf4\x90\x80\x80 := 1\n"},
        std::string{"var bad := 1\n\0var next := 2\n", 28},
    };

    for (const auto& text : invalid_sources) {
        const gdpp::SourceFile source{"invalid_unicode.gd", text};
        gdpp::DiagnosticBag diagnostics;
        (void)gdpp::Lexer{source, diagnostics}.scan();
        REQUIRE(diagnostics.has_errors());
    }
}

TEST_CASE("lexer reports Unicode columns in codepoints while retaining byte offsets") {
    const gdpp::SourceFile source{"unicode_columns.gd", "var π := 1\n"};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();

    REQUIRE(!diagnostics.has_errors());
    const auto identifier =
        std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
            return token.kind == gdpp::TokenKind::identifier && token.lexeme == "π";
        });
    const auto colon = std::find_if(tokens.begin(), tokens.end(), [](const gdpp::Token& token) {
        return token.kind == gdpp::TokenKind::colon_equal;
    });
    REQUIRE(identifier != tokens.end());
    REQUIRE(colon != tokens.end());
    REQUIRE_EQ(identifier->span.begin.offset, std::size_t{4});
    REQUIRE_EQ(identifier->span.begin.column, std::size_t{5});
    REQUIRE_EQ(identifier->span.end.offset, std::size_t{6});
    REQUIRE_EQ(identifier->span.end.column, std::size_t{6});
    REQUIRE_EQ(colon->span.begin.offset, std::size_t{7});
    REQUIRE_EQ(colon->span.begin.column, std::size_t{7});
}

TEST_CASE("lexer follows Godot newline comment and indentation rules") {
    const gdpp::SourceFile clean{
        "layout.gd", "# header\n\nfunc run() -> void:\n    # body\n    pass\n# trailer"};
    gdpp::DiagnosticBag clean_diagnostics;
    const auto clean_tokens = gdpp::Lexer{clean, clean_diagnostics}.scan();
    REQUIRE(!clean_diagnostics.has_errors());
    REQUIRE_EQ(
        std::count_if(clean_tokens.begin(), clean_tokens.end(),
                      [](const auto& token) { return token.kind == gdpp::TokenKind::newline; }),
        std::ptrdiff_t{2});

    const gdpp::SourceFile crlf{"crlf.gd", "func run() -> void:\r\n\tpass\r\n"};
    gdpp::DiagnosticBag crlf_diagnostics;
    (void)gdpp::Lexer{crlf, crlf_diagnostics}.scan();
    REQUIRE(!crlf_diagnostics.has_errors());

    const std::vector<std::string> invalid_layouts{
        "func run() -> void:\r    pass\n",
        "func run() -> void:\n \tpass\n",
        "if true:\n    pass\nif true:\n\tpass\n",
    };
    for (const auto& text : invalid_layouts) {
        const gdpp::SourceFile invalid{"invalid_layout.gd", text};
        gdpp::DiagnosticBag diagnostics;
        (void)gdpp::Lexer{invalid, diagnostics}.scan();
        REQUIRE(diagnostics.has_errors());
    }
}
