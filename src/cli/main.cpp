#include "gdpp/compiler/compiler.hpp"
#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/project/project_compiler.hpp"
#include "gdpp/core/path_utf8.hpp"
#include "gdpp/version.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

namespace {

void print_usage() {
    std::cerr << "GDPP " << GDPP_VERSION_STRING << "\n\n"
              << "Usage:\n"
              << "  gdpp compile <script.gd> --output <directory> [--no-optimize]\n"
              << "               [--metrics-json <file>]\n"
              << "  gdpp project <project-dir> [--output <build-dir>]\n"
              << "                 [--sdk-root <dir>] [--godot-cpp <dir>]\n"
              << "                 [--target-godot 4.4|4.5|4.6|4.7]\n"
              << "  gdpp --version\n";
}

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output{path, std::ios::binary};
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

std::string json_string(const std::string& value) {
    std::string result{"\""};
    for (const char character : value) {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        if (character == '\n')
            result += "\\n";
        else if (character == '\r')
            result += "\\r";
        else if (character == '\t')
            result += "\\t";
        else
            result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::string metrics_json(const std::filesystem::path& source, const gdpp::CompileResult& result) {
    const auto& value = result.metrics;
    std::ostringstream output;
    output << "{\n  \"schema\": 1,\n  \"source\": " << json_string(gdpp::path_to_utf8(source))
           << ",\n  \"success\": " << (result.success ? "true" : "false")
           << ",\n  \"duration_ns\": {\n"
           << "    \"total\": " << value.total_ns << ",\n"
           << "    \"lex\": " << value.lex_ns << ",\n"
           << "    \"parse\": " << value.parse_ns << ",\n"
           << "    \"semantic\": " << value.semantic_ns << ",\n"
           << "    \"hir_lower\": " << value.hir_lower_ns << ",\n"
           << "    \"optimize\": " << value.optimize_ns << ",\n"
           << "    \"mir_lower_verify\": " << value.mir_lower_verify_ns << ",\n"
           << "    \"mir_optimize\": " << value.mir_optimize_ns << ",\n"
           << "    \"codegen\": " << value.codegen_ns << "\n  },\n"
           << "  \"counts\": {\n"
           << "    \"tokens\": " << value.token_count << ",\n"
           << "    \"ast_expressions\": " << value.ast_expression_count << ",\n"
           << "    \"ast_statements\": " << value.ast_statement_count << ",\n"
           << "    \"hir_expressions\": " << value.hir_expression_count << ",\n"
           << "    \"hir_statements\": " << value.hir_statement_count << ",\n"
           << "    \"mir_functions\": " << value.mir_function_count << ",\n"
           << "    \"mir_blocks\": " << value.mir_block_count << ",\n"
           << "    \"mir_instructions\": " << value.mir_instruction_count << "\n  }\n}\n";
    return output.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string{argv[1]} == "--version") {
        std::cout << GDPP_VERSION_STRING << '\n';
        return 0;
    }
    if (argc >= 3 && std::string{argv[1]} == "project") {
        gdpp::ProjectCompileOptions options;
        options.project_root = std::filesystem::absolute(gdpp::path_from_utf8(argv[2]));
        options.output_directory = options.project_root / "addons/gdpp/build/project";
        options.sdk_root = std::filesystem::current_path();
        options.godot_cpp_directory = options.sdk_root / "third/godot-cpp";
        for (int index = 3; index < argc; ++index) {
            const std::string argument{argv[index]};
            if (argument == "--output" && index + 1 < argc)
                options.output_directory =
                    std::filesystem::absolute(gdpp::path_from_utf8(argv[++index]));
            else if (argument == "--sdk-root" && index + 1 < argc)
                options.sdk_root = std::filesystem::absolute(gdpp::path_from_utf8(argv[++index]));
            else if (argument == "--godot-cpp" && index + 1 < argc)
                options.godot_cpp_directory =
                    std::filesystem::absolute(gdpp::path_from_utf8(argv[++index]));
            else if (argument == "--no-optimize")
                options.compiler.optimize = false;
            else if (argument == "--target-godot" && index + 1 < argc) {
                const auto version = gdpp::parse_godot_version(argv[++index]);
                if (!version) {
                    std::cerr << "unsupported Godot target; expected 4.4, 4.5, 4.6, or 4.7\n";
                    return 2;
                }
                options.compiler.target_version = *version;
            } else {
                std::cerr << "unknown project argument: " << argument << '\n';
                print_usage();
                return 2;
            }
        }
        const gdpp::ProjectCompiler compiler;
        const auto result = compiler.compile(options);
        for (const auto& diagnostic : result.diagnostics) {
            const auto severity =
                diagnostic.diagnostic.severity == gdpp::DiagnosticSeverity::warning ? "warning"
                : diagnostic.diagnostic.severity == gdpp::DiagnosticSeverity::note  ? "note"
                                                                                    : "error";
            std::cerr << gdpp::path_to_utf8(diagnostic.path) << ':'
                      << diagnostic.diagnostic.span.begin.line << ':'
                      << diagnostic.diagnostic.span.begin.column << ": " << severity << '['
                      << diagnostic.diagnostic.code << "]: " << diagnostic.diagnostic.message
                      << '\n';
        }
        if (!result.success)
            return 1;
        std::cout << "project generated: " << result.scripts.size() << " script(s), "
                  << result.compiled_count << " generated, " << result.removed_count
                  << " removed; native compilation is performed by the export integration\n";
        return 0;
    }
    if (argc < 3 || std::string{argv[1]} != "compile") {
        print_usage();
        return 2;
    }

    const auto input_path = gdpp::path_from_utf8(argv[2]);
    std::filesystem::path output_path{"build/generated"};
    std::filesystem::path metrics_path;
    gdpp::CompileOptions compile_options;
    for (int index = 3; index < argc; ++index) {
        if (std::string{argv[index]} == "--output" && index + 1 < argc) {
            output_path = gdpp::path_from_utf8(argv[++index]);
        } else if (std::string{argv[index]} == "--no-optimize") {
            compile_options.optimize = false;
        } else if (std::string{argv[index]} == "--metrics-json" && index + 1 < argc) {
            metrics_path = gdpp::path_from_utf8(argv[++index]);
        } else if (std::string{argv[index]} == "--target-godot" && index + 1 < argc) {
            const auto version = gdpp::parse_godot_version(argv[++index]);
            if (!version) {
                std::cerr << "unsupported Godot target; expected 4.4, 4.5, 4.6, or 4.7\n";
                return 2;
            }
            compile_options.target_version = *version;
        } else {
            std::cerr << "unknown argument: " << argv[index] << '\n';
            print_usage();
            return 2;
        }
    }

    std::ifstream input{input_path, std::ios::binary};
    if (!input) {
        std::cerr << "cannot open input file: " << gdpp::path_to_utf8(input_path) << '\n';
        return 2;
    }
    const std::string source_text{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
    const auto input_name = gdpp::path_to_utf8(input_path);
    const gdpp::SourceFile source{input_name, source_text};
    const gdpp::Compiler compiler;
    const auto result = compiler.compile(input_name, source_text, compile_options);
    if (!metrics_path.empty()) {
        std::error_code metrics_error;
        if (!metrics_path.parent_path().empty())
            std::filesystem::create_directories(metrics_path.parent_path(), metrics_error);
        if (metrics_error || !write_file(metrics_path, metrics_json(input_path, result))) {
            std::cerr << "cannot write compiler metrics: " << gdpp::path_to_utf8(metrics_path)
                      << '\n';
            return 2;
        }
    }
    for (const auto& diagnostic : result.diagnostics) {
        std::cerr << gdpp::format_diagnostic(diagnostic, source, true);
    }
    if (!result.success) {
        return 1;
    }

    std::error_code error;
    std::filesystem::create_directories(output_path, error);
    if (error) {
        std::cerr << "cannot create output directory: " << error.message() << '\n';
        return 2;
    }
    const auto header_path = output_path / result.unit.header_file_name;
    const auto source_path = output_path / result.unit.source_file_name;
    const auto symbol_path = output_path / result.unit.symbol_file_name;
    if (!write_file(header_path, result.unit.header) ||
        !write_file(source_path, result.unit.source) ||
        !write_file(symbol_path, result.unit.symbol_map)) {
        std::cerr << "cannot write generated files under: " << gdpp::path_to_utf8(output_path)
                  << '\n';
        return 2;
    }
    std::cout << "compiled " << gdpp::path_to_utf8(input_path) << " -> "
              << gdpp::path_to_utf8(source_path) << " (" << result.optimization.constants_folded
              << " constants folded, " << result.optimization.statements_removed
              << " statements removed, " << result.optimization.branches_simplified
              << " HIR branches simplified, " << result.mir_optimization.branches_simplified
              << " MIR branches simplified, " << result.mir_optimization.blocks_removed
              << " MIR blocks removed)\n";
    return 0;
}
