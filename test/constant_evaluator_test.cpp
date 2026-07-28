#include "support/test.hpp"

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/frontend/constant_evaluator.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/parser.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

std::optional<std::int64_t>
evaluate(std::string expression,
         const std::unordered_map<std::string, std::int64_t>& constants = {}) {
    gdpp::DiagnosticBag diagnostics;
    const gdpp::SourceFile source{"constant.gd", "const VALUE = " + expression + "\n"};
    gdpp::Lexer lexer{source, diagnostics};
    const auto tokens = lexer.scan();
    gdpp::Parser parser{tokens, diagnostics};
    const auto script = parser.parse_script();
    REQUIRE(!diagnostics.has_errors());
    REQUIRE_EQ(script.variables.size(), std::size_t{1});
    REQUIRE(script.variables.front().initializer != nullptr);
    return gdpp::evaluate_integer_constant(*script.variables.front().initializer, constants);
}

} // namespace

TEST_CASE("integer constants use the same wrapped arithmetic as GDScript") {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    REQUIRE_EQ(evaluate("9223372036854775807 + 1"), std::optional<std::int64_t>{minimum});
    REQUIRE_EQ(evaluate("-9223372036854775808 - 1"), std::optional<std::int64_t>{maximum});
    REQUIRE_EQ(evaluate("9223372036854775807 * 2"), std::optional<std::int64_t>{-2});
    REQUIRE_EQ(evaluate("-(-9223372036854775808)"), std::optional<std::int64_t>{minimum});
    REQUIRE_EQ(evaluate("-9223372036854775808 / -1"), std::optional<std::int64_t>{minimum});
    REQUIRE_EQ(evaluate("-9223372036854775808 % -1"), std::optional<std::int64_t>{0});
}

TEST_CASE("integer constants normalize shifts and preserve signed bit operations") {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    REQUIRE_EQ(evaluate("1 << 63"), std::optional<std::int64_t>{minimum});
    REQUIRE_EQ(evaluate("1 << 64"), std::optional<std::int64_t>{1});
    REQUIRE_EQ(evaluate("1 << -1"), std::optional<std::int64_t>{minimum});
    REQUIRE_EQ(evaluate("-9223372036854775808 >> 1"), std::optional<std::int64_t>{minimum / 2});
    REQUIRE_EQ(evaluate("9223372036854775807 >> 64"), std::optional<std::int64_t>{maximum});
    REQUIRE_EQ(evaluate("~0"), std::optional<std::int64_t>{-1});
    REQUIRE_EQ(evaluate("-1 & 0x55aa"), std::optional<std::int64_t>{0x55AA});
    REQUIRE_EQ(evaluate("0x5500 | 0xaa"), std::optional<std::int64_t>{0x55AA});
    REQUIRE_EQ(evaluate("0x55ff ^ 0x55"), std::optional<std::int64_t>{0x55AA});
}

TEST_CASE("integer constant division by zero remains a rejected constant expression") {
    REQUIRE(!evaluate("7 / 0"));
    REQUIRE(!evaluate("7 % 0"));
}

TEST_CASE("integer constants resolve qualified script and enum members") {
    const std::unordered_map<std::string, std::int64_t> constants{
        {"ProgressCommons.UnknownProgress", 0},
        {"EChannel.CONNECT", 4},
    };
    REQUIRE_EQ(evaluate("ProgressCommons.UnknownProgress", constants),
               std::optional<std::int64_t>{0});
    REQUIRE_EQ(evaluate("EChannel.CONNECT + 1", constants), std::optional<std::int64_t>{5});
}
