#include "support/test.hpp"

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/frontend/ast_serializer.hpp"
#include "gdpp/frontend/lexer.hpp"
#include "gdpp/frontend/parser.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("AST serialization matches the complete source-span golden contract") {
    const auto source_root = std::filesystem::path{GDPP_TEST_SOURCE_DIR};
    const auto source_path = source_root / "test/fixtures/ast_comprehensive.gd";
    const auto source_text = read_file(source_path);
    REQUIRE(!source_text.empty());

    const gdpp::SourceFile source{"ast_comprehensive.gd", source_text};
    gdpp::DiagnosticBag diagnostics;
    const auto tokens = gdpp::Lexer{source, diagnostics}.scan();
    const auto script = gdpp::Parser{tokens, diagnostics}.parse_script();
    REQUIRE(!diagnostics.has_errors());

    const auto actual = gdpp::AstSerializer{}.serialize(script);
    const auto golden_path = source_root / "test/fixtures/ast_comprehensive.ast";
    const auto expected = read_file(golden_path);
    if (actual != expected) {
        const auto actual_path =
            std::filesystem::path{GDPP_TEST_BINARY_DIR} / "ast_comprehensive.actual";
        std::ofstream output{actual_path, std::ios::binary | std::ios::trunc};
        output.write(actual.data(), static_cast<std::streamsize>(actual.size()));
    }
    REQUIRE_EQ(actual, expected);
}
