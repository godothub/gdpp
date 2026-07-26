#include "gdpp/frontend/lexer.hpp"

#include "gdpp/frontend/literal.hpp"
#include "gdpp/frontend/unicode.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gdpp {
namespace {

struct DecodedUtf8 {
    std::uint32_t codepoint{0};
    std::size_t length{0};
};

std::optional<DecodedUtf8> decode_utf8(std::string_view text, std::size_t offset) noexcept {
    if (offset >= text.size())
        return std::nullopt;
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80U)
        return DecodedUtf8{first, 1};

    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2U && first <= 0xdfU) {
        length = 2;
        codepoint = first & 0x1fU;
        minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        length = 3;
        codepoint = first & 0x0fU;
        minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        length = 4;
        codepoint = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return std::nullopt;
    }
    if (offset + length > text.size())
        return std::nullopt;
    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(text[offset + index]);
        if ((continuation & 0xc0U) != 0x80U)
            return std::nullopt;
        codepoint = (codepoint << 6U) | (continuation & 0x3fU);
    }
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        return std::nullopt;
    }
    return DecodedUtf8{codepoint, length};
}

bool is_ascii_identifier_start(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return std::isalpha(byte) != 0 || value == '_';
}

bool is_ascii_identifier_continue(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) != 0 || value == '_';
}

bool identifier_start_at(std::string_view text, std::size_t offset) noexcept {
    if (offset >= text.size())
        return false;
    const auto byte = static_cast<unsigned char>(text[offset]);
    if (byte < 0x80U)
        return is_ascii_identifier_start(text[offset]);
    const auto decoded = decode_utf8(text, offset);
    return decoded && is_unicode_identifier_start(decoded->codepoint);
}

bool identifier_continue_at(std::string_view text, std::size_t offset) noexcept {
    if (offset >= text.size())
        return false;
    const auto byte = static_cast<unsigned char>(text[offset]);
    if (byte < 0x80U)
        return is_ascii_identifier_continue(text[offset]);
    const auto decoded = decode_utf8(text, offset);
    return decoded && is_unicode_identifier_continue(decoded->codepoint);
}

bool can_end_expression(TokenKind kind) {
    switch (kind) {
    case TokenKind::identifier:
    case TokenKind::integer:
    case TokenKind::floating:
    case TokenKind::string:
    case TokenKind::string_name:
    case TokenKind::node_path:
    case TokenKind::node_reference:
    case TokenKind::kw_true:
    case TokenKind::kw_false:
    case TokenKind::kw_null:
    case TokenKind::kw_self:
    case TokenKind::right_paren:
    case TokenKind::right_bracket:
    case TokenKind::right_brace:
        return true;
    default:
        return false;
    }
}

int hexadecimal_digit(char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU))
        return false;
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return true;
}

const std::unordered_map<std::string_view, TokenKind> keywords{
    {"extends", TokenKind::kw_extends},
    {"class_name", TokenKind::kw_class_name},
    {"class", TokenKind::kw_class},
    {"func", TokenKind::kw_func},
    {"var", TokenKind::kw_var},
    {"const", TokenKind::kw_const},
    {"signal", TokenKind::kw_signal},
    {"enum", TokenKind::kw_enum},
    {"static", TokenKind::kw_static},
    {"return", TokenKind::kw_return},
    {"assert", TokenKind::kw_assert},
    {"breakpoint", TokenKind::kw_breakpoint},
    {"pass", TokenKind::kw_pass},
    {"if", TokenKind::kw_if},
    {"match", TokenKind::kw_match},
    {"when", TokenKind::kw_when},
    {"elif", TokenKind::kw_elif},
    {"else", TokenKind::kw_else},
    {"while", TokenKind::kw_while},
    {"for", TokenKind::kw_for},
    {"in", TokenKind::kw_in},
    {"break", TokenKind::kw_break},
    {"continue", TokenKind::kw_continue},
    {"true", TokenKind::kw_true},
    {"false", TokenKind::kw_false},
    {"null", TokenKind::kw_null},
    {"self", TokenKind::kw_self},
    {"and", TokenKind::kw_and},
    {"or", TokenKind::kw_or},
    {"not", TokenKind::kw_not},
    {"as", TokenKind::kw_as},
    {"is", TokenKind::kw_is},
    {"await", TokenKind::kw_await},
};

} // namespace

Lexer::Lexer(const SourceFile& source, DiagnosticBag& diagnostics, FrontendLimits limits)
    : source_(source), diagnostics_(diagnostics), limits_(limits) {}

bool Lexer::at_end() const noexcept { return offset_ >= source_.text().size(); }

char Lexer::peek(std::size_t distance) const noexcept {
    const auto target = offset_ + distance;
    return target < source_.text().size() ? source_.text()[target] : '\0';
}

char Lexer::advance() noexcept {
    const char value = peek();
    if (at_end()) {
        return '\0';
    }
    ++offset_;
    if (value == '\n') {
        ++line_;
        column_ = 1;
    } else if ((static_cast<unsigned char>(value) & 0xc0U) != 0x80U) {
        ++column_;
    }
    return value;
}

bool Lexer::match(char expected) noexcept {
    if (peek() != expected) {
        return false;
    }
    advance();
    return true;
}

SourceLocation Lexer::location() const noexcept { return {offset_, line_, column_}; }

void Lexer::report_limit(const char* resource, SourceSpan span) {
    if (halted_)
        return;
    diagnostics_.error("GDS1010", std::string{resource} + " exceeds the configured frontend limit",
                       span);
    halted_ = true;
}

bool Lexer::literal_within_limit(SourceLocation begin) {
    if (offset_ - begin.offset <= limits_.max_literal_bytes)
        return true;
    report_limit("literal size", {begin, location()});
    return false;
}

void Lexer::emit_eof() {
    const auto eof = location();
    tokens_.push_back({TokenKind::end_of_file, "", {eof, eof}});
}

void Lexer::validate_utf8() {
    const auto text = std::string_view{source_.text()};
    if (text.size() > limits_.max_source_bytes) {
        report_limit("source size", {{0, 1, 1}, {text.size(), 1, 1}});
        return;
    }
    std::size_t cursor = 0;
    std::size_t line = 1;
    std::size_t column = 1;
    std::size_t line_bytes = 0;
    while (cursor < text.size()) {
        const auto byte = static_cast<unsigned char>(text[cursor]);
        if (byte != '\n' &&
            !(byte == '\r' && cursor + 1U < text.size() && text[cursor + 1U] == '\n') &&
            line_bytes >= limits_.max_line_bytes) {
            report_limit("line size", {{cursor - line_bytes, line, 1}, {cursor, line, column}});
            return;
        }
        if (byte == 0U) {
            diagnostics_.error("GDS1009", "NUL byte is not valid in GDScript source",
                               {{cursor, line, column}, {cursor + 1U, line, column + 1U}});
            ++cursor;
            ++column;
            ++line_bytes;
            continue;
        }
        if (byte < 0x80U) {
            ++cursor;
            if (byte == '\n') {
                ++line;
                column = 1;
                line_bytes = 0;
            } else {
                ++column;
                if (!(byte == '\r' && cursor < text.size() && text[cursor] == '\n'))
                    ++line_bytes;
            }
            continue;
        }
        const auto decoded = decode_utf8(text, cursor);
        if (!decoded) {
            diagnostics_.error("GDS1009", "invalid UTF-8 sequence in GDScript source",
                               {{cursor, line, column}, {cursor + 1U, line, column + 1U}});
            ++cursor;
            ++column;
            ++line_bytes;
            continue;
        }
        if (decoded->length > limits_.max_line_bytes - line_bytes) {
            report_limit("line size", {{cursor - line_bytes, line, 1}, {cursor, line, column}});
            return;
        }
        cursor += decoded->length;
        ++column;
        line_bytes += decoded->length;
    }
}

void Lexer::emit(TokenKind kind, SourceLocation begin, std::optional<std::string> lexeme) {
    if (kind != TokenKind::end_of_file && tokens_.size() >= limits_.max_tokens) {
        report_limit("token count", {begin, location()});
        return;
    }
    const auto end = location();
    if (!lexeme.has_value()) {
        lexeme = std::string{source_.text().substr(begin.offset, end.offset - begin.offset)};
    }
    tokens_.push_back({kind, std::move(*lexeme), {begin, end}});
}

void Lexer::scan_indentation() {
    const auto begin = location();
    std::size_t width = 0;
    char current_indentation_character = '\0';
    bool mixed_indentation = false;
    while (peek() == ' ' || peek() == '\t') {
        const auto character = advance();
        if (current_indentation_character == '\0') {
            current_indentation_character = character;
        } else if (character != current_indentation_character) {
            mixed_indentation = true;
        }
        if (character == '\t') {
            width += 4;
        } else {
            ++width;
        }
    }

    if (peek() == '\n' || peek() == '\r' || peek() == '#' || at_end()) {
        return;
    }

    at_line_start_ = false;
    if (mixed_indentation) {
        diagnostics_.error("GDS1001", "mixed use of tabs and spaces for indentation",
                           {begin, location()});
    }
    if (current_indentation_character != '\0') {
        if (indentation_character_ == '\0') {
            indentation_character_ = current_indentation_character;
        } else if (current_indentation_character != indentation_character_) {
            diagnostics_.error(
                "GDS1001", "indentation character differs from the one used earlier in the file",
                {begin, location()});
        }
    }
    if (width > indent_stack_.back()) {
        if (indent_stack_.size() - 1U >= limits_.max_indentation_depth) {
            report_limit("indentation depth", {begin, location()});
            return;
        }
        indent_stack_.push_back(width);
        emit(TokenKind::indent, begin, std::to_string(width));
        return;
    }
    while (width < indent_stack_.back()) {
        indent_stack_.pop_back();
        emit(TokenKind::dedent, begin, std::to_string(width));
    }
    if (width != indent_stack_.back()) {
        diagnostics_.error("GDS1001", "indentation does not match an outer block",
                           {begin, location()});
    }
}

void Lexer::scan_number(SourceLocation begin) {
    if (!literal_within_limit(begin))
        return;
    bool valid = true;
    const auto report = [&](const char* message) {
        valid = false;
        diagnostics_.error("GDS1006", message, {begin, location()});
    };
    const auto scan_digits = [&](const auto& is_digit, const char* message,
                                 bool saw_digit = false) {
        bool previous_separator = false;
        while (is_digit(peek()) || peek() == '_') {
            if (peek() == '_') {
                if (!saw_digit || previous_separator)
                    report(message);
                previous_separator = true;
            } else {
                saw_digit = true;
                previous_separator = false;
            }
            advance();
            if (!literal_within_limit(begin))
                return saw_digit;
        }
        // Godot permits one trailing separator in every numeric digit segment (`1_`,
        // `0xff_`, `.5_`, `1e3_`) but still rejects a leading or repeated separator.
        if (!saw_digit)
            report(message);
        return saw_digit;
    };

    const auto finish = [&](TokenKind kind) {
        if (halted_)
            return;
        const auto text = std::string_view{source_.text()};
        if (identifier_start_at(text, offset_) || identifier_continue_at(text, offset_)) {
            while (identifier_continue_at(text, offset_)) {
                const auto decoded = decode_utf8(text, offset_);
                for (std::size_t index = 0; index < decoded->length; ++index)
                    advance();
                if (!literal_within_limit(begin))
                    return;
            }
            report("invalid character in numeric literal");
        }

        const auto raw =
            std::string_view{source_.text()}.substr(begin.offset, offset_ - begin.offset);
        if (kind == TokenKind::floating) {
            auto parsed = analyze_floating_literal(raw);
            if (parsed.exponent_clamped)
                diagnostics_.warning("GDS1007", "floating exponent exceeds Godot's range",
                                     {begin, location()});
            emit(kind, begin, std::move(parsed.canonical));
            return;
        }

        const auto parsed = parse_integer_literal(raw);
        if (!parsed) {
            if (valid)
                report("integer literal is outside the supported 64-bit range");
            emit(kind, begin, std::string{raw});
            return;
        }
        const auto signed_max =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto decimal_minimum_magnitude = signed_max + 1U;
        const auto maximum = parsed->base == 10 ? decimal_minimum_magnitude : signed_max;
        if (parsed->magnitude > maximum)
            report("integer literal is outside the signed 64-bit range");
        emit(kind, begin, std::to_string(parsed->magnitude));
    };

    const bool leading_decimal_point = source_.text()[begin.offset] == '.';

    if (!leading_decimal_point && source_.text()[begin.offset] == '0' &&
        (peek() == 'o' || peek() == 'O')) {
        advance();
        if (!literal_within_limit(begin))
            return;
        const auto text = std::string_view{source_.text()};
        while (identifier_continue_at(text, offset_)) {
            const auto decoded = decode_utf8(text, offset_);
            for (std::size_t index = 0; index < decoded->length; ++index)
                advance();
            if (!literal_within_limit(begin))
                return;
        }
        report("octal integer notation is not part of GDScript");
        emit(TokenKind::integer, begin, "0");
        return;
    }
    if (!leading_decimal_point && source_.text()[begin.offset] == '0' &&
        (peek() == 'x' || peek() == 'X')) {
        advance();
        if (!literal_within_limit(begin))
            return;
        scan_digits(
            [](char value) { return std::isxdigit(static_cast<unsigned char>(value)) != 0; },
            "invalid hexadecimal integer literal");
        finish(TokenKind::integer);
        return;
    }
    if (!leading_decimal_point && source_.text()[begin.offset] == '0' &&
        (peek() == 'b' || peek() == 'B')) {
        advance();
        if (!literal_within_limit(begin))
            return;
        scan_digits([](char value) { return value == '0' || value == '1'; },
                    "invalid binary integer literal");
        finish(TokenKind::integer);
        return;
    }

    auto kind = leading_decimal_point ? TokenKind::floating : TokenKind::integer;
    if (leading_decimal_point) {
        scan_digits([](char value) { return std::isdigit(static_cast<unsigned char>(value)) != 0; },
                    "invalid fractional part");
    } else {
        scan_digits([](char value) { return std::isdigit(static_cast<unsigned char>(value)) != 0; },
                    "invalid numeric separator", true);

        // GDScript accepts both `1.0` and the compact trailing-dot form `1.`.
        if (peek() == '.') {
            kind = TokenKind::floating;
            advance();
            if (!literal_within_limit(begin))
                return;
            if (std::isdigit(static_cast<unsigned char>(peek())) != 0 || peek() == '_') {
                scan_digits(
                    [](char value) { return std::isdigit(static_cast<unsigned char>(value)) != 0; },
                    "invalid fractional part");
            }
        }
    }
    if (peek() == 'e' || peek() == 'E') {
        kind = TokenKind::floating;
        advance();
        if (!literal_within_limit(begin))
            return;
        if (peek() == '+' || peek() == '-') {
            advance();
            if (!literal_within_limit(begin))
                return;
        }
        scan_digits([](char value) { return std::isdigit(static_cast<unsigned char>(value)) != 0; },
                    "invalid exponent");
    }
    finish(kind);
}

void Lexer::scan_identifier(SourceLocation begin) {
    const auto text = std::string_view{source_.text()};
    while (identifier_continue_at(text, offset_)) {
        const auto decoded = decode_utf8(text, offset_);
        for (std::size_t index = 0; index < decoded->length; ++index)
            advance();
    }
    const auto lexeme =
        std::string_view{source_.text()}.substr(begin.offset, offset_ - begin.offset);
    const auto keyword = keywords.find(lexeme);
    const auto kind = keyword == keywords.end() ? TokenKind::identifier : keyword->second;
    emit(kind, begin);
    const auto contains_non_ascii = std::any_of(lexeme.begin(), lexeme.end(), [](const char byte) {
        return static_cast<unsigned char>(byte) >= 0x80U;
    });
    const auto codepoint_length = column_ - begin.column;
    if (kind == TokenKind::identifier && contains_non_ascii && codepoint_length >= 2U &&
        codepoint_length <= 10U) {
        static const auto keyword_skeletons = [] {
            std::vector<std::pair<std::string_view, std::string>> result;
            result.reserve(keywords.size());
            for (const auto& [spelling, ignored_kind] : keywords) {
                static_cast<void>(ignored_kind);
                const auto skeleton = unicode_confusable_skeleton(spelling);
                if (skeleton)
                    result.emplace_back(spelling, *skeleton);
            }
            std::sort(result.begin(), result.end(),
                      [](const auto& left, const auto& right) { return left.first < right.first; });
            return result;
        }();
        const auto skeleton = unicode_confusable_skeleton(lexeme);
        if (skeleton) {
            const auto match =
                std::find_if(keyword_skeletons.begin(), keyword_skeletons.end(),
                             [&](const auto& candidate) { return candidate.second == *skeleton; });
            if (match != keyword_skeletons.end()) {
                diagnostics_.error("GDS1020",
                                   "identifier '" + std::string{lexeme} +
                                       "' is visually confusable with GDScript keyword '" +
                                       std::string{match->first} + "'",
                                   {begin, location()});
            }
        }
    }
    if (kind == TokenKind::kw_func && grouping_depth_ > 0) {
        lambda_signature_in_group_ = true;
        lambda_signature_grouping_depth_ = grouping_depth_;
    }
}

void Lexer::scan_string(SourceLocation begin, char quote, TokenKind kind, bool raw,
                        bool triple_quoted) {
    if (!literal_within_limit(begin))
        return;
    std::string value;
    const auto at_closing_quote = [&]() {
        return peek() == quote && (!triple_quoted || (peek(1) == quote && peek(2) == quote));
    };
    const auto read_hexadecimal = [&](std::size_t digits,
                                      const char* message) -> std::optional<std::uint32_t> {
        std::uint32_t codepoint = 0;
        for (std::size_t index = 0; index < digits; ++index) {
            const auto digit = hexadecimal_digit(peek());
            if (digit < 0) {
                diagnostics_.error("GDS1003", message, {begin, location()});
                return std::nullopt;
            }
            codepoint = (codepoint << 4U) | static_cast<std::uint32_t>(digit);
            advance();
        }
        return codepoint;
    };

    while (!at_end() && !at_closing_quote()) {
        char current = advance();
        if (current == '\\') {
            if (at_end()) {
                break;
            }
            if (raw) {
                value.push_back('\\');
                value.push_back(advance());
                continue;
            }
            const char escaped = advance();
            switch (escaped) {
            case 'a':
                value.push_back('\a');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'v':
                value.push_back('\v');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case '\'':
                value.push_back('\'');
                break;
            case '"':
                value.push_back('"');
                break;
            case '\n':
                break;
            case '\r':
                if (peek() == '\n')
                    advance();
                break;
            case 'u': {
                auto codepoint = read_hexadecimal(4, "invalid UTF-16 escape sequence");
                if (!codepoint)
                    break;
                if (*codepoint >= 0xd800U && *codepoint <= 0xdbffU) {
                    if (peek() != '\\' || peek(1) != 'u') {
                        diagnostics_.error("GDS1003",
                                           "high UTF-16 surrogate requires a low surrogate",
                                           {begin, location()});
                        break;
                    }
                    advance();
                    advance();
                    const auto low = read_hexadecimal(4, "invalid UTF-16 low surrogate");
                    if (!low || *low < 0xdc00U || *low > 0xdfffU) {
                        diagnostics_.error("GDS1003", "invalid UTF-16 low surrogate",
                                           {begin, location()});
                        break;
                    }
                    *codepoint = 0x10000U + ((*codepoint - 0xd800U) << 10U) + (*low - 0xdc00U);
                } else if (*codepoint >= 0xdc00U && *codepoint <= 0xdfffU) {
                    diagnostics_.error("GDS1003", "unexpected UTF-16 low surrogate",
                                       {begin, location()});
                    break;
                }
                if (*codepoint == 0U) {
                    diagnostics_.warning(
                        "GDS1008", "NUL escape is replaced with U+FFFD to match Godot strings",
                        {begin, location()});
                    *codepoint = 0xfffdU;
                }
                if (!append_utf8(value, *codepoint))
                    diagnostics_.error("GDS1003", "invalid Unicode codepoint", {begin, location()});
                break;
            }
            case 'U': {
                auto codepoint = read_hexadecimal(6, "invalid UTF-32 escape sequence");
                if (codepoint && *codepoint == 0U) {
                    diagnostics_.warning(
                        "GDS1008", "NUL escape is replaced with U+FFFD to match Godot strings",
                        {begin, location()});
                    *codepoint = 0xfffdU;
                }
                if (codepoint && !append_utf8(value, *codepoint))
                    diagnostics_.error("GDS1003", "invalid Unicode codepoint", {begin, location()});
                break;
            }
            default:
                diagnostics_.error("GDS1003", "invalid escape sequence", {begin, location()});
                value.push_back(escaped);
                break;
            }
        } else {
            value.push_back(current);
        }
        if (!literal_within_limit(begin))
            return;
    }
    if (at_end() || !at_closing_quote()) {
        diagnostics_.error("GDS1002", "unterminated string literal", {begin, location()});
        return;
    }
    advance();
    if (triple_quoted) {
        advance();
        advance();
    }
    if (!literal_within_limit(begin))
        return;
    emit(kind, begin, std::move(value));
}

void Lexer::scan_node_reference(SourceLocation begin, char prefix) {
    if (!literal_within_limit(begin))
        return;
    std::string value(1, prefix);
    const auto scan_segment = [&]() {
        if (peek() == '%')
            value.push_back(advance());
        if (!literal_within_limit(begin))
            return false;

        if (peek() == '\'' || peek() == '"') {
            const char quote = advance();
            bool has_content = false;
            while (!at_end() && peek() != quote && peek() != '\n') {
                value.push_back(advance());
                has_content = true;
                if (!literal_within_limit(begin))
                    return false;
            }
            if (!match(quote)) {
                diagnostics_.error("GDS1002", "unterminated node path", {begin, location()});
                return false;
            }
            return has_content;
        }

        if (prefix == '$' && peek() == '.' && peek(1) == '.') {
            value.push_back(advance());
            value.push_back(advance());
            return literal_within_limit(begin);
        }

        const auto text = std::string_view{source_.text()};
        if (!identifier_start_at(text, offset_))
            return false;
        do {
            const auto decoded = decode_utf8(text, offset_);
            for (std::size_t index = 0; index < decoded->length; ++index)
                value.push_back(advance());
            if (!literal_within_limit(begin))
                return false;
        } while (identifier_continue_at(text, offset_));
        return true;
    };

    bool valid = scan_segment();
    while (valid && match('/')) {
        value.push_back('/');
        valid = scan_segment();
    }
    if (!valid)
        diagnostics_.error("GDS1005", "expected a node path segment after node shorthand",
                           {begin, location()});
    emit(TokenKind::node_reference, begin, std::move(value));
}

void Lexer::finish_lambda_block(const SourceLocation delimiter) {
    if (lambda_blocks_.empty())
        return;
    const auto context = lambda_blocks_.back();
    if (tokens_.empty() || tokens_.back().kind != TokenKind::newline)
        emit(TokenKind::newline, delimiter, "\\n");
    while (indent_stack_.back() > context.indentation_base) {
        indent_stack_.pop_back();
        emit(TokenKind::dedent, delimiter, std::to_string(context.indentation_base));
    }
    lambda_blocks_.pop_back();
}

bool Lexer::lambda_layout_active() const noexcept {
    return !lambda_blocks_.empty() && grouping_depth_ == lambda_blocks_.back().grouping_depth;
}

std::vector<Token> Lexer::scan() {
    validate_utf8();
    if (halted_) {
        emit_eof();
        return tokens_;
    }
    if (source_.text().size() >= 3 && static_cast<unsigned char>(source_.text()[0]) == 0xefU &&
        static_cast<unsigned char>(source_.text()[1]) == 0xbbU &&
        static_cast<unsigned char>(source_.text()[2]) == 0xbfU) {
        // Preserve byte offsets while treating an optional UTF-8 BOM as transport metadata.
        offset_ = 3;
    }
    while (!at_end() && !halted_) {
        if (at_line_start_ && (grouping_depth_ == 0 || lambda_layout_active())) {
            scan_indentation();
            if (halted_)
                break;
            while (!lambda_blocks_.empty() && !at_line_start_ &&
                   indent_stack_.back() <= lambda_blocks_.back().indentation_base)
                lambda_blocks_.pop_back();
            if (at_end()) {
                break;
            }
        }

        const auto begin = location();
        const auto text = std::string_view{source_.text()};
        if (static_cast<unsigned char>(peek()) >= 0x80U) {
            const auto decoded = decode_utf8(text, offset_);
            if (!decoded) {
                advance();
                continue;
            }
            if (!is_unicode_identifier_start(decoded->codepoint)) {
                for (std::size_t index = 0; index < decoded->length; ++index)
                    advance();
                diagnostics_.error("GDS1005", "Unicode character cannot start an identifier",
                                   {begin, location()});
                continue;
            }
            for (std::size_t index = 0; index < decoded->length; ++index)
                advance();
            if (explicit_line_continuation_)
                explicit_line_continuation_ = false;
            scan_identifier(begin);
            continue;
        }
        const char value = advance();
        if (explicit_line_continuation_ && value != ' ' && value != '\t' && value != '\r' &&
            value != '\n' && value != '#')
            explicit_line_continuation_ = false;
        switch (value) {
        case ' ':
        case '\t':
            break;
        case '\r':
            if (peek() != '\n') {
                diagnostics_.error("GDS1005", "stray carriage return character in source code",
                                   {begin, location()});
            }
            break;
        case '#':
            while (!at_end() && peek() != '\n') {
                advance();
            }
            break;
        case '\n':
            if (!explicit_line_continuation_ && (grouping_depth_ == 0 || lambda_layout_active())) {
                if (!at_line_start_)
                    emit(TokenKind::newline, begin, "\\n");
                at_line_start_ = true;
            }
            break;
        case '(':
            if (grouping_depth_ >= limits_.max_grouping_depth) {
                report_limit("grouping depth", {begin, location()});
                break;
            }
            ++grouping_depth_;
            emit(TokenKind::left_paren, begin);
            break;
        case ')':
            if (lambda_layout_active())
                finish_lambda_block(begin);
            if (grouping_depth_ > 0)
                --grouping_depth_;
            emit(TokenKind::right_paren, begin);
            break;
        case '[':
            if (grouping_depth_ >= limits_.max_grouping_depth) {
                report_limit("grouping depth", {begin, location()});
                break;
            }
            ++grouping_depth_;
            emit(TokenKind::left_bracket, begin);
            break;
        case ']':
            if (lambda_layout_active())
                finish_lambda_block(begin);
            if (grouping_depth_ > 0)
                --grouping_depth_;
            emit(TokenKind::right_bracket, begin);
            break;
        case '{':
            if (grouping_depth_ >= limits_.max_grouping_depth) {
                report_limit("grouping depth", {begin, location()});
                break;
            }
            ++grouping_depth_;
            emit(TokenKind::left_brace, begin);
            break;
        case '}':
            if (lambda_layout_active())
                finish_lambda_block(begin);
            if (grouping_depth_ > 0)
                --grouping_depth_;
            emit(TokenKind::right_brace, begin);
            break;
        case '@':
            emit(TokenKind::at_sign, begin);
            break;
        case ',':
            if (lambda_layout_active())
                finish_lambda_block(begin);
            emit(TokenKind::comma, begin);
            break;
        case '.':
            if (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                scan_number(begin);
            } else if (match('.')) {
                emit(match('.') ? TokenKind::ellipsis : TokenKind::dot_dot, begin);
            } else {
                emit(TokenKind::dot, begin);
            }
            break;
        case ':':
            if (match('=')) {
                emit(TokenKind::colon_equal, begin);
            } else {
                emit(TokenKind::colon, begin);
                std::size_t lookahead = 0;
                while (peek(lookahead) == ' ' || peek(lookahead) == '\t' ||
                       peek(lookahead) == '\r') {
                    ++lookahead;
                }
                if (lambda_signature_in_group_ &&
                    grouping_depth_ == lambda_signature_grouping_depth_) {
                    lambda_signature_in_group_ = false;
                    if (peek(lookahead) == '\n') {
                        lambda_blocks_.push_back({indent_stack_.back(), grouping_depth_});
                    }
                }
            }
            break;
        case ';':
            emit(TokenKind::semicolon, begin);
            break;
        case '+':
            emit(match('=') ? TokenKind::plus_equal : TokenKind::plus, begin);
            break;
        case '-':
            if (match('>'))
                emit(TokenKind::arrow, begin);
            else
                emit(match('=') ? TokenKind::minus_equal : TokenKind::minus, begin);
            break;
        case '*':
            if (match('*'))
                emit(match('=') ? TokenKind::power_equal : TokenKind::power, begin);
            else
                emit(match('=') ? TokenKind::star_equal : TokenKind::star, begin);
            break;
        case '/':
            emit(match('=') ? TokenKind::slash_equal : TokenKind::slash, begin);
            break;
        case '%':
            if (match('=')) {
                emit(TokenKind::percent_equal, begin);
            } else if ((identifier_start_at(text, offset_) || peek() == '\'' || peek() == '"') &&
                       (tokens_.empty() || !can_end_expression(tokens_.back().kind))) {
                scan_node_reference(begin, '%');
            } else {
                emit(TokenKind::percent, begin);
            }
            break;
        case '=':
            emit(match('=') ? TokenKind::equal_equal : TokenKind::equal, begin);
            break;
        case '!':
            if (match('='))
                emit(TokenKind::bang_equal, begin);
            else
                emit(TokenKind::kw_not, begin, "not");
            break;
        case '<':
            if (match('='))
                emit(TokenKind::less_equal, begin);
            else if (match('<'))
                emit(match('=') ? TokenKind::shift_left_equal : TokenKind::shift_left, begin);
            else
                emit(TokenKind::less, begin);
            break;
        case '>':
            if (match('='))
                emit(TokenKind::greater_equal, begin);
            else if (match('>'))
                emit(match('=') ? TokenKind::shift_right_equal : TokenKind::shift_right, begin);
            else
                emit(TokenKind::greater, begin);
            break;
        case '&':
            if (peek() == '\'' || peek() == '"') {
                const char quote = advance();
                const bool triple_quoted = peek() == quote && peek(1) == quote;
                if (triple_quoted) {
                    advance();
                    advance();
                }
                scan_string(begin, quote, TokenKind::string_name, false, triple_quoted);
            } else if (match('&')) {
                emit(TokenKind::kw_and, begin, "and");
            } else {
                emit(match('=') ? TokenKind::ampersand_equal : TokenKind::ampersand, begin);
            }
            break;
        case '|':
            if (match('|'))
                emit(TokenKind::kw_or, begin, "or");
            else
                emit(match('=') ? TokenKind::pipe_equal : TokenKind::pipe, begin);
            break;
        case '^':
            if (peek() == '\'' || peek() == '"') {
                const char quote = advance();
                const bool triple_quoted = peek() == quote && peek(1) == quote;
                if (triple_quoted) {
                    advance();
                    advance();
                }
                scan_string(begin, quote, TokenKind::node_path, false, triple_quoted);
            } else {
                emit(match('=') ? TokenKind::caret_equal : TokenKind::caret, begin);
            }
            break;
        case '~':
            emit(TokenKind::tilde, begin);
            break;
        case '\'':
        case '"': {
            const bool triple_quoted = peek() == value && peek(1) == value;
            if (triple_quoted) {
                advance();
                advance();
            }
            scan_string(begin, value, TokenKind::string, false, triple_quoted);
            break;
        }
        case '$':
            scan_node_reference(begin, '$');
            break;
        case '\\':
            if (peek() == '\r' && peek(1) == '\n')
                advance();
            if (match('\n')) {
                at_line_start_ = false;
                explicit_line_continuation_ = true;
            } else {
                diagnostics_.error("GDS1004", "unexpected backslash", {begin, location()});
            }
            break;
        default:
            if (std::isdigit(static_cast<unsigned char>(value)) != 0) {
                scan_number(begin);
            } else if (value == 'r' && (peek() == '\'' || peek() == '"')) {
                const char quote = advance();
                const bool triple_quoted = peek() == quote && peek(1) == quote;
                if (triple_quoted) {
                    advance();
                    advance();
                }
                scan_string(begin, quote, TokenKind::string, true, triple_quoted);
            } else if (is_ascii_identifier_start(value)) {
                scan_identifier(begin);
            } else {
                diagnostics_.error("GDS1005", "unexpected character", {begin, location()});
            }
            break;
        }
    }

    if (halted_) {
        emit_eof();
        return tokens_;
    }
    const auto eof = location();
    if (!tokens_.empty() && tokens_.back().kind != TokenKind::newline && grouping_depth_ == 0) {
        emit(TokenKind::newline, eof, "\\n");
    }
    while (indent_stack_.size() > 1) {
        indent_stack_.pop_back();
        emit(TokenKind::dedent, eof, "0");
    }
    emit(TokenKind::end_of_file, eof, "");
    return tokens_;
}

} // namespace gdpp
