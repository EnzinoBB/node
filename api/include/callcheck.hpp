#ifndef CALLCHECK_HPP
#define CALLCHECK_HPP

#include <string>
#include <vector>

#include <general_types.h>

namespace api {

// Why the executor cannot find the method a call names, or an empty string when it may.
//
// The executor picks the method with commons-beanutils' getMatchingAccessibleMethod: same name,
// same number of arguments, and every argument's class (from its Variant: v_string is a String,
// v_big_decimal a BigDecimal, v_int an Integer, ...) assignable to the declared type, a wrapper
// also to its primitive. A call that matches no method fails on every node, and its fee is paid
// all the same. Only certain mismatches are reported: an argument or a declared type outside
// String, BigDecimal, boolean, int, long, double (and their wrappers) and byte[] is taken as a
// possible match.
std::string checkCallArguments(const std::vector<general::MethodDescription>& methods, const std::string& method,
                               const std::vector<general::Variant>& params);

}  // namespace api

#endif  // CALLCHECK_HPP
