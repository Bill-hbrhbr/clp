#include "Utils.hpp"

#include <boost/filesystem.hpp>
#include <clp/string_utils/string_utils.hpp>
#include <spdlog/spdlog.h>

namespace clp_s {
using std::string;
using std::string_view;
using clp::string_utils::is_alphabet;
using clp::string_utils::is_decimal_digit;
using clp::string_utils::is_delim;

bool FileUtils::find_all_files(std::string const& path, std::vector<std::string>& file_paths) {
    try {
        if (false == boost::filesystem::is_directory(path)) {
            // path is a file
            file_paths.push_back(path);
            return true;
        }

        if (boost::filesystem::is_empty(path)) {
            // path is an empty directory
            return true;
        }

        // Iterate directory
        boost::filesystem::recursive_directory_iterator iter(
                path,
                boost::filesystem::directory_options::follow_directory_symlink
        );
        boost::filesystem::recursive_directory_iterator end;
        for (; iter != end; ++iter) {
            // Check if current entry is an empty directory or a file
            if (boost::filesystem::is_directory(iter->path())) {
                if (boost::filesystem::is_empty(iter->path())) {
                    iter.disable_recursion_pending();
                }
            } else {
                file_paths.push_back(iter->path().string());
            }
        }
    } catch (boost::filesystem::filesystem_error& exception) {
        SPDLOG_ERROR(
                "Failed to find files/directories at '{}' - {}.",
                path.c_str(),
                exception.what()
        );
        return false;
    }

    return true;
}

bool FileUtils::validate_path(std::vector<std::string> const& paths) {
    bool all_paths_exist = true;
    for (auto const& path : paths) {
        if (false == boost::filesystem::exists(path)) {
            SPDLOG_ERROR("'{}' does not exist.", path.c_str());
            all_paths_exist = false;
        }
    }

    return all_paths_exist;
}

bool StringUtils::get_bounds_of_next_var(string const& msg, size_t& begin_pos, size_t& end_pos) {
    auto const msg_length = msg.length();
    if (end_pos >= msg_length) {
        return false;
    }

    while (true) {
        begin_pos = end_pos;
        // Find next non-delimiter
        for (; begin_pos < msg_length; ++begin_pos) {
            if (false == is_delim(msg[begin_pos])) {
                break;
            }
        }
        if (msg_length == begin_pos) {
            // Early exit for performance
            return false;
        }

        bool contains_decimal_digit = false;
        bool contains_alphabet = false;

        // Find next delimiter
        end_pos = begin_pos;
        for (; end_pos < msg_length; ++end_pos) {
            char c = msg[end_pos];
            if (is_decimal_digit(c)) {
                contains_decimal_digit = true;
            } else if (is_alphabet(c)) {
                contains_alphabet = true;
            } else if (is_delim(c)) {
                break;
            }
        }

        // Treat token as variable if:
        // - it contains a decimal digit, or
        // - it's directly preceded by an equals sign and contains an alphabet, or
        // - it could be a multi-digit hex value
        if (contains_decimal_digit
            || (begin_pos > 0 && '=' == msg[begin_pos - 1] && contains_alphabet)
            || could_be_multi_digit_hex_value(msg, begin_pos, end_pos))
        {
            break;
        }
    }

    return (msg_length != begin_pos);
}

bool StringUtils::has_unescaped_wildcards(std::string const& str) {
    for (size_t i = 0; i < str.size(); ++i) {
        if ('*' == str[i] || '?' == str[i]) {
            return true;
        }
        if ('\\' == str[i]) {
            ++i;
        }
    }
    return false;
}

bool StringUtils::advance_tame_to_next_match(
        char const*& tame_current,
        char const*& tame_bookmark,
        char const* tame_end,
        char const*& wild_current,
        char const*& wild_bookmark
) {
    auto w = *wild_current;
    if ('?' != w) {
        // No need to check for '*' since the caller ensures wild doesn't
        // contain consecutive '*'

        // Handle escaped characters
        if ('\\' == w) {
            ++wild_current;
            // This is safe without a bounds check since this the caller
            // ensures there are no dangling escape characters
            w = *wild_current;
        }

        // Advance tame_current until it matches wild_current
        while (true) {
            if (tame_end == tame_current) {
                // Wild group is longer than last group in tame, so
                // can't match
                // e.g. "*abc" doesn't match "zab"
                return false;
            }
            auto t = *tame_current;
            if (t == w) {
                break;
            }
            ++tame_current;
        }
    }

    tame_bookmark = tame_current;

    return true;
}

bool StringUtils::convert_string_to_double(std::string const& raw, double& converted) {
    if (raw.empty()) {
        // Can't convert an empty string
        return false;
    }

    char const* c_str = raw.c_str();
    char* end_ptr;
    // Reset errno so we can detect a new error
    errno = 0;
    double raw_as_double = strtod(c_str, &end_ptr);
    if (ERANGE == errno || (end_ptr - c_str) < raw.length()) {
        return false;
    }
    converted = raw_as_double;
    return true;
}
}  // namespace clp_s
