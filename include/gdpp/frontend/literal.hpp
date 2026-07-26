#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gdpp {

// Numeric tokens retain source spans in the lexer, while their semantic value is normalized once
// through this module. Keeping base detection and separator removal here prevents enum folding and
// C++ emission from interpreting the same GDScript literal differently.
struct IntegerLiteralValue {
    std::uint64_t magnitude{0};
    unsigned base{10};
};

enum class FloatingLiteralRange { finite, overflow, underflow, not_a_number, invalid };

struct FloatingLiteralValue {
    std::string canonical;
    FloatingLiteralRange range{FloatingLiteralRange::finite};
    bool exponent_clamped{false};
};

[[nodiscard]] std::optional<IntegerLiteralValue>
parse_integer_literal(std::string_view text) noexcept;

[[nodiscard]] FloatingLiteralValue analyze_floating_literal(std::string_view text);

} // namespace gdpp
