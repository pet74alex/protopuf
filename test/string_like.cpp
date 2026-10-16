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
#include <protopuf/string_like.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdint>

#include "test_fixture.h"

using namespace pp;
using namespace std;

// a minimal user-defined shared (reference-counted) string type, standing in for
// interned strings used by the OpenTelemetry example; satisfies `string_buildable`
class shared_string {
public:
    shared_string() = default;

    shared_string(const char* p, std::size_t n) : data_(std::make_shared<std::string>(p, n)) {}

    explicit shared_string(std::string s) : data_(std::make_shared<std::string>(std::move(s))) {}

    const char* data() const { return data_ ? data_->data() : nullptr; }
    std::size_t size() const { return data_ ? data_->size() : 0; }

    const char* begin() const { return data(); }
    const char* end() const { return data() + size(); }

    long use_count() const { return data_.use_count(); }

    operator std::string_view() const { return data_ ? std::string_view(*data_) : std::string_view{}; }

    friend bool operator==(const shared_string& l, const shared_string& r) {
        return static_cast<std::string_view>(l) == static_cast<std::string_view>(r);
    }

private:
    std::shared_ptr<std::string> data_;
};

// concept coverage
static_assert(char_like<char>);
static_assert(char_like<unsigned char>);
static_assert(char_like<char8_t>);
static_assert(char_like<std::byte>);
static_assert(!char_like<bool>);
static_assert(!char_like<wchar_t> || sizeof(wchar_t) == 1);
static_assert(!char_like<int>);

static_assert(string_like<std::string>);
static_assert(string_like<std::string_view>);
static_assert(string_like<std::vector<char>>);
static_assert(string_like<std::span<const std::byte>>);
static_assert(string_like<shared_string>);
static_assert(!string_like<std::vector<int>>);

static_assert(string_buildable<std::string>);
static_assert(string_buildable<std::string_view>);
static_assert(string_buildable<shared_string>);
static_assert(string_buildable<std::u8string>);
static_assert(string_buildable<std::u8string_view>);

static_assert(string_decodable<std::u8string>);
static_assert(!string_decodable<std::u8string_view>);

static_assert(string_view_like<std::string_view>);
static_assert(!string_view_like<std::string>);
static_assert(!string_view_like<shared_string>);

// coders satisfy the full coder concept
static_assert(coder<string_like_coder<std::string_view>>);
static_assert(coder<string_like_coder<shared_string>>);
static_assert(coder<string_like_coder<std::u8string>>);

using ViewMsg = message<
    string_view_field<"name", 1>,
    string_view_field<"tags", 2, repeated>,
    uint32_field<"n", 3>>;

using StringMsg = message<
    string_field<"name", 1>,
    string_field<"tags", 2, repeated>,
    uint32_field<"n", 3>>;

using SharedMsg = message<
    string_like_field<"name", 1, shared_string>,
    string_like_field<"tags", 2, shared_string, repeated>,
    uint32_field<"n", 3>>;


using U8StringMsg = message<
    string_like_field<"name", 1, std::u8string>,
    uint32_field<"n", 2>>;

template<typename T>
using string_like_coding = test_fixture<T>;
TYPED_TEST_SUITE(string_like_coding, coder_mode_types, test_name_generator);

TYPED_TEST(string_like_coding, wire_identical_to_string_field) {
    const ViewMsg vm{"hello"sv, {"a"sv, "bc"sv}, 42u};
    const StringMsg sm{"hello"s, {"a"s, "bc"s}, 42u};

    ASSERT_EQ(skipper<message_coder<ViewMsg>>::encode_skip(vm),
              skipper<message_coder<StringMsg>>::encode_skip(sm));

    array<byte, 64> bv{}, bs{};
    bytes rv, rs;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<ViewMsg>::encode<TypeParam>(vm, bv), rv));
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<StringMsg>::encode<TypeParam>(sm, bs), rs));

    const auto nv = begin_diff(rv, bv), ns = begin_diff(rs, bs);
    ASSERT_EQ(nv, ns);
    ASSERT_TRUE(equal(bv.begin(), bv.begin() + nv, bs.begin()));

    // a std::string message decodes what a view message encoded, and vice versa
    decode_value<StringMsg> ds;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<StringMsg>::decode<TypeParam>(bytes(bv.begin(), nv)), ds));
    ASSERT_EQ(ds.first, sm);

    decode_value<ViewMsg> dv;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<ViewMsg>::decode<TypeParam>(bytes(bs.begin(), ns)), dv));
    ASSERT_EQ(dv.first, vm);
}

TYPED_TEST(string_like_coding, decoded_view_is_zero_copy) {
    const ViewMsg vm{"zero-copy"sv, {"x"sv, "yz"sv}, 1u};

    array<byte, 64> buf{};
    bytes rest;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<ViewMsg>::encode<TypeParam>(vm, buf), rest));
    const auto n = begin_diff(rest, buf);

    decode_value<ViewMsg> dv;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<ViewMsg>::decode<TypeParam>(bytes(buf.begin(), n)), dv));

    const auto& decoded = dv.first;
    ASSERT_EQ(decoded, vm);

    // every decoded view must point directly into `buf` (no copies were made)
    const char* const lo = reinterpret_cast<const char*>(buf.data());
    const char* const hi = lo + buf.size();
    const auto in_buffer = [&](std::string_view v) {
        return v.data() >= lo && v.data() + v.size() <= hi;
    };
    ASSERT_TRUE(in_buffer(decoded["name"_f].value()));
    for (const auto& t : decoded["tags"_f]) {
        ASSERT_TRUE(in_buffer(t));
    }
}

TYPED_TEST(string_like_coding, shared_string_round_trip) {
    shared_string name{"span.name"s};
    const long rc_before = name.use_count();

    SharedMsg m;
    m["name"_f] = name;                       // shares, does not deep-copy characters
    m["tags"_f].emplace_back("k1", 2);
    m["tags"_f].emplace_back("key2", 4);
    m["n"_f] = 7u;

    ASSERT_EQ(name.use_count(), rc_before + 1);
    ASSERT_EQ(m["name"_f]->data(), name.data()); // same underlying buffer

    array<byte, 64> buf{};
    bytes rest;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<SharedMsg>::encode<TypeParam>(m, buf), rest));
    const auto n = begin_diff(rest, buf);

    // the wire bytes equal those of the plain std::string equivalent
    const StringMsg sm{"span.name"s, {"k1"s, "key2"s}, 7u};
    array<byte, 64> bs{};
    bytes rs;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<StringMsg>::encode<TypeParam>(sm, bs), rs));
    ASSERT_EQ(n, begin_diff(rs, bs));
    ASSERT_TRUE(equal(buf.begin(), buf.begin() + n, bs.begin()));

    // round trip back into shared_string
    decode_value<SharedMsg> dm;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<SharedMsg>::decode<TypeParam>(bytes(buf.begin(), n)), dm));
    ASSERT_EQ(dm.first, m);
}

TYPED_TEST(string_like_coding, bytes_view_field_round_trip) {
    using BytesMsg = message<bytes_view_field<"payload", 1>>;

    const array<byte, 4> payload{byte{0xde}, byte{0xad}, byte{0xbe}, byte{0xef}};
    BytesMsg m{span<const byte>(payload)};

    array<byte, 16> buf{};
    bytes rest;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<BytesMsg>::encode<TypeParam>(m, buf), rest));
    const auto n = begin_diff(rest, buf);

    decode_value<BytesMsg> dm;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<BytesMsg>::decode<TypeParam>(bytes(buf.begin(), n)), dm));

    // bytes_view compares by content, so whole messages with byte views are comparable
    // even though the decoded view points into a different buffer than the original
    ASSERT_EQ(dm.first, m);

    const auto v = dm.first["payload"_f].value();
    ASSERT_EQ(v.size(), payload.size());
    ASSERT_TRUE(equal(v.begin(), v.end(), payload.begin()));
    // zero-copy: points into buf
    ASSERT_GE(v.data(), buf.data());
    ASSERT_LE(v.data() + v.size(), buf.data() + buf.size());
}

TYPED_TEST(string_like_coding, u8string_decode_owns_char8_storage) {
    // Include a multi-byte UTF-8 sequence and an embedded NUL. Protobuf string-like
    // encoding preserves bytes; it does not perform Unicode normalization or validation.
    const std::u8string value = std::u8string{u8"hello 🌍"}
        + std::u8string(1, u8'\0') + u8"tail";
    const U8StringMsg original{value, 17u};

    array<byte, 64> buf{};
    bytes rest;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<U8StringMsg>::encode<TypeParam>(original, buf), rest));
    const auto n = begin_diff(rest, buf);

    decode_value<U8StringMsg> decoded;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<U8StringMsg>::decode<TypeParam>(bytes(buf.begin(), n)), decoded));
    ASSERT_EQ(decoded.first, original);

    const auto decoded_name = decoded.first["name"_f].value();
    ASSERT_EQ(decoded_name.size(), value.size());
    ASSERT_EQ(decoded_name, value);

    // The decoded owning string must not point into the wire buffer.
    ASSERT_FALSE(decoded_name.empty());
    const auto decoded_addr = reinterpret_cast<std::uintptr_t>(decoded_name.data());
    const auto buffer_addr = reinterpret_cast<std::uintptr_t>(buf.data());
    ASSERT_FALSE(decoded_addr >= buffer_addr && decoded_addr < buffer_addr + buf.size());
}

TEST(string_like_coding_safe, insufficient_buffer) {
    const std::string_view s = "abcdef";
    run_safe_encode_tests_with_insufficient_buffer_size<string_like_coder<std::string_view>, 6>(s);

    array<byte, 8> buf{};
    bytes rest;
    ASSERT_TRUE(safe_mode::get_value_from_result(
        string_like_coder<std::string_view>::encode<safe_mode>(s, buf), rest));
    const auto n = begin_diff(rest, buf);
    ASSERT_EQ(n, 7u);

    for (std::size_t k = 0; k < n; ++k) {
        ASSERT_FALSE(string_like_coder<std::string_view>::decode<safe_mode>(bytes(buf.begin(), k)));
    }
}
