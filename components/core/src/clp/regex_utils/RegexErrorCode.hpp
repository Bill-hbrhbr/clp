#ifndef CLP_REGEXERRORCODE_HPP
#define CLP_REGEXERRORCODE_HPP

#include <cstdint>
#include <string>
#include <system_error>

#include <error_handling/ErrorCode.hpp>
//#include "GenericErrorCode.hpp"

namespace clp::regex_utils {
enum class RegexErrorEnum : uint8_t {
    Success = 0,
    IllegalState,
    Star,
    Plus,
    Question,
    Pipe,
    Caret,
    Dollar,
    DisallowedEscapeSequence,
    UnmatchedParenthesis,
    UnsupportedCharsets,
    IncompleteCharsetStructure,
    UnsupportedQuantifier,
    TokenUnquantifiable,
};

//using RegexErrorCategory = clp::error_handling::ErrorCategory<RegexErrorEnum>;
//using RegexErrorCode = clp::error_handling::ErrorCode<RegexErrorEnum>;
using RegexErrorCategory = clp::error_handling::ErrorCategory<RegexErrorEnum>;
using RegexErrorCode = clp::error_handling::ErrorCode<RegexErrorEnum>;
}  // namespace clp::regex_utils

namespace std {
template <>
struct is_error_code_enum<clp::regex_utils::RegexErrorCode> : std::true_type {};
}  // namespace std

#endif  // CLP_REGEXERRORCODE_HPP
