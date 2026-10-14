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

#ifndef PROTOPUF_INDIRECT_H
#define PROTOPUF_INDIRECT_H

#include <memory>
#include <utility>
#include <concepts>
#include <type_traits>

namespace pp {

    /// @brief A nullable, deep-copying, heap-allocating value wrapper - the same idea as
    /// C++26 `std::indirect`, after which it is named.
    ///
    /// `indirect<T>` provides `std::optional`-like observers (`has_value`, `value`, `operator*`)
    /// but stores its value on the heap, so `T` may be an *incomplete* type at the point
    /// where `indirect<T>` is used (e.g. as a base/member of a class that is still being defined).
    /// This is the key building block for **self-referencing (recursive) message types**:
    /// a singular field of a message cannot hold the message itself by value
    /// (`std::optional<T>` requires a complete `T`), but it can hold an `indirect<T>`.
    ///
    /// Unlike `std::unique_ptr`, `indirect` has value semantics: copying an `indirect` deep-copies
    /// the owned object, and equality compares the owned objects, not the pointers.
    ///
    /// Differences from C++26 `std::indirect` (P3019): `pp::indirect` is *nullable by design* -
    /// an empty `indirect` models an unset optional field and is part of the normal state space
    /// (default construction is empty, `reset()` empties), whereas `std::indirect` is only
    /// valueless after being moved from; allocator support is also omitted. When compiling
    /// against a C++26 standard library, `std::indirect` is **not** a drop-in replacement for
    /// exactly this reason.
    ///
    /// NOTE on name lookup: precisely because the name matches `std::indirect`, translation
    /// units that open both `using namespace pp` and `using namespace std` must write
    /// `pp::indirect` qualified - an unqualified `indirect` is ambiguous there once the
    /// standard library ships `std::indirect` in `<memory>` (C++26). Inside `namespace pp`
    /// and in code that only opens one of the two namespaces, no qualification is needed.
    template <typename T>
    class indirect {
        std::unique_ptr<T> ptr;

    public:
        using value_type = T;

        constexpr indirect() noexcept = default;
        constexpr indirect(std::nullptr_t) noexcept {}

        /// construct from a value (copies onto the heap)
        indirect(const T& v) : ptr(std::make_unique<T>(v)) {}
        /// construct from a value (moves onto the heap)
        indirect(T&& v) : ptr(std::make_unique<T>(std::move(v))) {}

        /// in-place construction of the owned value
        template <typename... Args> requires std::constructible_from<T, Args...>
        explicit indirect(std::in_place_t, Args&&... args) : ptr(std::make_unique<T>(std::forward<Args>(args)...)) {}

        indirect(const indirect& other) : ptr(other.ptr ? std::make_unique<T>(*other.ptr) : nullptr) {}
        indirect(indirect&& other) noexcept = default;

        indirect& operator=(const indirect& other) {
            if (this != &other) {
                if (other.ptr) {
                    if (ptr) { *ptr = *other.ptr; }
                    else     { ptr = std::make_unique<T>(*other.ptr); }
                } else {
                    ptr.reset();
                }
            }
            return *this;
        }
        indirect& operator=(indirect&& other) noexcept = default;

        indirect& operator=(const T& v) {
            if (ptr) { *ptr = v; } else { ptr = std::make_unique<T>(v); }
            return *this;
        }
        indirect& operator=(T&& v) {
            if (ptr) { *ptr = std::move(v); } else { ptr = std::make_unique<T>(std::move(v)); }
            return *this;
        }

        /// whether the indirect owns a value (mirrors `std::optional::has_value`)
        [[nodiscard]] bool has_value() const noexcept { return static_cast<bool>(ptr); }
        explicit operator bool() const noexcept { return has_value(); }

        /// access the owned value (mirrors `std::optional::value`); requires `has_value()`
        T& value() & { return *ptr; }
        const T& value() const & { return *ptr; }
        T&& value() && { return std::move(*ptr); }

        T& operator*() & { return *ptr; }
        const T& operator*() const & { return *ptr; }
        T&& operator*() && { return std::move(*ptr); }

        T* operator->() noexcept { return ptr.get(); }
        const T* operator->() const noexcept { return ptr.get(); }

        /// construct the owned value in place, destroying the previous one (if any)
        template <typename... Args> requires std::constructible_from<T, Args...>
        T& emplace(Args&&... args) {
            ptr = std::make_unique<T>(std::forward<Args>(args)...);
            return *ptr;
        }

        /// destroy the owned value (if any); afterwards `has_value() == false`
        void reset() noexcept { ptr.reset(); }

        /// deep (value) equality: two empty indirects are equal; empty != non-empty
        ///
        /// note: all comparisons are *templates*, so that they are never selected through
        /// a derived-to-base conversion; otherwise, comparing two objects of a class
        /// (indirectly) derived from `indirect` (i.e. a self-referencing message with a
        /// heap-indirected field of its own type) would be ambiguous
        template <typename U>
        friend bool operator==(const indirect& a, const indirect<U>& b) {
            if (a.has_value() != b.has_value()) return false;
            return !a.has_value() || *a == *b;
        }

        /// equality against a plain value: an empty indirect compares unequal to any value.
        /// (types derived from `indirect` are excluded so that this overload is never selected
        /// through a derived-to-base conversion)
        template <typename U>
            requires (!std::is_base_of_v<indirect, std::remove_cvref_t<U>>)
        friend bool operator==(const indirect& a, const U& v) { return a.has_value() && *a.ptr == v; }
    };

    /// Checks whether the type is a @ref indirect type
    template <typename>
    constexpr inline bool is_indirect = false;

    template <typename T>
    constexpr inline bool is_indirect<indirect<T>> = true;

    /// A concept satisfied while type `T` is a @ref indirect type
    template <typename T>
    concept indirect_c = is_indirect<T>;

}

#endif //PROTOPUF_INDIRECT_H
