#include <callcheck.hpp>

#include <gtest/gtest.h>

namespace {

general::MethodDescription method(const std::string& name, const std::vector<std::string>& types) {
    general::MethodDescription m;
    m.name = name;
    m.returnType = "boolean";
    for (size_t i = 0; i < types.size(); ++i) {
        general::MethodArgument a;
        a.type = types[i];
        a.name = "arg" + std::to_string(i);
        m.arguments.push_back(a);
    }
    return m;
}

general::Variant str(const std::string& s) {
    general::Variant v;
    v.__set_v_string(s);
    return v;
}

general::Variant dec(const std::string& s) {
    general::Variant v;
    v.__set_v_big_decimal(s);
    return v;
}

general::Variant integer(int32_t i) {
    general::Variant v;
    v.__set_v_int(i);
    return v;
}

// the methods of a v1 token (ExtensionTokenStandard) and of a v0 one
const std::vector<general::MethodDescription> v1Token = {
    method("transfer", {"java.lang.String", "java.math.BigDecimal"}),
    method("transferFrom", {"java.lang.String", "java.lang.String", "java.math.BigDecimal"}),
    method("balanceOf", {"java.lang.String"}),
    method("register", {}),
    method("setFrozen", {"boolean"}),
    method("payable", {"java.math.BigDecimal", "byte[]"}),
};

const std::vector<general::MethodDescription> v0Token = {
    method("transfer", {"java.lang.String", "java.lang.String"}),
};

}  // namespace

TEST(CallCheck, MatchingCallsPass) {
    EXPECT_EQ(api::checkCallArguments(v1Token, "transfer", {str("addr"), dec("1.5")}), "");
    EXPECT_EQ(api::checkCallArguments(v1Token, "register", {}), "");
    EXPECT_EQ(api::checkCallArguments(v0Token, "transfer", {str("addr"), str("1.5")}), "");
    general::Variant b;
    b.__set_v_boolean(true);
    EXPECT_EQ(api::checkCallArguments(v1Token, "setFrozen", {b}), "");
    general::Variant box;
    box.__set_v_boolean_box(true);
    EXPECT_EQ(api::checkCallArguments(v1Token, "setFrozen", {box}), "");  // a wrapper matches its primitive
}

TEST(CallCheck, WrongAmountTypeIsRefused) {
    const auto v1 = api::checkCallArguments(v1Token, "transfer", {str("addr"), str("1.5")});
    EXPECT_NE(v1.find("argument 2 is java.math.BigDecimal (v_big_decimal), the call passes v_string"), std::string::npos) << v1;
    const auto v0 = api::checkCallArguments(v0Token, "transfer", {str("addr"), dec("1.5")});
    EXPECT_NE(v0.find("argument 2 is java.lang.String (v_string), the call passes v_big_decimal"), std::string::npos) << v0;
    EXPECT_NE(api::checkCallArguments(v1Token, "setFrozen", {integer(1)}), "");
}

TEST(CallCheck, UnknownMethodOrArgumentCount) {
    EXPECT_NE(api::checkCallArguments(v1Token, "mint", {str("addr"), dec("1")}).find("no public method mint()"), std::string::npos);
    EXPECT_NE(api::checkCallArguments(v1Token, "transfer", {str("addr")}).find("takes 2 arguments, the call passes 1"), std::string::npos);
    EXPECT_EQ(api::checkCallArguments(v1Token, "toString", {}), "");  // Object's methods are not listed
}

TEST(CallCheck, UncertainTypesPass) {
    // declared types and arguments outside the known ones are taken as possible matches
    const std::vector<general::MethodDescription> generic = {method("put", {"java.lang.Object", "java.util.List"})};
    general::Variant list;
    list.__set_v_list({});
    EXPECT_EQ(api::checkCallArguments(generic, "put", {str("x"), list}), "");
    general::Variant null;
    null.__set_v_null("java.math.BigDecimal");
    EXPECT_EQ(api::checkCallArguments(v1Token, "transfer", {str("addr"), null}), "");
    // overloads: one of them matches
    const std::vector<general::MethodDescription> overloaded = {method("pay", {"java.math.BigDecimal"}), method("pay", {"java.lang.String"})};
    EXPECT_EQ(api::checkCallArguments(overloaded, "pay", {str("1")}), "");
}
