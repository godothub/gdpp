#include "gdpp/frontend/token.hpp"

namespace gdpp {

const char* token_kind_name(TokenKind kind) noexcept {
#define GDPP_TOKEN_NAME(name)                                                                      \
    case TokenKind::name:                                                                          \
        return #name
    switch (kind) {
        GDPP_TOKEN_NAME(end_of_file);
        GDPP_TOKEN_NAME(newline);
        GDPP_TOKEN_NAME(indent);
        GDPP_TOKEN_NAME(dedent);
        GDPP_TOKEN_NAME(identifier);
        GDPP_TOKEN_NAME(integer);
        GDPP_TOKEN_NAME(floating);
        GDPP_TOKEN_NAME(string);
        GDPP_TOKEN_NAME(string_name);
        GDPP_TOKEN_NAME(node_path);
        GDPP_TOKEN_NAME(node_reference);
        GDPP_TOKEN_NAME(kw_extends);
        GDPP_TOKEN_NAME(kw_class_name);
        GDPP_TOKEN_NAME(kw_class);
        GDPP_TOKEN_NAME(kw_func);
        GDPP_TOKEN_NAME(kw_var);
        GDPP_TOKEN_NAME(kw_const);
        GDPP_TOKEN_NAME(kw_signal);
        GDPP_TOKEN_NAME(kw_enum);
        GDPP_TOKEN_NAME(kw_static);
        GDPP_TOKEN_NAME(kw_return);
        GDPP_TOKEN_NAME(kw_assert);
        GDPP_TOKEN_NAME(kw_breakpoint);
        GDPP_TOKEN_NAME(kw_pass);
        GDPP_TOKEN_NAME(kw_if);
        GDPP_TOKEN_NAME(kw_match);
        GDPP_TOKEN_NAME(kw_when);
        GDPP_TOKEN_NAME(kw_elif);
        GDPP_TOKEN_NAME(kw_else);
        GDPP_TOKEN_NAME(kw_while);
        GDPP_TOKEN_NAME(kw_for);
        GDPP_TOKEN_NAME(kw_in);
        GDPP_TOKEN_NAME(kw_break);
        GDPP_TOKEN_NAME(kw_continue);
        GDPP_TOKEN_NAME(kw_true);
        GDPP_TOKEN_NAME(kw_false);
        GDPP_TOKEN_NAME(kw_null);
        GDPP_TOKEN_NAME(kw_self);
        GDPP_TOKEN_NAME(kw_and);
        GDPP_TOKEN_NAME(kw_or);
        GDPP_TOKEN_NAME(kw_not);
        GDPP_TOKEN_NAME(kw_as);
        GDPP_TOKEN_NAME(kw_is);
        GDPP_TOKEN_NAME(kw_await);
        GDPP_TOKEN_NAME(left_paren);
        GDPP_TOKEN_NAME(right_paren);
        GDPP_TOKEN_NAME(left_bracket);
        GDPP_TOKEN_NAME(right_bracket);
        GDPP_TOKEN_NAME(left_brace);
        GDPP_TOKEN_NAME(right_brace);
        GDPP_TOKEN_NAME(at_sign);
        GDPP_TOKEN_NAME(comma);
        GDPP_TOKEN_NAME(dot);
        GDPP_TOKEN_NAME(dot_dot);
        GDPP_TOKEN_NAME(ellipsis);
        GDPP_TOKEN_NAME(colon);
        GDPP_TOKEN_NAME(colon_equal);
        GDPP_TOKEN_NAME(semicolon);
        GDPP_TOKEN_NAME(arrow);
        GDPP_TOKEN_NAME(plus);
        GDPP_TOKEN_NAME(minus);
        GDPP_TOKEN_NAME(star);
        GDPP_TOKEN_NAME(power);
        GDPP_TOKEN_NAME(slash);
        GDPP_TOKEN_NAME(percent);
        GDPP_TOKEN_NAME(equal);
        GDPP_TOKEN_NAME(plus_equal);
        GDPP_TOKEN_NAME(minus_equal);
        GDPP_TOKEN_NAME(star_equal);
        GDPP_TOKEN_NAME(power_equal);
        GDPP_TOKEN_NAME(slash_equal);
        GDPP_TOKEN_NAME(percent_equal);
        GDPP_TOKEN_NAME(equal_equal);
        GDPP_TOKEN_NAME(bang_equal);
        GDPP_TOKEN_NAME(less);
        GDPP_TOKEN_NAME(less_equal);
        GDPP_TOKEN_NAME(greater);
        GDPP_TOKEN_NAME(greater_equal);
        GDPP_TOKEN_NAME(ampersand);
        GDPP_TOKEN_NAME(ampersand_equal);
        GDPP_TOKEN_NAME(pipe);
        GDPP_TOKEN_NAME(pipe_equal);
        GDPP_TOKEN_NAME(caret);
        GDPP_TOKEN_NAME(caret_equal);
        GDPP_TOKEN_NAME(tilde);
        GDPP_TOKEN_NAME(shift_left);
        GDPP_TOKEN_NAME(shift_left_equal);
        GDPP_TOKEN_NAME(shift_right);
        GDPP_TOKEN_NAME(shift_right_equal);
    }
#undef GDPP_TOKEN_NAME
    return "unknown";
}

} // namespace gdpp
