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

#ifndef PROTOPUF_STRING_LIKE_H
#define PROTOPUF_STRING_LIKE_H

#include <ranges>
#include <span>
#include <algorithm>
#include <string>
#include <string_view>
#include <concepts>
#include <type_traits>
#include <cstring>

#include "coder.h"
#include "varint.h"
#include "skip.h"
#include "field.h"

namespace pp {

    /// @brief A concept satisfied while `T` is a one-byte, character-like type
    /// (`char`, `signed char`, `unsigned char`, `char8_t` or `std::byte`).
    ///
    /// Such elements can be copied to/from the wire byte-by-byte, which allows
    /// bulk (memcpy-style) encoding and *zero-copy* decoding of contiguous strings.
    template <typename T>
    concept char_like =
        (std::integral<std::remove_cv_t<T>> && sizeof(T) == 1 && !std::same_as<std::remove_cv_t<T>, bool>)
        || std::same_as<std::remove_cv_t<T>, std::byte>;

    /// @brief A concept satisfied while `R` is a *string-like container*: any contiguous,
    /// sized range of @ref char_like elements.
    ///
    /// Examples: `std::string`, `std::string_view`, `std::u8string`, `std::vector<char>`,
    /// `std::span<const char>`, or any user-defined shared/interned string type exposing
    /// `data()`/`size()` over one-byte characters.
    template <typename R>
    concept string_like =
        std::ranges::contiguous_range<R>
        && std::ranges::sized_range<R>
        && char_like<std::ranges::range_value_t<R>>;

    /// @brief A @ref string_like type which can be (re)built from a `(const CharT*, size)` pair,
    /// as required for decoding.
    ///
    /// - For view types (i.e. `std::string_view`) construction is **zero-copy**: the decoded
    ///   value refers directly into the decoded buffer.
    /// - For owning types (i.e. `std::string` or a user-defined shared string) the constructor
    ///   decides how the bytes are stored.
    template <typename R>
    concept string_buildable =
        string_like<R>
        && std::constructible_from<R, const std::ranges::range_value_t<R>*, std::size_t>;

    /// @brief A @ref string_like type which is also a (non-owning) view, i.e. `std::string_view`.
    ///
    /// Decoding into such a type performs no character copies at all, but the decoded value
    /// only remains valid as long as the decoded buffer is alive.
    template <typename R>
    concept string_view_like = string_like<R> && std::ranges::view<R>;

    /// @brief A string-like type for which decoding has a lifetime- and aliasing-safe strategy.
    ///
    /// Views over ordinary byte-compatible character types can decode zero-copy. A view over
    /// `char8_t` is excluded: the wire buffer contains bytes, not `char8_t` objects, so a
    /// `std::u8string_view` cannot safely be made by reinterpreting that storage. Owning
    /// `char8_t` strings remain supported and are decoded by copying into real `char8_t` storage.
    template <typename R>
    concept string_decodable =
        string_buildable<R>
        && !(std::same_as<std::remove_cv_t<std::ranges::range_value_t<R>>, char8_t>
             && string_view_like<R>);

    /// @brief A @ref coder for @ref string_decodable types, encoding them as protobuf
    /// length-delimited fields (wire type 2), byte-for-byte compatible with @ref string_coder.
    ///
    /// Unlike @ref array_coder (which encodes element-by-element through an element coder),
    /// this coder copies the character storage in bulk and never materializes intermediate
    /// containers. In particular:
    /// - **encoding** works directly from any @ref string_like source (`std::string_view`,
    ///   shared strings, ...) without converting to `std::string` first;
    /// - **decoding** into a supported @ref string_view_like type is zero-copy (the result
    ///   is a view into the input buffer);
    /// - decoding into `std::u8string` copies the wire bytes into owning `char8_t` storage;
    ///   `char8_t` views are intentionally not supported because the input buffer does not
    ///   contain `char8_t` objects to view.
    template <string_decodable R>
    struct string_like_coder {
        using value_type = R;
        using char_type = std::ranges::range_value_t<R>;

        string_like_coder() = delete;

        template <coder_mode Mode = safe_mode>
        static constexpr encode_result<Mode> encode(const R& s, bytes b) {
            const std::size_t n = std::ranges::size(s);

            bytes safe_b;
            if (!Mode::get_value_from_result(varint_coder<uint<8>>::template encode<Mode>(n, b), safe_b)) {
                return {};
            }

            if (!Mode::check_bytes_span(safe_b, n)) {
                return {};
            }

            const char_type* const p = std::ranges::data(s);
            for (std::size_t i = 0; i < n; ++i) {
                safe_b[i] = static_cast<std::byte>(static_cast<unsigned char>(p[i]));
            }

            return Mode::template make_result<encode_result<Mode>>(safe_b.subspan(n));
        }

        template <coder_mode Mode = safe_mode>
        static decode_result<R, Mode> decode(bytes b) {
            decode_value<uint<8>> decode_len;
            if (!Mode::get_value_from_result(varint_coder<uint<8>>::template decode<Mode>(b), decode_len)) {
                return {};
            }

            uint<8> len = 0;
            std::tie(len, b) = decode_len;

            if (!Mode::check_bytes_span(b, len)) {
                return {};
            }

            const auto size = static_cast<std::size_t>(len);

            if constexpr (std::same_as<std::remove_cv_t<char_type>, char8_t>) {
                // Do not reinterpret the wire bytes as char8_t objects. Copy them into
                // actual char8_t storage first. The temporary is then used to construct
                // R, which must not be a view (enforced by string_decodable).
                std::u8string storage(size, u8'\0');
                if (size != 0) {
                    std::memcpy(storage.data(), b.data(), size);
                }

                if constexpr (std::same_as<R, std::u8string>) {
                    return Mode::template make_result<decode_result<R, Mode>>(
                        std::move(storage), b.subspan(len));
                } else {
                    R s{storage.data(), size};
                    return Mode::template make_result<decode_result<R, Mode>>(
                        std::move(s), b.subspan(len));
                }
            } else {
                // For the remaining supported character types, a view can safely refer
                // directly into the input byte buffer; owning types may copy in their
                // pointer-and-size constructor as before.
                R s{reinterpret_cast<const char_type*>(b.data()), size};
                return Mode::template make_result<decode_result<R, Mode>>(
                    std::move(s), b.subspan(len));
            }
        }
    };

    template <string_decodable R>
    struct skipper<string_like_coder<R>> {
        using coder = string_like_coder<R>;
        using value_type = R;

        static constexpr std::size_t encode_skip(const R& s) {
            const std::size_t n = std::ranges::size(s);
            return n + skipper<varint_coder<uint<8>>>::encode_skip(n);
        }

        template <coder_mode Mode = safe_mode>
        static constexpr decode_skip_result<Mode> decode_skip(bytes b) {
            decode_value<uint<8>> decode_len;
            if (!Mode::get_value_from_result(varint_coder<uint<8>>::template decode<Mode>(b), decode_len)) {
                return {};
            }

            uint<8> n = 0;
            std::tie(n, b) = decode_len;

            if (!Mode::check_bytes_span(b, n)) {
                return {};
            }

            return Mode::template make_result<decode_skip_result<Mode>>(b.subspan(n));
        }
    };

    /// string-like coders are length-delimited on the wire (wire type 2)
    template <string_decodable R>
    struct wire_type_impl<string_like_coder<R>> : std::integral_constant<uint<1>, 2> {};

    /// Type alias for fields of any @ref string_buildable type `R`
    /// (i.e. `std::string_view`, `std::string`, user-defined shared strings, ...)
    template <basic_fixed_string S, uint<4> N, string_decodable R, attribute A = singular,
              typename Container = std::vector<R>>
    using string_like_field = field<S, N, string_like_coder<R>, A, Container>;

    /// Type alias for `std::string_view` fields: zero-copy on encoding *and* decoding
    template <basic_fixed_string S, uint<4> N, attribute A = singular,
              typename Container = std::vector<std::string_view>>
    using string_view_field = string_like_field<S, N, std::string_view, A, Container>;

    /// @brief A `std::span<const std::byte>` with *content* equality.
    ///
    /// `std::span` deliberately has no `operator==`, but protobuf `bytes` fields need
    /// value comparison so that messages containing them (and oneofs with a bytes
    /// member) remain equality-comparable. `bytes_view` is therefore used as the value
    /// type of @ref bytes_view_field: it behaves exactly like a span (zero-copy view)
    /// while comparing byte-by-byte.
    struct bytes_view : std::span<const std::byte> {
        using base = std::span<const std::byte>;

        constexpr bytes_view() noexcept = default;
        constexpr bytes_view(const std::byte* p, std::size_t n) : base(p, n) {}
        constexpr bytes_view(base s) noexcept : base(s) {}

        friend constexpr bool operator==(const bytes_view& a, const bytes_view& b) {
            return a.size() == b.size() && std::ranges::equal(a, b);
        }
    };

    static_assert(string_like<bytes_view>);
    static_assert(string_buildable<bytes_view>);

    /// Type alias for bytes fields viewed as @ref bytes_view : zero-copy on encoding and
    /// decoding, comparable by content
    template <basic_fixed_string S, uint<4> N, attribute A = singular,
              typename Container = std::vector<bytes_view>>
    using bytes_view_field = string_like_field<S, N, bytes_view, A, Container>;
}

#endif //PROTOPUF_STRING_LIKE_H
