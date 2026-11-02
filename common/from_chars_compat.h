#pragma once
// std::from_chars for floating point where the standard library lacks it (the NDK's libc++ deletes those overloads until LLVM 20).
// Same contract as std::from_chars: no leading whitespace or '+', no hex, ptr past the parsed text, errc::invalid_argument or
// errc::result_out_of_range on failure. The fallback parses with strtod, which is exact in the "C" numeric locale (bionic has no other).
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <system_error>
#include <type_traits>

namespace uc {
#if defined(_LIBCPP_VERSION) && !defined(__APPLE__) && _LIBCPP_VERSION < 200000
template<class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
inline std::from_chars_result from_chars(const char* first, const char* last, T& value) {
    if (first == last || *first == ' ' || *first == '\t' || *first == '\n' || *first == '+') return {first, std::errc::invalid_argument};
    const char* digits = *first == '-' ? first + 1 : first;
    if (last - digits >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {   // from_chars' general format: "0" then stop
        value = *first == '-' ? T(-0.0) : T(0);
        return {digits + 1, std::errc{}};
    }
    const std::string text(first, last);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return {first, std::errc::invalid_argument};
    const char* ptr = first + (end - text.c_str());
    if (errno == ERANGE || (std::is_same_v<T, float> && std::isfinite(parsed) && std::fabs(parsed) > 3.4028234663852886e38))
        return {ptr, std::errc::result_out_of_range};
    value = T(parsed);
    return {ptr, std::errc{}};
}
#else
template<class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
inline std::from_chars_result from_chars(const char* first, const char* last, T& value) { return std::from_chars(first, last, value); }
#endif
}  // namespace uc
