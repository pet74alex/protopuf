//   Copyright 2026 pet74alex
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#include <gtest/gtest.h>

#include <protopuf/message.h>
#include <protopuf/reflection.h>

#include <array>
#include <string>
#include <vector>

#include "test_fixture.h"

using namespace pp;
using namespace std;

// a oneof with scalar, string and embedded message members
using Inner = message<uint32_field<"id", 1>>;

using OneofValue = oneof_field<"value",
    string_field<"string_value", 2>,
    uint32_field<"int_value", 3>,
    bool_field<"bool_value", 4>,
    message_field<"inner_value", 5, Inner>>;

using OneofMsg = message<
    int32_field<"before", 1>,
    OneofValue,
    string_field<"after", 6>>;

// the very same fields, declared as plain optional (singular) fields instead of a oneof
using PlainMsg = message<
    int32_field<"before", 1>,
    string_field<"string_value", 2>,
    uint32_field<"int_value", 3>,
    bool_field<"bool_value", 4>,
    message_field<"inner_value", 5, Inner>,
    string_field<"after", 6>>;

// concept sanity checks
static_assert(oneof_c<OneofValue>);
static_assert(!oneof_c<int32_field<"before", 1>>);
static_assert(!field_c<OneofValue>);
static_assert(message_component_c<OneofValue>);
static_assert(message_component_c<int32_field<"before", 1>>);
static_assert(oneof_member_c<string_field<"string_value", 2>>);
static_assert(!oneof_member_c<string_field<"string_value", 2, repeated>>);
static_assert(message_c<OneofMsg>);
static_assert(std::same_as<OneofValue::member_by_name<"string_value">, string_field<"string_value", 2>>);
static_assert(std::same_as<OneofValue::member_by_number<3>, uint32_field<"int_value", 3>>);
static_assert(std::same_as<OneofMsg::get_type_by_number<3>, OneofValue>);
static_assert(std::same_as<OneofMsg::get_type_by_name<"value">, OneofValue>);
static_assert(std::same_as<OneofMsg::get_type_by_name<"int_value">, OneofValue>);
static_assert(std::same_as<OneofMsg::get_type_by_number<1>, int32_field<"before", 1>>);

GTEST_TEST(oneof, function) {
    OneofValue v;
    EXPECT_TRUE(v.empty());
    EXPECT_FALSE(v.holds<"string_value">());

    v.emplace<"string_value">("hello");
    EXPECT_FALSE(v.empty());
    EXPECT_TRUE(v.holds<"string_value">());
    EXPECT_TRUE(v.holds<2>());
    EXPECT_FALSE(v.holds<"int_value">());
    EXPECT_EQ(v.get<"string_value">(), "hello");
    EXPECT_EQ(v.get<2>(), "hello");
    ASSERT_NE(v.get_if<"string_value">(), nullptr);
    EXPECT_EQ(*v.get_if<"string_value">(), "hello");
    EXPECT_EQ(v.get_if<"int_value">(), nullptr);

    // setting another member replaces the previous one (variant semantics)
    v.emplace<"int_value">(42u);
    EXPECT_TRUE(v.holds<"int_value">());
    EXPECT_FALSE(v.holds<"string_value">());
    EXPECT_EQ(v.get<"int_value">(), 42u);

    // value-converting assignment: the member is selected the way std::variant's
    // converting constructor selects an alternative (best match, narrowing excluded)
    v = true;
    EXPECT_TRUE(v.holds<"bool_value">());
    EXPECT_EQ(v.get<"bool_value">(), true);

    // visit
    v.visit(overloaded{
        [](bool b) { EXPECT_TRUE(b); },
        [](const auto&) { FAIL() << "wrong alternative visited"; }
    });

    v.reset();
    EXPECT_TRUE(v.empty());
}

GTEST_TEST(oneof, message_access) {
    // on Clang 15+ (and all supported GCC) this can be spelled more directly as
    // OneofValue("string_value"_f, "abc"); the of<> factory is used throughout the
    // test bodies because Clang 14's frontend crashes on the name-tag UDL form
    // in some contexts (compiler bug, fixed in Clang 15)
    OneofMsg m{7, OneofValue::of<"string_value">("abc"), "tail"};

    // the oneof is reachable by its own name; a *member's* name or number yields
    // a zero-copy field proxy for that member (ref to direct_member_access below)
    EXPECT_TRUE(m.get<"value">().holds<"string_value">());
    EXPECT_EQ(m["value"_f].get<2>(), "abc");
    EXPECT_EQ(m.get<"string_value">().value(), "abc");  // member proxy, set
    EXPECT_FALSE(m.get<"int_value">().has_value());     // member proxy, empty

    EXPECT_EQ(m.get<"before">(), 7);
    EXPECT_EQ(m.get<"after">(), "tail");
}

GTEST_TEST(oneof, direct_member_access_on_message) {
    // of<...> instead of the name-tag UDL constructor: clang 14's frontend
    // crashes on the UDL-tag form in some function bodies (fixed in clang 15);
    // the two spellings are semantically identical
    OneofMsg m{-1, OneofValue::of<"int_value">(7u), "tail"};
    const OneofMsg& cm = m;

    // ---- reading, by member name and by member number --------------------
    ASSERT_TRUE(cm["int_value"_f].has_value());
    EXPECT_EQ(cm["int_value"_f].value(), 7u);
    EXPECT_EQ(*cm[3_i], 7u);                        // same member, by number
    EXPECT_FALSE(cm["string_value"_f].has_value()); // inactive member: empty
    EXPECT_FALSE(cm[4_i].has_value());

    // the proxy reads the value stored inside the oneof - no copy
    EXPECT_EQ(&cm["int_value"_f].value(), &cm["value"_f].get<"int_value">());

    // the proxy carries the member's field identity
    using P = decltype(cm["int_value"_f]);
    static_assert(field_c<std::remove_cvref_t<P>>);
    static_assert(std::remove_cvref_t<P>::number == 3);
    static_assert(std::same_as<typename std::remove_cvref_t<P>::field_type,
                               uint32_field<"int_value", 3>>);

    // the oneof's own name still yields the whole oneof
    static_assert(std::same_as<std::remove_cvref_t<decltype(cm["value"_f])>, OneofValue>);

    // ---- writing through, by member name and number -----------------------
    m["string_value"_f] = "direct";     // activates string_value (int_value gone)
    ASSERT_TRUE(m["value"_f].holds<"string_value">());
    EXPECT_EQ(m["value"_f].get<"string_value">(), "direct");
    EXPECT_FALSE(m["int_value"_f].has_value());

    m[4_i] = true;                      // switch the active member by number
    ASSERT_TRUE(m["value"_f].holds<"bool_value">());

    m["bool_value"_f].reset();          // clearing the active member empties the oneof
    EXPECT_TRUE(m["value"_f].empty());

    m["inner_value"_f].emplace(9u);     // in-place construction of a message member
    ASSERT_TRUE(m["value"_f].holds<"inner_value">());
    EXPECT_EQ(m["inner_value"_f]->get<"id">(), 9u);

    // plain fields are untouched by all of the above
    EXPECT_EQ(m["before"_f], -1);
    EXPECT_EQ(m["after"_f], "tail");
}

GTEST_TEST(oneof, equality_and_merge) {
    OneofMsg a{1, OneofValue{}, "x"}, b{1, OneofValue{}, "x"};
    EXPECT_EQ(a, b);

    // members set directly on the message, like plain fields
    a["int_value"_f] = 5u;
    EXPECT_NE(a, b);

    b["int_value"_f] = 5u;
    EXPECT_EQ(a, b);

    b["string_value"_f] = "s";
    EXPECT_NE(a, b);

    // merge: non-empty oneof of the source overwrites
    a.merge(b);
    EXPECT_TRUE(a["string_value"_f].has_value());
    EXPECT_FALSE(a["int_value"_f].has_value());

    // merge: empty oneof of the source is a no-op
    OneofMsg empty_src;
    a.merge(empty_src);
    EXPECT_TRUE(a["string_value"_f].has_value());
}

template<typename T>
using oneof_coding = test_fixture<T>;
TYPED_TEST_SUITE(oneof_coding, coder_mode_types, test_name_generator);

TYPED_TEST(oneof_coding, encode_decode_round_trip) {
    using Mode = typename TestFixture::mode;

    OneofMsg m{123, OneofValue{}, "after"};
    m["inner_value"_f] = Inner{99u};  // set the member directly on the message

    array<byte, 64> buf{};

    auto encode_result = message_coder<OneofMsg>::encode<Mode>(m, buf);
    bytes rest;
    ASSERT_TRUE(Mode::get_value_from_result(std::move(encode_result), rest));
    const auto encoded = bytes(buf).first(begin_diff(rest, buf));

    auto decode_result = message_coder<OneofMsg>::decode<Mode>(encoded);
    decode_value<OneofMsg> decoded;
    ASSERT_TRUE(Mode::get_value_from_result(std::move(decode_result), decoded));

    EXPECT_EQ(decoded.first, m);
    ASSERT_TRUE(decoded.first["inner_value"_f].has_value());
    EXPECT_EQ(decoded.first["inner_value"_f]->get<"id">(), 99u);
}

TYPED_TEST(oneof_coding, wire_identical_to_plain_fields) {
    using Mode = typename TestFixture::mode;

    // messages carrying the same data: one via a oneof, one via plain optional fields
    const auto check_pair = [](const OneofMsg& om, const PlainMsg& pm) {
        using Mode2 = Mode;
        array<byte, 128> b1{}, b2{};

        bytes r1, r2;
        ASSERT_TRUE(Mode2::get_value_from_result(message_coder<OneofMsg>::encode<Mode2>(om, b1), r1));
        ASSERT_TRUE(Mode2::get_value_from_result(message_coder<PlainMsg>::encode<Mode2>(pm, b2), r2));

        const auto s1 = bytes(b1).first(begin_diff(r1, b1));
        const auto s2 = bytes(b2).first(begin_diff(r2, b2));

        ASSERT_EQ(s1.size(), s2.size());
        EXPECT_TRUE(std::equal(s1.begin(), s1.end(), s2.begin()))
            << "oneof and plain-field encodings differ";

        // sizes reported by the skippers must agree as well
        EXPECT_EQ(skipper<message_coder<OneofMsg>>::encode_skip(om),
                  skipper<message_coder<PlainMsg>>::encode_skip(pm));

        // cross-decoding: bytes encoded from the oneof message decode into the plain message
        decode_value<PlainMsg> pd;
        ASSERT_TRUE(Mode2::get_value_from_result(message_coder<PlainMsg>::decode<Mode2>(s1), pd));
        EXPECT_EQ(pd.first, pm);

        // ... and vice versa
        decode_value<OneofMsg> od;
        ASSERT_TRUE(Mode2::get_value_from_result(message_coder<OneofMsg>::decode<Mode2>(s2), od));
        EXPECT_EQ(od.first, om);
    };

    // note: the oneof member and the equivalent plain field are set with the
    // very same syntax - direct member access makes the two models interchangeable
    { // string member set
        OneofMsg om{11, OneofValue{}, "z"};
        om["string_value"_f] = "hello oneof";
        PlainMsg pm{11, {}, {}, {}, {}, "z"};
        pm["string_value"_f] = "hello oneof";
        check_pair(om, pm);
    }
    { // integer member set
        OneofMsg om{11, OneofValue{}, "z"};
        om["int_value"_f] = 777u;
        PlainMsg pm{11, {}, {}, {}, {}, "z"};
        pm["int_value"_f] = 777u;
        check_pair(om, pm);
    }
    { // embedded message member set
        OneofMsg om{11, OneofValue{}, "z"};
        om["inner_value"_f] = Inner{5u};
        PlainMsg pm{11, {}, {}, {}, {}, "z"};
        pm["inner_value"_f] = Inner{5u};
        check_pair(om, pm);
    }
    { // no member set: the oneof contributes no bytes at all
        OneofMsg om{11, OneofValue{}, "z"};
        PlainMsg pm{11, {}, {}, {}, {}, "z"};
        check_pair(om, pm);
    }
}

TYPED_TEST(oneof_coding, last_member_on_wire_wins) {
    using Mode = typename TestFixture::mode;

    // protobuf semantics: if several members of a oneof appear in the stream,
    // the last one parsed wins
    PlainMsg pm{{}, "first", 9000u, {}, {}, {}};  // string (field 2) then int (field 3)

    array<byte, 64> buf{};
    bytes rest;
    ASSERT_TRUE(Mode::get_value_from_result(message_coder<PlainMsg>::encode<Mode>(pm, buf), rest));
    const auto encoded = bytes(buf).first(begin_diff(rest, buf));

    decode_value<OneofMsg> od;
    ASSERT_TRUE(Mode::get_value_from_result(message_coder<OneofMsg>::decode<Mode>(encoded), od));

    ASSERT_TRUE(od.first["int_value"_f].has_value());
    EXPECT_EQ(od.first["int_value"_f].value(), 9000u);
    EXPECT_FALSE(od.first["string_value"_f].has_value());  // the earlier member lost
}

GTEST_TEST(oneof, embedded_message_with_oneof) {
    using Outer = message<message_field<"inner", 1, OneofMsg>, uint32_field<"n", 2>>;

    Outer o{OneofMsg{1, OneofValue{}, "s"}, 4u};
    o["inner"_f]->get<"string_value">() = "nested";  // through the embedded message

    array<byte, 128> buf{};
    auto rest = message_coder<Outer>::encode<unsafe_mode>(o, buf);
    const auto encoded = bytes(buf).first(begin_diff(rest, buf));

    auto [decoded, remaining] = message_coder<Outer>::decode<unsafe_mode>(encoded);
    EXPECT_EQ(decoded, o);
    EXPECT_EQ(decoded["inner"_f]->get<"string_value">().value(), "nested");
}

GTEST_TEST(oneof, for_each_visits_members_as_fields) {
    OneofMsg m{-1, OneofValue::of<"int_value">(77u), "tail"};

    std::vector<std::string> visited;
    std::vector<std::string> set_fields;
    m.for_each([&]<field_c F>(const F& f) {
        visited.emplace_back(F::name.data);
        if (!empty_field(f)) {
            set_fields.emplace_back(F::name.data);
        }
        // the active oneof member arrives as an ordinary set singular field
        if constexpr (F::name == basic_fixed_string("int_value")) {
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f.value(), 77u);
        }
        // inactive members arrive as empty singular fields
        if constexpr (F::name == basic_fixed_string("string_value")) {
            EXPECT_FALSE(f.has_value());
        }
    });

    // the oneof itself is never passed to `f`; its members are, individually
    const std::vector<std::string> expected_visited{
        "before", "string_value", "int_value", "bool_value", "inner_value", "after"};
    EXPECT_EQ(visited, expected_visited);

    const std::vector<std::string> expected_set{"before", "int_value", "after"};
    EXPECT_EQ(set_fields, expected_set);
}

GTEST_TEST(oneof, for_each_visits_all_members_unset_when_oneof_empty) {
    const OneofMsg m{-1, OneofValue{}, {}};

    std::size_t visits = 0, set = 0;
    m.for_each([&](const auto& f) {
        ++visits;
        if (!empty_field(f)) ++set;
    });
    EXPECT_EQ(visits, 6u);  // 2 plain fields + 4 oneof members
    EXPECT_EQ(set, 1u);     // only "before"
}

GTEST_TEST(oneof, for_each_mutation_writes_back_into_oneof) {
    OneofMsg m{{}, OneofValue::of<"string_value">("old"), {}};

    // setting a member through the synthesized field makes it the active member
    m.for_each([]<field_c F>(F& f) {
        if constexpr (F::name == basic_fixed_string("int_value")) {
            f = 5u;
        }
    });
    ASSERT_TRUE(m.get<"value">().holds<"int_value">());
    EXPECT_EQ(m.get<"value">().get<"int_value">(), 5u);

    // mutating the active member's value in place
    m.for_each([]<field_c F>(F& f) {
        if constexpr (F::name == basic_fixed_string("int_value")) {
            if (f.has_value()) *f += 1;
        }
    });
    EXPECT_EQ(m.get<"value">().get<"int_value">(), 6u);

    // clearing the active member empties the oneof
    m.for_each([]<field_c F>(F& f) {
        if constexpr (F::name == basic_fixed_string("int_value")) {
            f.reset();
        }
    });
    EXPECT_TRUE(m.get<"value">().empty());

    // if several members are set in one pass, the last in declaration order wins
    m.for_each([]<field_c F>(F& f) {
        if constexpr (F::name == basic_fixed_string("string_value")) {
            f = "loses";
        }
        if constexpr (F::name == basic_fixed_string("bool_value")) {
            f = true;
        }
    });
    ASSERT_TRUE(m.get<"value">().holds<"bool_value">());
    EXPECT_EQ(m.get<"value">().get<"bool_value">(), true);
}

GTEST_TEST(oneof, for_each_component_passes_whole_oneof) {
    OneofMsg m{{}, OneofValue::of<"bool_value">(true), {}};

    std::size_t components = 0;
    bool saw_oneof = false;
    m.for_each_component([&]<message_component_c C>(const C&) {
        ++components;
        if constexpr (oneof_c<C>) {
            saw_oneof = true;
            static_assert(C::name == basic_fixed_string("value"));
        }
    });
    EXPECT_EQ(components, 3u);  // before, the oneof itself, after
    EXPECT_TRUE(saw_oneof);
}

GTEST_TEST(oneof, for_each_unchanged_for_messages_without_oneof) {
    const Inner i{42u};
    std::size_t visits = 0;
    i.for_each([&](const auto&) { ++visits; });
    EXPECT_EQ(visits, 1u);
    i.for_each_component([&](const auto&) { ++visits; });
    EXPECT_EQ(visits, 2u);
}

GTEST_TEST(oneof, for_each_is_zero_copy) {
    OneofMsg m{{}, OneofValue::of<"string_value">("shared characters"), {}};

    // const iteration: the proxy reads the value stored inside the variant
    const OneofMsg& cm = m;
    const std::string* stored = &cm.get<"value">().get<"string_value">();
    cm.for_each([&]<field_c F>(const F& f) {
        if constexpr (F::name == basic_fixed_string("string_value")) {
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(&f.value(), stored);  // same object - nothing was copied
            // `f` is a proxy carrying the member's identity, not a field copy
            static_assert(!std::same_as<F, string_field<"string_value", 2>>);
            static_assert(std::same_as<typename F::field_type, string_field<"string_value", 2>>);
            static_assert(F::number == 2);
        }
    });

    // mutable iteration: the proxy writes through to the variant immediately
    m.for_each([&]<field_c F>(F& f) {
        if constexpr (F::name == basic_fixed_string("string_value")) {
            *f += "!";
            EXPECT_EQ(&f.value(), &m.get<"value">().get<"string_value">());
            EXPECT_EQ(m.get<"value">().get<"string_value">().back(), '!');  // visible at once
        }
    });
    EXPECT_EQ(m.get<"value">().get<"string_value">(), "shared characters!");
}

GTEST_TEST(oneof, construction_with_active_member) {
    // by member name tag: unambiguous even if several members share a value type
    const OneofValue by_name("int_value"_f, 42u);
    ASSERT_TRUE(by_name.holds<"int_value">());
    EXPECT_EQ(by_name.get<"int_value">(), 42u);

    // by member number tag
    const OneofValue by_number(2_i, "hello");
    ASSERT_TRUE(by_number.holds<"string_value">());
    EXPECT_EQ(by_number.get<"string_value">(), "hello");

    // named factory
    const auto by_of = OneofValue::of<"bool_value">(true);
    ASSERT_TRUE(by_of.holds<"bool_value">());
    EXPECT_EQ(by_of.get<"bool_value">(), true);

    // multi-argument in-place construction of the member value
    const OneofValue str("string_value"_f, 3u, 'x');  // std::string(3, 'x')
    EXPECT_EQ(str.get<"string_value">(), "xxx");

    // embedded message member
    const OneofValue inner("inner_value"_f, Inner{9u});
    ASSERT_TRUE(inner.holds<"inner_value">());
    EXPECT_EQ(inner.get<"inner_value">()["id"_f], 9u);
}

TYPED_TEST(oneof_coding, single_expression_const_message) {
    // the whole message, oneof member included, built in one expression:
    // it can be declared const - no default-construct-then-emplace needed.
    // (the equivalent name-tag spelling `OneofValue("int_value"_f, 7u)` - used in
    // the non-template tests above - crashes Clang 14's frontend when written in
    // a template context such as this TYPED_TEST: a compiler bug with UDLs
    // producing class-type non-type template parameters, fixed in Clang 15)
    const OneofMsg m{-1, OneofValue::of<"int_value">(7u), "tail"};
    EXPECT_EQ(m["before"_f], -1);
    ASSERT_TRUE(m["int_value"_f].has_value());  // direct member access on the const message
    EXPECT_EQ(*m["int_value"_f], 7u);
    EXPECT_EQ(m["after"_f], "tail");

    // equal and wire-identical to the emplace-built equivalent
    // (deliberately using the oneof's own emplace API here, for contrast)
    OneofMsg e{-1, OneofValue{}, "tail"};
    e["value"_f].emplace<"int_value">(7u);
    EXPECT_EQ(m, e);

    array<byte, 64> bm{}, be{};
    bytes rm, re;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<OneofMsg>::encode<TypeParam>(m, bm), rm));
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<OneofMsg>::encode<TypeParam>(e, be), re));
    const auto nm = begin_diff(rm, bm), ne = begin_diff(re, be);
    ASSERT_EQ(nm, ne);
    EXPECT_TRUE(equal(bm.begin(), bm.begin() + nm, be.begin()));
}

// ---- compile-time (constexpr) oneofs and messages -------------------------
//
// A oneof constructed with an active member is a literal value as long as the
// active member's value type is constexpr-friendly; inactive members (strings,
// embedded messages) don't matter, since only the active alternative is
// constructed. Unset optional string fields and empty repeated fields are
// constexpr-friendly too, so whole messages can be compile-time constants.
//
// This requires a standard library with complete C++20 constexpr support for
// std::string / std::vector (and thus constexpr destruction of variant
// alternatives, P2231) - e.g. libstdc++ from GCC 12; on GCC 11 and also on
// MacOS 14 (AppleClang) which protopuf otherwise supports, these compile-time
// checks are skipped.
//
// 1. Core library feature test macro validation
#if defined(__cpp_lib_constexpr_string) && __cpp_lib_constexpr_string >= 201907L \
    && defined(__cpp_lib_constexpr_vector) && __cpp_lib_constexpr_vector >= 201907L

    // 2. Explicitly drop AppleClang from structural constexpr dynamic allocation tracking
    #if defined(__apple_build_version__)
        #define PROTOPUF_HAS_CONSTEXPR_CONTAINERS 0
    #else
        // Non-Apple toolchains (GCC, Vanilla Clang, MSVC) with the macros defined are safe
        #define PROTOPUF_HAS_CONSTEXPR_CONTAINERS 1
    #endif
#else
    #define PROTOPUF_HAS_CONSTEXPR_CONTAINERS 0
#endif

#if (PROTOPUF_HAS_CONSTEXPR_CONTAINERS)

constexpr OneofValue constexpr_oneof("bool_value"_f, true);
static_assert(constexpr_oneof.holds<"bool_value">());
static_assert(!constexpr_oneof.holds<"int_value">());
static_assert(constexpr_oneof.get<"bool_value">() == true);
static_assert(!constexpr_oneof.empty());

static_assert(OneofValue::of<"int_value">(41u).get<"int_value">() + 1 == 42u);
static_assert(OneofValue{}.empty());

constexpr OneofMsg constexpr_msg{-1, OneofValue("int_value"_f, 7u), {}};
static_assert(constexpr_msg.get<"before">() == -1);
static_assert(constexpr_msg.get<"value">().holds<"int_value">());
static_assert(constexpr_msg.get<"value">().get<"int_value">() == 7u);
static_assert(!constexpr_msg.get<"after">().has_value());

// direct member access works in constant expressions too
static_assert(constexpr_msg.get<"int_value">().has_value());
static_assert(constexpr_msg.get<"int_value">().value() == 7u);
static_assert(constexpr_msg.get<3>().value() == 7u);
static_assert(!constexpr_msg.get<"bool_value">().has_value());

static_assert(constexpr_msg.fold([]<field_c F>(std::size_t acc, const F& f) {
    return acc + (empty_field(f) ? 0u : 1u);
}, std::size_t{0}) == 2);  // before + int_value

static_assert(constexpr_msg.fold([]<field_c F>(auto acc, const F& f) {
    return empty_field(f) ? pp::uint<4>(acc) : F::number;
}, 0) == 3);  // int_value is the last set field, its number is 3

GTEST_TEST(oneof, constexpr_message_usable_at_runtime) {
    // the compile-time constant is an ordinary message at runtime
    EXPECT_EQ(constexpr_msg["value"_f].get<"int_value">(), 7u);

    array<byte, 32> buf{};
    bytes rest;
    ASSERT_TRUE(safe_mode::get_value_from_result(
        message_coder<OneofMsg>::encode<safe_mode>(constexpr_msg, buf), rest));
    const auto n = begin_diff(rest, buf);

    decode_value<OneofMsg> dv;
    ASSERT_TRUE(safe_mode::get_value_from_result(
        message_coder<OneofMsg>::decode<safe_mode>(bytes(buf.begin(), n)), dv));
    EXPECT_EQ(dv.first, constexpr_msg);
}
#endif

// ---- structural assignment between plain-field and oneof messages ---------
//
// A message declaring plain optional fields and a message grouping (some of)
// the same fields into a oneof are wire-identical; they are also *assignable*
// to each other, in both directions, through the structural message
// assignment (matched by field numbers).

// the plain/oneof pair above, extended with a repeated field shared by both shapes
using OneofAssignMsg = message<
    int32_field<"before", 1>,
    OneofValue,
    string_field<"after", 6>,
    uint32_field<"nums", 7, repeated>>;

using PlainAssignMsg = message<
    int32_field<"before", 1>,
    string_field<"string_value", 2>,
    uint32_field<"int_value", 3>,
    bool_field<"bool_value", 4>,
    message_field<"inner_value", 5, Inner>,
    string_field<"after", 6>,
    uint32_field<"nums", 7, repeated>>;

// assignability is symmetric between the two shapes...
static_assert(std::is_assignable_v<OneofMsg&, const PlainMsg&>);
static_assert(std::is_assignable_v<PlainMsg&, const OneofMsg&>);
static_assert(std::is_assignable_v<OneofAssignMsg&, const PlainAssignMsg&>);
static_assert(std::is_assignable_v<PlainAssignMsg&, const OneofAssignMsg&>);
// ...requires every target field number to be covered by the source...
static_assert(!std::is_assignable_v<OneofMsg&, const Inner&>);
static_assert(!std::is_assignable_v<OneofMsg&,
    const message<int32_field<"before", 1>>&>);              // members 2..5, 6 uncovered
static_assert(!std::is_assignable_v<OneofAssignMsg&, const PlainMsg&>);  // "nums" (7) uncovered
// ...with value-compatible field declarations under each number
static_assert(!std::is_assignable_v<OneofMsg&, const message<
    string_field<"before", 1>,   // int32 target under number 1
    string_field<"string_value", 2>, uint32_field<"int_value", 3>,
    bool_field<"bool_value", 4>, message_field<"inner_value", 5, Inner>,
    string_field<"after", 6>>&>);
// same-type assignment still picks the ordinary copy assignment
static_assert(std::is_assignable_v<OneofMsg&, const OneofMsg&>);

GTEST_TEST(oneof, assign_plain_message_to_oneof_message) {
    PlainAssignMsg pm{-3, {}, {}, {}, {}, "tail", {1u, 2u, 3u}};
    pm["int_value"_f] = 7u;

    OneofAssignMsg om{1, OneofValue::of<"string_value">("stale"), "old", {9u}};
    om = pm;

    EXPECT_EQ(om["before"_f], -3);
    ASSERT_TRUE(om["value"_f].holds<"int_value">());     // the set plain field became the active member
    EXPECT_EQ(om["value"_f].get<"int_value">(), 7u);
    EXPECT_FALSE(om["string_value"_f].has_value());      // the previously active member is gone
    EXPECT_EQ(om["after"_f], "tail");
    EXPECT_EQ(om["nums"_f].size(), 3u);                  // repeated field copied
    EXPECT_EQ(om["nums"_f][2], 3u);

    // an unset source empties both the oneof and the plain fields
    om = PlainAssignMsg{};
    EXPECT_TRUE(om["value"_f].empty());
    EXPECT_FALSE(om["before"_f].has_value());
    EXPECT_FALSE(om["after"_f].has_value());
    EXPECT_TRUE(om["nums"_f].empty());
}

GTEST_TEST(oneof, assign_oneof_message_to_plain_message) {
    OneofAssignMsg om{5, OneofValue::of<"bool_value">(true), "t", {4u, 5u}};

    PlainAssignMsg pm;
    pm["string_value"_f] = "stale";   // pre-set: must be cleared, bool_value is active
    pm["int_value"_f] = 999u;
    pm = om;

    EXPECT_EQ(pm["before"_f], 5);
    ASSERT_TRUE(pm["bool_value"_f].has_value());
    EXPECT_EQ(pm["bool_value"_f].value(), true);
    EXPECT_FALSE(pm["string_value"_f].has_value());  // inactive members arrive empty
    EXPECT_FALSE(pm["int_value"_f].has_value());
    EXPECT_FALSE(pm["inner_value"_f].has_value());
    EXPECT_EQ(pm["after"_f], "t");
    EXPECT_EQ(pm["nums"_f], (vector{4u, 5u}));

    // an empty oneof clears all of its member fields in the plain message
    pm["inner_value"_f] = Inner{3u};
    pm = OneofAssignMsg{-1, OneofValue{}, {}, {}};
    EXPECT_FALSE(pm["bool_value"_f].has_value());
    EXPECT_FALSE(pm["inner_value"_f].has_value());
    EXPECT_EQ(pm["before"_f], -1);
}

GTEST_TEST(oneof, assign_last_set_member_wins) {
    // several source fields fall into the oneof: the last one in member
    // declaration order wins, mirroring last-wins decoding on the wire
    PlainMsg pm{{}, "first", 9000u, {}, {}, {}};  // string (2) and int (3) both set

    OneofMsg om{{}, OneofValue{}, {}};
    om = pm;
    ASSERT_TRUE(om["value"_f].holds<"int_value">());  // int_value is declared after string_value
    EXPECT_EQ(om["value"_f].get<"int_value">(), 9000u);
    EXPECT_FALSE(om["string_value"_f].has_value());
}

TYPED_TEST(oneof_coding, assignment_preserves_wire_bytes) {
    using Mode = typename TestFixture::mode;

    // one field of the oneof set: plain -> oneof -> plain assignments all
    // produce byte-identical encodings
    PlainAssignMsg pm{42, {}, {}, {}, {}, "z", {7u, 8u}};
    pm["inner_value"_f] = Inner{6u};

    OneofAssignMsg om;
    om = pm;
    PlainAssignMsg pm2;
    pm2 = om;
    EXPECT_EQ(pm2, pm);  // the full plain -> oneof -> plain round trip is lossless

    array<byte, 128> bp{}, bo{};
    bytes rp, ro;
    ASSERT_TRUE(Mode::get_value_from_result(
        message_coder<PlainAssignMsg>::encode<Mode>(pm, bp), rp));
    ASSERT_TRUE(Mode::get_value_from_result(
        message_coder<OneofAssignMsg>::encode<Mode>(om, bo), ro));
    const auto np = begin_diff(rp, bp), no = begin_diff(ro, bo);
    ASSERT_EQ(np, no);
    EXPECT_TRUE(equal(bp.begin(), bp.begin() + np, bo.begin()))
        << "assigned oneof message encodes differently from its plain source";
}

GTEST_TEST(oneof, fold_visits_members_as_fields) {
    // of<> instead of the name-tag UDL: works around the Clang 14 frontend crash
    // described in single_expression_const_message above
    const OneofMsg m{-1, OneofValue::of<"int_value">(7u), "tail"};

    // a field_c-constrained callback works: oneof members arrive as zero-copy
    // field proxies, exactly like for_each
    const auto set_count = m.fold([]<field_c F>(std::size_t acc, const F& f) {
        return acc + (empty_field(f) ? 0u : 1u);
    }, std::size_t{0});
    EXPECT_EQ(set_count, 3u);  // before, int_value, after

    // members are folded individually, in declaration order
    const auto names = m.fold([]<field_c F>(std::string acc, const F&) {
        return std::move(acc) + F::name.data + ";";
    }, std::string{});
    EXPECT_EQ(names, "before;string_value;int_value;bool_value;inner_value;after;");

    // the accumulator type may change from step to step
    const auto last_set_number = m.fold([]<field_c F>(auto acc, const F& f) {
        return empty_field(f) ? pp::uint<4>(acc) : F::number;
    }, 0);
    EXPECT_EQ(last_set_number, 6u);

    // same results as folding the equivalent plain-fields message
    PlainMsg p;
    p["before"_f] = -1;
    p["int_value"_f] = 7u;
    p["after"_f] = "tail";
    const auto plain_count = p.fold([]<field_c F>(std::size_t acc, const F& f) {
        return acc + (empty_field(f) ? 0u : 1u);
    }, std::size_t{0});
    const auto plain_names = p.fold([]<field_c F>(std::string acc, const F&) {
        return std::move(acc) + F::name.data + ";";
    }, std::string{});
    EXPECT_EQ(set_count, plain_count);
    EXPECT_EQ(names, plain_names);
}

GTEST_TEST(oneof, fold_mutable_writes_through) {
    OneofMsg m{-1, OneofValue::of<"int_value">(7u), "t"};  // of<>: Clang 14 workaround
    const auto visited = m.fold([]<field_c F>(std::size_t acc, F& f) {
        if constexpr (F::name == basic_fixed_string("int_value")) {
            *f += 1;  // through the proxy, straight into the variant
        }
        return acc + 1;
    }, std::size_t{0});
    EXPECT_EQ(visited, 6u);
    EXPECT_EQ(m["value"_f].get<"int_value">(), 8u);
}

GTEST_TEST(oneof, fold_component_passes_whole_oneof) {
    const OneofMsg m{-1, OneofValue::of<"bool_value">(true), {}};  // of<>: Clang 14 workaround
    const auto counts = m.fold_component(
        []<message_component_c C>(std::pair<int, int> acc, const C&) {
            if constexpr (oneof_c<C>) ++acc.second; else ++acc.first;
            return acc;
        }, std::pair<int, int>{0, 0});
    EXPECT_EQ(counts.first, 2);   // before, after
    EXPECT_EQ(counts.second, 1);  // the oneof itself
}