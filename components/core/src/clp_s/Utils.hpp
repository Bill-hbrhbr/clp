#ifndef CLP_S_UTILS_HPP
#define CLP_S_UTILS_HPP

#include <charconv>
#include <cstring>
#include <string>

#include <boost/filesystem.hpp>

namespace clp_s {
class FileUtils {
public:
    /**
     * Find all files in a directory
     * @param path
     * @param file_paths
     * @return true if successful, false otherwise
     */
    static bool find_all_files(std::string const& path, std::vector<std::string>& file_paths);

    /**
     * Validate if all paths exist
     * @param paths
     * @return true if all paths exist, false otherwise
     */
    static bool validate_path(std::vector<std::string> const& paths);
};

class StringUtils {
public:
    /**
     * Checks if the string could be a hexadecimal value
     * @param str
     * @param begin_pos
     * @param end_pos
     * @return true if str could be a hexadecimal value, false otherwise
     */
    static inline bool
    could_be_multi_digit_hex_value(std::string const& str, size_t begin_pos, size_t end_pos) {
        if (end_pos - begin_pos < 2) {
            return false;
        }

        for (size_t i = begin_pos; i < end_pos; ++i) {
            auto c = str[i];
            if (false
                == (('a' <= c && c <= 'f') || ('A' <= c && c <= 'F') || ('0' <= c && c <= '9')))
            {
                return false;
            }
        }

        return true;
    }

    /**
     * Returns bounds of next variable in given string
     * A variable is a token (word between two delimiters) that contains numbers or is directly
     * preceded by an equals sign
     * @param msg
     * @param begin_pos Begin position of last variable, changes to begin position of next variable
     * @param end_pos End position of last variable, changes to end position of next variable
     * @return true if a variable was found, false otherwise
     */
    static bool get_bounds_of_next_var(std::string const& msg, size_t& begin_pos, size_t& end_pos);

    /**
     * Checks if the given string has unescaped wildcards
     * @param str
     * @return true if the string has unescaped wildcards, false otherwise
     */
    static bool has_unescaped_wildcards(std::string const& str);

    /**
     * Converts the given string to a double if possible
     * @param raw
     * @param converted
     * @return true if the conversion was successful, false otherwise
     */
    static bool convert_string_to_double(std::string const& raw, double& converted);

private:
    /**
     * Helper for ``wildcard_match_unsafe_case_sensitive`` to advance the
     * pointer in tame to the next character which matches wild. This method
     * should be inlined for performance.
     * @param tame_current
     * @param tame_bookmark
     * @param tame_end
     * @param wild_current
     * @param wild_bookmark
     * @return true on success, false if wild cannot match tame
     */
    static inline bool advance_tame_to_next_match(
            char const*& tame_current,
            char const*& tame_bookmark,
            char const* tame_end,
            char const*& wild_current,
            char const*& wild_bookmark
    );
};

enum EvaluatedValue {
    True,
    False,
    Unknown
};

template <class T2, class T1>
inline T2 bit_cast(T1 t1) {
    static_assert(sizeof(T1) == sizeof(T2), "Must match size");
    static_assert(std::is_standard_layout<T1>::value, "Need to be standard layout");
    static_assert(std::is_standard_layout<T2>::value, "Need to be standard layout");

    T2 t2;
    std::memcpy(std::addressof(t2), std::addressof(t1), sizeof(T1));
    return t2;
}

/**
 * A span of memory where the underlying memory may not be aligned correctly for type T.
 *
 * This class should be used whenever we need a view into some memory, and we do not know whether it
 * is aligned correctly for type T. If the alignment of the underlying memory is known std::span
 * should be used instead.
 *
 * In C++ creating a pointer to objects of type T that is not correctly aligned for type T is
 * undefined behaviour, as is dereferencing such a pointer. This class avoids this undefined
 * behaviour by using memcpy (which any modern compiler should be able to optimize away).
 *
 * For any modern x86 platform the performance difference between using std::span and
 * UnalignedMemSpan should be fairly minimal.
 *
 * @tparam T
 */
template <typename T>
class UnalignedMemSpan {
public:
    UnalignedMemSpan() = default;
    UnalignedMemSpan(char* begin, size_t size) : m_begin(begin), m_size(size) {};

    size_t size() { return m_size; }

    T operator[](size_t i) {
        T tmp;
        memcpy(&tmp, m_begin + i * sizeof(T), sizeof(T));
        return tmp;
    }

    UnalignedMemSpan<T> sub_span(size_t start, size_t size) {
        return {m_begin + start * sizeof(T), size};
    }

private:
    char* m_begin{nullptr};
    size_t m_size{0};
};
}  // namespace clp_s
#endif  // CLP_S_UTILS_HPP
