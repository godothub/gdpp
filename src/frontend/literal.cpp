#include "gdpp/frontend/literal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace gdpp {
namespace {

struct ParsedGodotFloat {
    double value{0.0};
    bool exponent_clamped{false};
    bool nonzero_mantissa{false};
    bool valid{true};
};

// GDScript literal values are produced by Godot's deterministic built_in_strtod algorithm, not by
// the host C library. Preserve its observable 18-digit mantissa and power-table behavior so AOT
// output cannot change at overflow, underflow or rounding boundaries on another toolchain.
ParsedGodotFloat parse_like_godot(std::string_view normalized) {
    constexpr int maximum_exponent = 511;
    constexpr double powers_of_ten[]{10.0,   100.0,  1.0e4,   1.0e8,  1.0e16,
                                     1.0e32, 1.0e64, 1.0e128, 1.0e256};

    const auto exponent_marker = normalized.find_first_of("eE");
    if (normalized.empty() ||
        (exponent_marker != std::string_view::npos &&
         normalized.find_first_of("eE", exponent_marker + 1U) != std::string_view::npos)) {
        return {0.0, false, false, false};
    }
    const auto mantissa = normalized.substr(0, exponent_marker);
    const auto decimal_point = mantissa.find('.');
    if (mantissa.empty() || (decimal_point != std::string_view::npos &&
                             mantissa.find('.', decimal_point + 1U) != std::string_view::npos)) {
        return {0.0, false, false, false};
    }
    const auto digits_before_point =
        decimal_point == std::string_view::npos ? mantissa.size() : decimal_point;
    const auto digit_count = mantissa.size() - (decimal_point == std::string_view::npos ? 0U : 1U);
    const auto retained_digits = std::min<std::size_t>(digit_count, 18U);
    const auto first_count = retained_digits > 9U ? retained_digits - 9U : 0U;

    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::size_t consumed = 0;
    bool nonzero_mantissa = false;
    for (const char character : mantissa) {
        if (character == '.')
            continue;
        if (character < '0' || character > '9')
            return {0.0, false, false, false};
        nonzero_mantissa = nonzero_mantissa || character != '0';
        if (consumed < first_count) {
            first = 10U * first + static_cast<std::uint32_t>(character - '0');
        } else if (consumed < retained_digits) {
            second = 10U * second + static_cast<std::uint32_t>(character - '0');
        }
        ++consumed;
    }
    if (consumed == 0)
        return {0.0, false, false, false};
    double fraction = (1.0e9 * static_cast<double>(first)) + static_cast<double>(second);

    long long fractional_exponent = 0;
    if (digit_count > 18U) {
        fractional_exponent =
            static_cast<long long>(std::min<std::size_t>(digits_before_point, 1000000000U)) - 18LL;
    } else {
        fractional_exponent =
            static_cast<long long>(digits_before_point) - static_cast<long long>(digit_count);
    }

    long long explicit_exponent = 0;
    bool negative_exponent = false;
    if (exponent_marker != std::string_view::npos) {
        std::size_t cursor = exponent_marker + 1U;
        if (cursor < normalized.size() &&
            (normalized[cursor] == '+' || normalized[cursor] == '-')) {
            negative_exponent = normalized[cursor] == '-';
            ++cursor;
        }
        if (cursor == normalized.size())
            return {0.0, false, false, false};
        for (; cursor < normalized.size(); ++cursor) {
            const auto character = normalized[cursor];
            if (character < '0' || character > '9')
                return {0.0, false, false, false};
            const auto digit = static_cast<long long>(character - '0');
            constexpr auto exponent_limit = 1000000000LL;
            explicit_exponent = explicit_exponent > (exponent_limit - digit) / 10LL
                                    ? exponent_limit
                                    : explicit_exponent * 10LL + digit;
        }
    }

    const auto combined_exponent =
        fractional_exponent + (negative_exponent ? -explicit_exponent : explicit_exponent);
    const bool exponent_negative = combined_exponent < 0;
    auto magnitude =
        static_cast<unsigned long long>(exponent_negative ? -combined_exponent : combined_exponent);
    const bool exponent_clamped = magnitude > static_cast<unsigned long long>(maximum_exponent);
    magnitude = std::min(magnitude, static_cast<unsigned long long>(maximum_exponent));

    double scale = 1.0;
    std::size_t power = 0;
    while (magnitude != 0U) {
        if ((magnitude & 1U) != 0U)
            scale *= powers_of_ten[power];
        magnitude >>= 1U;
        ++power;
    }
    fraction = exponent_negative ? fraction / scale : fraction * scale;
    return {fraction, exponent_clamped, nonzero_mantissa, true};
}

std::string canonical_float(double value, std::string normalized) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return "inf";
    if (value == 0.0)
        return "0.0";

    // Floating-point from_chars is unavailable on the advertised macOS 11 deployment target.
    // classic() is deterministic and does not mutate Godot's process-wide C locale.
    std::istringstream input{normalized};
    input.imbue(std::locale::classic());
    double standard_value = 0.0;
    input >> standard_value;
    if (standard_value == value)
        return normalized;

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    auto canonical = output.str();
    if (canonical.find_first_of(".eE") == std::string::npos)
        canonical += ".0";
    return canonical;
}

} // namespace

std::optional<IntegerLiteralValue> parse_integer_literal(std::string_view text) noexcept {
    std::string normalized{text};
    normalized.erase(std::remove(normalized.begin(), normalized.end(), '_'), normalized.end());

    unsigned base = 10;
    std::size_t prefix = 0;
    if (normalized.size() >= 2 && normalized[0] == '0') {
        if (normalized[1] == 'x' || normalized[1] == 'X') {
            base = 16;
            prefix = 2;
        } else if (normalized[1] == 'b' || normalized[1] == 'B') {
            base = 2;
            prefix = 2;
        }
    }
    if (prefix == normalized.size())
        return std::nullopt;

    std::uint64_t magnitude = 0;
    const auto* begin = normalized.data() + prefix;
    const auto* end = normalized.data() + normalized.size();
    const auto parsed = std::from_chars(begin, end, magnitude, static_cast<int>(base));
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::nullopt;
    return IntegerLiteralValue{magnitude, base};
}

FloatingLiteralValue analyze_floating_literal(std::string_view text) {
    std::string normalized{text};
    normalized.erase(std::remove(normalized.begin(), normalized.end(), '_'), normalized.end());
    if (!normalized.empty() && normalized.front() == '.')
        normalized.insert(normalized.begin(), '0');

    const auto point = normalized.find('.');
    if (point != std::string::npos &&
        (point + 1 == normalized.size() || normalized[point + 1] == 'e' ||
         normalized[point + 1] == 'E')) {
        normalized.insert(point + 1, 1, '0');
    }

    const auto parsed = parse_like_godot(normalized);
    if (!parsed.valid)
        return {std::move(normalized), FloatingLiteralRange::invalid, false};
    auto range = FloatingLiteralRange::finite;
    if (std::isnan(parsed.value))
        range = FloatingLiteralRange::not_a_number;
    else if (std::isinf(parsed.value))
        range = FloatingLiteralRange::overflow;
    else if (parsed.value == 0.0 && parsed.nonzero_mantissa)
        range = FloatingLiteralRange::underflow;
    return {canonical_float(parsed.value, std::move(normalized)), range, parsed.exponent_clamped};
}

} // namespace gdpp
