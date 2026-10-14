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

#ifndef OTEL_SHARED_STRING_H
#define OTEL_SHARED_STRING_H

#include <atomic>
#include <cstring>
#include <memory>
#include <string_view>

#include <protopuf/string_like.h>

namespace otel {

    /// @brief An immutable, reference-counted string.
    ///
    /// The tracing hot path never copies string *characters* around: a `shared_string`
    /// holds a `shared_ptr` to an immutable character block, so
    /// - storing a span name / attribute key into a span is a refcount bump,
    /// - the same interned name shared by thousands of spans occupies one allocation,
    /// - protopuf encodes it *directly* from `data()`/`size()` through
    ///   `pp::string_like_coder` (no intermediate `std::string` is ever materialized).
    ///
    /// It satisfies `pp::string_decodable` (a contiguous range of `char` with a
    /// `(const char*, size)` constructor), so it can be used as the value type of a
    /// `pp::string_like_field` for both encoding and decoding. `string_decodable`
    /// includes the `string_buildable` construction requirement while excluding view
    /// types whose decoding strategy would be unsafe.
    ///
    /// NOTE: if your codebase already has an interned/shared string type, substitute
    /// it here — any type modeling `pp::string_decodable` works unchanged with
    /// `shared_string_field` below.
    class shared_string {
    public:
        shared_string() = default;

        /// build from a `(pointer, size)` pair — required by `pp::string_buildable`,
        /// used by protopuf when *decoding* (characters are copied out of the wire buffer,
        /// since the buffer may not outlive the message)
        shared_string(const char* p, std::size_t n) : data_(make_data(p, n)), size_(n) {}

        /// build from anything string-view-ish (copies the characters once, on creation)
        explicit shared_string(std::string_view s) : shared_string(s.data(), s.size()) {}

        /// static (string-literal) strings: zero allocation, zero copy
        /// (the pointer must outlive every use, which string literals trivially do)
        static shared_string literal(std::string_view s) {
            shared_string r;
            r.unowned_ = s.data();
            r.size_ = s.size();
            return r;
        }

        const char* data() const noexcept { return data_ ? data_.get() : unowned_; }
        std::size_t size() const noexcept { return size_; }
        bool empty() const noexcept { return size_ == 0; }

        const char* begin() const noexcept { return data(); }
        const char* end() const noexcept { return data() + size(); }

        operator std::string_view() const noexcept { return {data(), size()}; }
        std::string_view view() const noexcept { return *this; }

        long use_count() const noexcept { return data_.use_count(); }

        friend bool operator==(const shared_string& l, const shared_string& r) noexcept {
            return l.view() == r.view();
        }
        friend bool operator==(const shared_string& l, std::string_view r) noexcept {
            return l.view() == r;
        }

    private:
        static std::shared_ptr<char[]> make_data(const char* p, std::size_t n) {
            if (n == 0) return nullptr;
            auto b = 
#if defined(__cpp_lib_shared_ptr_arrays) && (__cpp_lib_shared_ptr_arrays >= 201707L)
               std::make_shared<char[]>(n);
#else
               std::shared_ptr<char[]>(new char[n]);
#endif
            std::memcpy(b.get(), p, n);
            return b;
        }

        std::shared_ptr<char[]> data_;
        const char* unowned_ = nullptr; // for `literal`
        std::size_t size_ = 0;
    };

    static_assert(pp::string_like<shared_string>);
    static_assert(pp::string_decodable<shared_string>);

    /// a protopuf field storing a @ref shared_string, wire-compatible with `string_field`
    template <pp::basic_fixed_string S, pp::uint<4> N, pp::attribute A = pp::singular>
    using shared_string_field = pp::string_like_field<S, N, shared_string, A>;

}

#endif // OTEL_SHARED_STRING_H
