#include "callcheck.hpp"

#include <map>
#include <sstream>

namespace {

// the class the executor gives an argument (ContractExecutorServiceUtils.getArgTypes), for the
// Variants whose class is certain; empty for the others
std::string argumentClass(const general::Variant& v) {
    if (v.__isset.v_string) return "java.lang.String";
    if (v.__isset.v_big_decimal) return "java.math.BigDecimal";
    if (v.__isset.v_boolean || v.__isset.v_boolean_box) return "java.lang.Boolean";
    if (v.__isset.v_int || v.__isset.v_int_box) return "java.lang.Integer";
    if (v.__isset.v_long || v.__isset.v_long_box) return "java.lang.Long";
    if (v.__isset.v_double || v.__isset.v_double_box) return "java.lang.Double";
    if (v.__isset.v_byte_array) return "byte[]";
    return {};
}

std::string variantName(const general::Variant& v) {
    if (v.__isset.v_string) return "v_string";
    if (v.__isset.v_big_decimal) return "v_big_decimal";
    if (v.__isset.v_boolean) return "v_boolean";
    if (v.__isset.v_boolean_box) return "v_boolean_box";
    if (v.__isset.v_int) return "v_int";
    if (v.__isset.v_int_box) return "v_int_box";
    if (v.__isset.v_long) return "v_long";
    if (v.__isset.v_long_box) return "v_long_box";
    if (v.__isset.v_double) return "v_double";
    if (v.__isset.v_double_box) return "v_double_box";
    if (v.__isset.v_byte_array) return "v_byte_array";
    return "another type";
}

// declared types whose assignability from the classes above is certain, with the class each accepts
const std::map<std::string, std::string>& knownTypes() {
    static const std::map<std::string, std::string> types = {
        {"java.lang.String", "java.lang.String"},
        {"java.math.BigDecimal", "java.math.BigDecimal"},
        {"java.lang.Boolean", "java.lang.Boolean"},
        {"boolean", "java.lang.Boolean"},
        {"java.lang.Integer", "java.lang.Integer"},
        {"int", "java.lang.Integer"},
        {"java.lang.Long", "java.lang.Long"},
        {"long", "java.lang.Long"},
        {"java.lang.Double", "java.lang.Double"},
        {"double", "java.lang.Double"},
        {"byte[]", "byte[]"},
    };
    return types;
}

// the Variant a wallet sends for a declared type
std::string variantFor(const std::string& type) {
    static const std::map<std::string, std::string> variants = {
        {"java.lang.String", "v_string"}, {"java.math.BigDecimal", "v_big_decimal"},
        {"java.lang.Boolean", "v_boolean_box"}, {"boolean", "v_boolean"},
        {"java.lang.Integer", "v_int_box"}, {"int", "v_int"},
        {"java.lang.Long", "v_long_box"}, {"long", "v_long"},
        {"java.lang.Double", "v_double_box"}, {"double", "v_double"},
        {"byte[]", "v_byte_array"},
    };
    const auto it = variants.find(type);
    return it == variants.end() ? std::string{} : it->second;
}

bool certainlyIncompatible(const std::string& declared, const general::Variant& argument) {
    const std::string actual = argumentClass(argument);
    const auto it = knownTypes().find(declared);
    return !actual.empty() && it != knownTypes().end() && it->second != actual;
}

std::string signature(const general::MethodDescription& m) {
    std::ostringstream os;
    os << m.name << '(';
    for (size_t i = 0; i < m.arguments.size(); ++i) {
        os << (i ? ", " : "") << m.arguments[i].type;
    }
    os << ')';
    return os.str();
}

}  // namespace

namespace api {

std::string checkCallArguments(const std::vector<general::MethodDescription>& methods, const std::string& method,
                               const std::vector<general::Variant>& params) {
    // the methods of Object are callable but not listed by the executor (getContractMethods)
    static const char* objectMethods[] = {"getClass", "hashCode", "equals", "toString", "notify", "notifyAll", "wait", "finalize"};
    for (const char* o : objectMethods) {
        if (method == o) {
            return {};
        }
    }

    std::vector<const general::MethodDescription*> named;
    for (const auto& m : methods) {
        if (m.name == method) {
            named.push_back(&m);
        }
    }
    if (named.empty()) {
        return "the contract has no public method " + method + "(): the call would fail and its fee be paid";
    }

    std::vector<const general::MethodDescription*> sameCount;
    for (const auto* m : named) {
        if (m->arguments.size() == params.size()) {
            sameCount.push_back(m);
        }
    }
    std::ostringstream os;
    if (sameCount.empty()) {
        os << method << " takes ";
        for (size_t i = 0; i < named.size(); ++i) {
            os << (i ? " or " : "") << named[i]->arguments.size();
        }
        os << " arguments, the call passes " << params.size() << ": it would fail and its fee be paid ("
           << signature(*named.front()) << ")";
        return os.str();
    }

    std::string firstMismatch;
    for (const auto* m : sameCount) {
        std::string mismatch;
        for (size_t i = 0; i < params.size() && mismatch.empty(); ++i) {
            if (certainlyIncompatible(m->arguments[i].type, params[i])) {
                std::ostringstream one;
                one << signature(*m) << ": argument " << i + 1 << " is " << m->arguments[i].type;
                const auto expected = variantFor(m->arguments[i].type);
                if (!expected.empty()) {
                    one << " (" << expected << ")";
                }
                one << ", the call passes " << variantName(params[i]);
                mismatch = one.str();
            }
        }
        if (mismatch.empty()) {
            return {};  // this method may match
        }
        if (firstMismatch.empty()) {
            firstMismatch = mismatch;
        }
    }
    return "no method matches the arguments, the call would fail and its fee be paid: " + firstMismatch;
}

}  // namespace api
