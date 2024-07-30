#include "RegexErrorCode.hpp"

#include <string>

#include <error_handling/ErrorCode.hpp>

template <>
auto clp::regex_utils::RegexErrorCategory::name() const noexcept -> char const* {
    return "regex utility";
}

template <>
auto clp::regex_utils::RegexErrorCategory::message(clp::regex_utils::RegexErrorEnum ev) const -> std::string {
    switch (ev) {
        case clp::regex_utils::RegexErrorEnum::Success:
            return "Success.";

        case clp::regex_utils::RegexErrorEnum::IllegalState:
            return "Unrecognized state.";

        case clp::regex_utils::RegexErrorEnum::Star:
            return "Failed to translate due to metachar `*` (zero or more occurences).";

        case clp::regex_utils::RegexErrorEnum::Plus:
            return "Failed to translate due to metachar `+` (one or more occurences).";

        case clp::regex_utils::RegexErrorEnum::Question:
            return "Currently does not support returning a list of wildcard translations. The "
                   "metachar `?` (lazy match) may be supported in the future.";

        default:
            return "(unrecognized error)";
    }
}
