//   Copyright 2020-2021 PragmaTwice
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

#ifndef PROTOPUF_MESSAGE_H
#define PROTOPUF_MESSAGE_H

#include "coder.h"
#include "field.h"
#include "oneof.h"
#include "float.h"
#include "byte.h"
#include "coder_mode.h"

#include <unordered_map>
#include <functional>

namespace pp {

    /// Checks whether all types in the parameter list are equal
    template <typename...>
    constexpr inline bool are_same = true;

    template <typename T, typename... Ts>
    constexpr inline bool are_same<T, Ts...> = std::conjunction_v<std::is_same<T, Ts>...>;

    template <typename U, typename F>
    struct fold_impl {

        template <typename V>
            requires (!is_oneof<std::remove_cvref_t<V>>) && std::invocable<F, U&&, V&&>
        constexpr auto operator+(V&& other) {
            return pp::fold_impl {
                std::forward<F>(f)(static_cast<U&&>(v), std::forward<V>(other)), std::forward<F>(f)
            };
        }

        /// a oneof component is folded member-by-member, each member presented as a
        /// zero-copy field proxy - ref to `oneof_field::fold_members`
        template <typename V>
            requires is_oneof<std::remove_cvref_t<V>>
                && requires (V&& o, U&& u, F& fn) {
                    std::forward<V>(o).fold_members(std::forward<U>(u), fn);
                }
        constexpr auto operator+(V&& other) {
            return pp::fold_impl {
                std::forward<V>(other).fold_members(static_cast<U&&>(v), f), std::forward<F>(f)
            };
        }

        U v;
        F&& f;
    };

    template <typename U, typename F>
    fold_impl(U&&, F&&) -> fold_impl<U, F>;

    /// like @ref fold_impl, but folding raw components: a oneof is passed to `f`
    /// whole, not expanded into its members (backs `message::fold_component`)
    template <typename U, typename F>
    struct fold_component_impl {

        template <typename V> requires std::invocable<F, U&&, V&&>
        constexpr auto operator+(V&& other) {
            return pp::fold_component_impl {
                std::forward<F>(f)(static_cast<U&&>(v), std::forward<V>(other)), std::forward<F>(f)
            };
        }

        U v;
        F&& f;
    };

    template <typename U, typename F>
    fold_component_impl(U&&, F&&) -> fold_component_impl<U, F>;

    namespace type_forward_impl {
        template <typename V, typename W>
        using forward_const = std::conditional_t<std::is_const_v<W>, std::add_const_t<V>, V>;

        template <typename V, typename W>
        using forward_volatile = std::conditional_t<std::is_volatile_v<W>, std::add_volatile_t<V>, V>;

        template <typename V, typename W>
        using forward_cv = forward_volatile<forward_const<V, W>, W>;

        template <typename V, typename W>
        using forward_lvref = std::conditional_t<std::is_lvalue_reference_v<W>, std::add_lvalue_reference_t<V>, V>;

        template <typename V, typename W>
        using forward_rvref = std::conditional_t<std::is_rvalue_reference_v<W>, std::add_rvalue_reference_t<V>, V>;

        
        template <typename V, typename W>
        using forward_ref = forward_rvref<forward_lvref<V, W>, W>;

        template <typename T, typename U>
        using type = forward_ref<forward_cv<T, std::remove_reference_t<U>>, U>;
    }
    
    /// Copy cvref (`const`, `volatile` and lvalue/rvalue reference) of `U` to `T`
    template <typename T, typename U>
    using type_forward = type_forward_impl::type<T, U>;

    /// @brief Checks whether a *singular* plain field `C` can be assigned from the
    /// component covering the same field number in message type `Other` (a plain field
    /// or a oneof member, both exposing the optional-like `has_value`/`value` surface).
    ///
    /// Staged on `Other::contains_number` first, so `Other`'s accessors are never named
    /// for numbers it does not declare.
    template <typename Other, typename C,
              bool = std::remove_cvref_t<Other>::template contains_number<C::number>>
    constexpr inline bool singular_field_assignable_from_message = false;

    template <typename Other, typename C>
    constexpr inline bool singular_field_assignable_from_message<Other, C, true> =
        requires (C& c, const std::remove_cvref_t<Other>& o) {
            { o.template get<C::number>().has_value() } -> std::convertible_to<bool>;
            c = o.template get<C::number>().value();
            c.reset();
        };

    /// Checks whether a *repeated* plain field `C` can be assigned from the field with
    /// the same number in message type `Other` (container-to-container assignment; a
    /// oneof member can never be the source, since oneof members are singular)
    template <typename Other, typename C,
              bool = std::remove_cvref_t<Other>::template contains_number<C::number>>
    constexpr inline bool repeated_field_assignable_from_message = false;

    template <typename Other, typename C>
    constexpr inline bool repeated_field_assignable_from_message<Other, C, true> =
        requires (C& c, const std::remove_cvref_t<Other>& o) {
            static_cast<typename C::base_type&>(c) = static_cast<const typename
                std::remove_cvref_t<Other>::template field_type_by_number<C::number>::base_type&>(
                    o.template get<C::number>());
        };

    /// A lightweight structural check that `T` is (derived from) a @ref message, usable
    /// *before* `message` is defined (the full @ref message_c concept is declared below):
    /// every message injects the `message_type` alias
    template <typename T>
    concept message_shaped = requires { typename std::remove_cvref_t<T>::message_type; }
        && std::derived_from<std::remove_cvref_t<T>, typename std::remove_cvref_t<T>::message_type>;

    /// @brief Checks whether component `C` (plain field or oneof) of a message can be
    /// assigned from the structurally matching part of message type `Other`, ref to
    /// the structural `message::operator=`
    template <typename Other, message_component_c C>
    consteval bool component_assignable_from_message() {
        if constexpr (oneof_c<C>) {
            return C::template assignable_from_message<Other>;
        } else if constexpr (C::attr == repeated) {
            return repeated_field_assignable_from_message<Other, C>;
        } else {
            return singular_field_assignable_from_message<Other, C>;
        }
    }

    /// @brief The message type
    /// @param T the component types of the message: plain fields (@ref field_c) and/or
    /// oneofs (@ref oneof_c), where all `T::name_type::value_type` are equal
    template <message_component_c ... T> requires are_same<typename T::name_type::value_type...>
    struct message : private T... {

        /// the plain @ref message type which this type is (derived from);
        /// used to support user-defined self-referencing message types, i.e.
        /// `struct Node : message<..., message_field<"child", 2, Node, repeated>> {};`
        using message_type = message;

        constexpr message() = default;

        constexpr explicit message(T&& ...v) : T(std::move(v))... {};
        constexpr explicit message(const T& ...v) : T(v)... {};

        constexpr message(const message& other) : T(static_cast<const T&>(other))... {}
        constexpr message(message&& other) noexcept : T(static_cast<T&&>(other))... {}

        template <typename... U>
            requires (sizeof...(T) == sizeof...(U) && !are_same<message, std::remove_reference_t<U>...> )
        constexpr explicit message(U&& ...v) : T(std::forward<U>(v))... {}

        constexpr message& operator=(const message& other) {
            ((static_cast<T&>(*this) = static_cast<const T&>(other)), ...);
            return *this;
        }

        constexpr message& operator=(message&& other) noexcept {
            ((static_cast<T&>(*this) = static_cast<T&&>(other)), ...);
            return *this;
        }

        /// @brief Assign from a *structurally equivalent* message of a different type:
        /// one declaring value-compatible fields under the same field numbers, possibly
        /// grouping them differently - plain optional fields on one side may be members
        /// of a oneof on the other, in either direction.
        ///
        /// Every component of this message is assigned from the source part covering the
        /// same field numbers: a plain singular field copies the source value (and is
        /// reset when the source is empty - for a source oneof, inactive members read as
        /// empty), a repeated field copies the source container, and a oneof is emptied
        /// and then set to the *last* (in member declaration order) non-empty source
        /// field among its member numbers - mirroring the wire format, where the last
        /// member seen wins. Since a oneof is wire-identical to the equivalent plain
        /// fields, assigning either shape from the other yields a message with the very
        /// same encoding as the source (up to the last-member-wins collapse when several
        /// source fields fall into one oneof).
        template <typename Other>
            requires message_shaped<Other>
                && (!std::derived_from<std::remove_cvref_t<Other>, message>)
                && (component_assignable_from_message<Other, T>() && ...)
        constexpr message& operator=(const Other& other) {
            (assign_component_from_message<T>(other), ...);
            return *this;
        }

    private:
        template <message_component_c C, typename Other>
        constexpr void assign_component_from_message(const Other& other) {
            auto& dst = static_cast<C&>(*this);
            if constexpr (oneof_c<C>) {
                dst.assign_from_message(other);
            } else if constexpr (C::attr == repeated) {
                using SrcF = typename std::remove_cvref_t<Other>::template field_type_by_number<C::number>;
                static_cast<typename C::base_type&>(dst) =
                    static_cast<const typename SrcF::base_type&>(other.template get<C::number>());
            } else {
                decltype(auto) src = other.template get<C::number>();
                if (src.has_value()) {
                    dst = src.value();
                } else {
                    dst.reset();
                }
            }
        }

    public:

        /// the number of fields
        static constexpr uint<4> size = sizeof...(T);

        /// get the component type by the specific field number
        /// (for a member of a oneof: the enclosing @ref oneof_field type)
        template <uint<4> N>
        using get_type_by_number = component_number_selector<N, T...>;

        /// get the component type by the specific field/oneof name
        /// (for a member of a oneof: the enclosing @ref oneof_field type)
        template <basic_fixed_string S>
        using get_type_by_name = component_name_selector<S, T...>;

        /// whether any component of this message covers the field number `N`
        /// (a oneof covers the numbers of all of its members)
        template <uint<4> N>
        static constexpr bool contains_number = (component_contains_number<N, T> || ...);

        /// the *member-shaped field type* covering the field number `N`: the plain field
        /// with that number, or - if `N` belongs to a oneof - that oneof's member field
        template <uint<4> N>
        using field_type_by_number = component_member_field<N, component_number_selector<N, T...>>;

        /// @brief get a component (field or oneof) by the field number
        ///
        /// If `N` is the number of a *member of a oneof*, a zero-copy
        /// @ref oneof_member_ref proxy for that member is returned instead of the
        /// oneof (a oneof itself has no number), so members are directly usable by
        /// their own numbers: `msg.get<3>().has_value()`, `msg[3_i] = 42u`.
        template <uint<4> N>
        constexpr decltype(auto) get() const {
            using C = component_number_selector<N, T...>;
            if constexpr (oneof_c<C>) {
                return oneof_member_ref<typename C::template member_by_number<N>, const C>(
                    static_cast<const C&>(*this));
            } else {
                return static_cast<const C&>(*this);
            }
        }

        /// @brief get a component (field or oneof) by the field/oneof name
        ///
        /// If `S` names a *member of a oneof* (rather than the oneof itself), a
        /// zero-copy @ref oneof_member_ref proxy for that member is returned, so
        /// members are directly usable by their own names:
        /// `msg["int_value"_f].has_value()`, `msg["int_value"_f] = 42u`; the
        /// oneof's own name still yields the whole oneof.
        template <basic_fixed_string S>
        constexpr decltype(auto) get() const {
            using C = component_name_selector<S, T...>;
            if constexpr (oneof_c<C> && !(S == C::name)) {
                return oneof_member_ref<typename C::template member_by_name<S>, const C>(
                    static_cast<const C&>(*this));
            } else {
                return static_cast<const C&>(*this);
            }
        }

        /// get a component by the field number; a oneof member's number yields a
        /// writing-through @ref oneof_member_ref proxy (ref to the const overload)
        template <uint<4> N>
        constexpr decltype(auto) get() {
            using C = component_number_selector<N, T...>;
            if constexpr (oneof_c<C>) {
                return oneof_member_ref<typename C::template member_by_number<N>, C>(
                    static_cast<C&>(*this));
            } else {
                return static_cast<C&>(*this);
            }
        }

        /// get a component by the field/oneof name; a oneof member's name yields a
        /// writing-through @ref oneof_member_ref proxy (ref to the const overload)
        template <basic_fixed_string S>
        constexpr decltype(auto) get() {
            using C = component_name_selector<S, T...>;
            if constexpr (oneof_c<C> && !(S == C::name)) {
                return oneof_member_ref<typename C::template member_by_name<S>, C>(
                    static_cast<C&>(*this));
            } else {
                return static_cast<C&>(*this);
            }
        }

        /// get a field by the field number, i.e. `msg[233_i]`
        template <uint<4> F>
        constexpr decltype(auto) operator[](std::integral_constant<uint<4>, F>) const {
            return get<F>();
        }

        /// get a field by the field name, i.e. `msg["a"_f]`
        template <basic_fixed_string F>
        constexpr decltype(auto) operator[](constant<F>) const {
            return get<F>();
        }

        /// get a field by the field number, i.e. `msg[233_i]`
        template <uint<4> F>
        constexpr decltype(auto) operator[](std::integral_constant<uint<4>, F>) {
            return get<F>();
        }

        /// get a field by the field name, i.e. `msg["a"_f]
        template <basic_fixed_string F>
        constexpr decltype(auto) operator[](constant<F>) {
            return get<F>();
        }

        /// get a component as its `base_type` by the field number (a oneof owns its
        /// storage and exposes no base: for a oneof member's number, the whole oneof
        /// is returned instead)
        template <uint<4> N>
        constexpr decltype(auto) get_base() const {
            using C = component_number_selector<N, T...>;
            if constexpr (oneof_c<C>) {
                return static_cast<const C&>(*this);
            } else {
                return static_cast<const typename C::base_type&>(static_cast<const C&>(*this));
            }
        }

        /// get a component as its `base_type` by the field/oneof name (a oneof owns its
        /// storage and exposes no base: for a oneof or oneof member's name, the whole
        /// oneof is returned instead)
        template <basic_fixed_string S>
        constexpr decltype(auto) get_base() const {
            using C = component_name_selector<S, T...>;
            if constexpr (oneof_c<C>) {
                return static_cast<const C&>(*this);
            } else {
                return static_cast<const typename C::base_type&>(static_cast<const C&>(*this));
            }
        }

        /// get a component as its `base_type` by the field number
        template <uint<4> N>
        constexpr decltype(auto) get_base() {
            using C = component_number_selector<N, T...>;
            if constexpr (oneof_c<C>) {
                return static_cast<C&>(*this);
            } else {
                return static_cast<typename C::base_type&>(static_cast<C&>(*this));
            }
        }

        /// get a component as its `base_type` by the field/oneof name
        template <basic_fixed_string S>
        constexpr decltype(auto) get_base() {
            using C = component_name_selector<S, T...>;
            if constexpr (oneof_c<C>) {
                return static_cast<C&>(*this);
            } else {
                return static_cast<typename C::base_type&>(static_cast<C&>(*this));
            }
        }

    private:
        /// compare one component of two messages: plain fields compare through their
        /// `base_type` (avoiding the ambiguity self-referencing value types would
        /// otherwise create), a oneof through its own `operator==`
        template <message_component_c C>
        static constexpr bool component_equal(const C& a, const C& b) {
            if constexpr (oneof_c<C>) {
                return a == b;
            } else {
                return static_cast<const typename C::base_type&>(a) ==
                       static_cast<const typename C::base_type&>(b);
            }
        }

    public:
        constexpr bool operator==(const message & other) const {
            return (component_equal(static_cast<const T&>(*this), static_cast<const T&>(other)) && ...);
        }

        constexpr bool operator!=(const message & other) const {
            return !(*this == other);
        }

        /// @brief iterate all *fields* and apply function `f` to them
        ///
        /// Plain fields are passed to `f` directly (by reference). A @ref oneof_field
        /// is *not* passed as a whole: each of its members is presented as an ordinary
        /// singular field through a zero-copy @ref oneof_member_ref proxy, of which at
        /// most one (the active member) is set - ref to `oneof_field::for_each_member`.
        /// No values are copied anywhere. Use @ref for_each_component to iterate raw
        /// components (whole oneofs) instead.
        template <typename F> requires (component_invocable<F, const T&> && ...)
        constexpr void for_each(F&& f) const {
            (for_each_dispatch(f, static_cast<const T&>(*this)), ...);
        }

        /// @brief iterate all *fields* and apply function `f` to them
        ///
        /// Mutable overload; the oneof member proxies write through to the oneof
        /// immediately (setting a member makes it the active member, last writer
        /// wins; `reset()` on the active member empties the oneof), ref to
        /// `oneof_field::for_each_member`.
        template <typename F> requires (component_invocable<F, T&> && ...)
        constexpr void for_each(F&& f) {
            (for_each_dispatch(f, static_cast<T&>(*this)), ...);
        }

        /// iterate all *components* (plain fields and whole @ref oneof_field objects)
        /// and apply function `f` to them
        template <typename F> requires (std::invocable<F, T> && ...)
        constexpr void for_each_component(F&& f) const {
            (std::forward<F>(f)(static_cast<const T&>(*this)), ...);
        }

        /// iterate all *components* (plain fields and whole @ref oneof_field objects)
        /// and apply function `f` to them
        template <typename F> requires (std::invocable<F, T> && ...)
        constexpr void for_each_component(F&& f) {
            (std::forward<F>(f)(static_cast<T&>(*this)), ...);
        }

    private:
        template <typename F, typename C>
        static constexpr void for_each_dispatch(F& f, C&& c) {
            if constexpr (oneof_c<std::remove_cvref_t<C>>) {
                std::forward<C>(c).for_each_member(f);
            } else {
                f(std::forward<C>(c));
            }
        }

    public:

        /// @brief left-fold `f` over all *fields*:
        /// `f(f(...f(f(init, field1), field2), ...), fieldN)`
        ///
        /// Like @ref for_each, a @ref oneof_field is not folded as a whole: each of its
        /// members is presented as an ordinary singular field through a zero-copy
        /// @ref oneof_member_ref proxy, of which at most one (the active member) is set.
        /// The accumulator type may change from step to step. Use @ref fold_component
        /// to fold raw components (whole oneofs) instead.
        template <typename F, typename U>
        constexpr auto fold(F&& f, U&& init) const {
            return (fold_impl{ std::forward<U>(init), std::forward<F>(f) } + ... + static_cast<const T&>(*this)).v;
        }

        /// mutable overload of @ref fold : the oneof member proxies write through
        /// to the oneof immediately, as in the mutable @ref for_each
        template <typename F, typename U>
        constexpr auto fold(F&& f, U&& init) {
            return (fold_impl{ std::forward<U>(init), std::forward<F>(f) } + ... + static_cast<T&>(*this)).v;
        }

        /// left-fold `f` over all *components* (plain fields and whole @ref oneof_field
        /// objects): `f(f(...f(f(init, component1), component2), ...), componentN)`
        template <typename F, typename U>
        constexpr auto fold_component(F&& f, U&& init) const {
            return (fold_component_impl{ std::forward<U>(init), std::forward<F>(f) } + ... + static_cast<const T&>(*this)).v;
        }

        /// left-fold `f` over all *components* (plain fields and whole @ref oneof_field
        /// objects): `f(f(...f(f(init, component1), component2), ...), componentN)`
        template <typename F, typename U>
        constexpr auto fold_component(F&& f, U&& init) {
            return (fold_component_impl{ std::forward<U>(init), std::forward<F>(f) } + ... + static_cast<T&>(*this)).v;
        }

        /// @brief Merge another message into this message, for all fields: overwrite if it is singular and non-empty, merge to end otherwise
        ///
        /// same as `merge_field(field1, other.field1), ..., merge_field(fieldN, other.fieldN)`, ref to @ref merge_field
        template <merge_mode mode = merge_mode{}, typename M> requires std::same_as<std::remove_cvref_t<M>, message>
        constexpr void merge(M&& other) {
            (merge_field<mode>(static_cast<T&>(*this), static_cast<type_forward<T, M&&>>(other)), ...);
        }
    };

    /// Checks whether the type is a @ref message type
    template <typename>
    constexpr inline bool is_message = false;

    template <typename ...T>
    constexpr inline bool is_message <message<T...>> = true;

    /// @brief A concept satisfied while type `T` is a @ref message type or a user-defined
    /// type publicly derived from a @ref message type (declaring `using message_type`).
    ///
    /// Deriving from a message is the way to declare **self-referencing (recursive) messages**:
    /// ```
    /// struct Node : pp::message<
    ///     pp::string_field<"name", 1>,
    ///     pp::message_field<"children", 2, Node, pp::repeated>,   // repeated self-reference
    ///     pp::indirect_message_field<"parent_hint", 3, Node>> {   // singular self-reference (heap)
    /// };
    /// ```
    template <typename T>
    concept message_c =
        requires { typename std::remove_cvref_t<T>::message_type; }
        && is_message<typename std::remove_cvref_t<T>::message_type>
        && std::derived_from<std::remove_cvref_t<T>, typename std::remove_cvref_t<T>::message_type>;

    /// The plain @ref message type which `T` is (derived from)
    template <message_c T>
    using message_base_t = typename std::remove_cvref_t<T>::message_type;

    /// Merge multiple messages into one message, and return the merged message
    template <merge_mode mode = merge_mode{}, typename M1, typename... MN>
    requires message_c<std::remove_cvref_t<M1>> && 
             are_same<std::remove_cvref_t<M1>, std::remove_cvref_t<MN>...>
    constexpr auto merge(M1&& msg1, MN&& ... msgN) {
        std::remove_cvref_t<M1> res = std::forward<M1>(msg1);

        (res.template merge<mode>(std::forward<MN>(msgN)), ...);

        return res;
    }

    /// Get the wire type from a field key, ref to @ref field::key and @ref wire_type
    inline constexpr uint<1> to_wire_key(uint<4> field_key) {
        return field_key & 0b111u;
    }

    /// Get the field number from a field key, ref to @ref field::key and @ref field::number
    inline constexpr uint<4> to_field_number(uint<4> field_key) {
        return field_key >> 3u;
    }

    template <uint<1>>
    struct wire_skip_impl;

    template <>
    struct wire_skip_impl<0> {
        using type = skipper<varint_coder<uint<8>>>;
    };

    template <>
    struct wire_skip_impl<1> {
        using type = skipper<integer_coder<uint<8>>>;
    };

    template <>
    struct wire_skip_impl<2> {
        using type = skipper<string_coder>;
    };

    template <>
    struct wire_skip_impl<5> {
        using type = skipper<integer_coder<uint<4>>>;
    };

    /// Dispatch a wire type number to a @ref skipper
    template <uint<1> N>
    using wire_skip = typename wire_skip_impl<N>::type;

    template <coder_mode Mode, uint<1>... I>
    struct message_skip_map_impl : std::unordered_map<uint<4>, std::function<decode_skip_result<Mode>(bytes)>> {
        message_skip_map_impl() : std::unordered_map<uint<4>, std::function<decode_skip_result<Mode>(bytes)>> {
                {I, [](bytes b){
                    return wire_skip<I>::template decode_skip<Mode>(b);
                }}...
        } {}
    };

    template <coder_mode Mode = safe_mode>
    using message_skip_map = message_skip_map_impl<Mode, 0, 1, 2, 5>;
    template <coder_mode Mode = safe_mode>
    inline const message_skip_map<Mode> skip_map;

    template <coder_mode Mode = safe_mode>
    using message_decode_map_result = typename Mode::template result_type<std::pair<bytes, bool>>;

    template <coder_mode Mode = safe_mode>
    using message_decode_map_function_result = typename Mode::template result_type<bytes>;

    template <coder_mode, message_c>
    struct message_decode_map;

    template <coder_mode Mode, message_component_c... F>
    struct message_decode_map<Mode, message<F...>> :
        std::unordered_map<uint<4>, std::function<message_decode_map_function_result<Mode>(message<F...>&, bytes)>> {
    private:
        using T = message<F...>;
        using function_result = message_decode_map_function_result<Mode>;

        /// register the decoder of a plain field, keyed by its field key
        template <field_c F2>
        void insert_component() {
            this->emplace(F2::key, [](T& m, bytes b) -> function_result {
                static_assert(coder<typename F2::coder>,
                    "the coder of a message field must satisfy the pp::coder concept");

                decode_value<typename F2::coder::value_type> decode_v;
                if (Mode::get_value_from_result(F2::coder::template decode<Mode>(b), decode_v)) {
                    auto &f = m.template get<F2::number>();
                    push_field(f, std::move(decode_v.first));

                    return function_result{decode_v.second};
                }
                return function_result{};
            });
        }

        /// register the decoders of all members of a oneof, keyed by each member key
        template <oneof_c O>
        void insert_component() {
            O::template insert_decoders<Mode, T, function_result>(*this);
        }

    public:
        message_decode_map() {
            (insert_component<F>(), ...);
        }

        constexpr message_decode_map_result<Mode> decode(T& v, bytes b) const {
            decode_value<uint<4>> decode_v;
            if (!Mode::get_value_from_result(varint_coder<uint<4>>::decode<Mode>(b), decode_v)) {
                return {};
            }

            const auto &[n, nb] = decode_v;

            if(to_field_number(n) == 0) {
                return Mode::template make_result<message_decode_map_result<Mode>>(b, false);
            }

            const auto iter = this->find(n);
            if (iter != this->end()) {
                if (!Mode::get_value_from_result(iter->second(v, nb), b)) {
                    return {};
                }
            } else {
                if (!Mode::get_value_from_result(
                        skip_map<Mode>.at(to_wire_key(n))(nb), b)) {
                    return {};
                }
            }

            return Mode::template make_result<message_decode_map_result<Mode>>(b, true);
        }
    };

    template <coder_mode Mode, message_c T>
    inline const message_decode_map<Mode, T> decode_map;

    /// A @ref coder for @ref message type
    template <message_c T>
    struct message_coder {
        using value_type = T;

        message_coder() = delete;

        template <coder_mode Mode = safe_mode>
        static constexpr encode_result<Mode> encode(const T& msg, bytes b) {
            encode_result<Mode> result{b};
            msg.for_each_component([&result]<message_component_c F> (const F& f) {
                if(empty_field(f)) {
                    return;
                }

                bytes safe_b;
                if (!Mode::get_value_from_result(result, safe_b)) {
                    return;
                }

                if constexpr (oneof_c<F>) {
                    result = f.template encode<Mode>(safe_b);
                } else {
                    static_assert(coder<typename F::coder>,
                        "the coder of a message field must satisfy the pp::coder concept");

                    if constexpr (singular_attribute<F::attr>) {
                        result = varint_coder<uint<4>>::encode<Mode>(F::key, safe_b);
                        if (Mode::get_value_from_result(result, safe_b)) {
                            result = F::coder::template encode<Mode>(f.value(), safe_b);
                        }
                    } else {
                        // deferred from the field declaration, where the element type may
                        // still be incomplete (self-referencing messages), ref to field_container
                        static_assert(std::ranges::sized_range<typename F::base_type>,
                            "the container of a repeated field must satisfy std::ranges::sized_range");
                        for(const auto &i : f) {
                            result = varint_coder<uint<4>>::encode<Mode>(F::key, safe_b);
                            if (Mode::get_value_from_result(result, safe_b)) {
                                result = F::coder::template encode<Mode>(i, safe_b);
                                if (!Mode::get_value_from_result(result, safe_b)) {
                                    break;
                                }
                            } else {
                                break;
                            }
                        }
                    }
                }
            });

            return result;
        }
        
        template <coder_mode Mode = safe_mode>
        static constexpr decode_result<T, Mode> decode(bytes b) {
            T v;
            auto& base = static_cast<message_base_t<T>&>(v);

            while(b.end() > b.begin()) {
                std::pair<bytes, bool> bytes_with_next;
                if (!Mode::get_value_from_result(decode_map<Mode, message_base_t<T>>.decode(base, b), bytes_with_next)) {
                    return {};
                }

                bool next = true;
                std::tie(b, next) = bytes_with_next;

                if(!next) break;
            }

            return Mode::template make_result<decode_result<T, Mode>>(std::move(v), b);
        }
    };

    template <message_c T>
    struct skipper<message_coder<T>> {
        using value_type = T;

        static constexpr std::size_t encode_skip(const T& msg) {
            std::size_t n = 0;
            msg.for_each_component([&n]<message_component_c F> (const F& f) {
                if(empty_field(f)) {
                    return;
                }

                if constexpr (oneof_c<F>) {
                    n += f.encode_skip();
                } else if constexpr (singular_attribute<F::attr>) {
                    n += skipper<varint_coder<uint<4>>>::encode_skip(F::key);
                    n += skipper<typename F::coder>::encode_skip(f.value());
                } else {
                    static_assert(std::ranges::sized_range<typename F::base_type>,
                        "the container of a repeated field must satisfy std::ranges::sized_range");
                    for(const auto &i : f) {
                        n += skipper<varint_coder<uint<4>>>::encode_skip(F::key);
                        n += skipper<typename F::coder>::encode_skip(i);
                    }
                }
            });

            return n;
        }
    };

    /// @brief A @ref coder for embedded message
    ///
    /// Ref to https://developers.google.com/protocol-buffers/docs/encoding#embedded
    /// (note: the template parameter is deliberately *not* constrained with @ref message_c here,
    /// so that the coder can be named while `T` is still an incomplete self-referencing message;
    /// message-ness is statically asserted inside `encode`/`decode`, when `T` must be complete)
    template <typename T>
    struct embedded_message_coder {
        using value_type = T;

        embedded_message_coder() = delete;

        template <coder_mode Mode = safe_mode>
        static constexpr encode_result<Mode> encode(const T& msg, bytes b) {
            static_assert(message_c<T>, "embedded_message_coder requires a protopuf message type");

            auto n = skipper<message_coder<T>>::encode_skip(msg);

            if (Mode::get_value_from_result(varint_coder<uint<8>>::encode<Mode>(n, b), b)) {
                return message_coder<T>::template encode<Mode>(msg, b);
            }

            return {};
        }

        template <coder_mode Mode = safe_mode>
        static constexpr decode_result<T, Mode> decode(bytes b) {
            static_assert(message_c<T>, "embedded_message_coder requires a protopuf message type");

            T v;
            auto& base = static_cast<message_base_t<T>&>(v);

            decode_value<uint<8>> decod_len;
            if (!Mode::get_value_from_result(varint_coder<uint<8>>::decode<Mode>(b), decod_len)) {
                return {};
            }

            std::size_t len = 0;
            std::tie(len, b) = decod_len;

            const auto origin_b = b;
            while(begin_diff(b, origin_b) < len) {
                std::pair<bytes, bool> bytes_with_next;
                if (!Mode::get_value_from_result(decode_map<Mode, message_base_t<T>>.decode(base, b), bytes_with_next)) {
                    return {};
                }

                bool next = true;
                std::tie(b, next) = bytes_with_next;

                if(!next) break;
            }

            return Mode::template make_result<decode_result<T, Mode>>(std::move(v), b);
        }
    };

    template <typename T>
    struct skipper<embedded_message_coder<T>> {
        using value_type = T;

        static constexpr std::size_t encode_skip(const T& v) {
            static_assert(message_c<T>, "embedded_message_coder requires a protopuf message type");

            uint<8> n = skipper<message_coder<T>>::encode_skip(v);

            n += skipper<varint_coder<uint<8>>>::encode_skip(n);

            return n;
        }

        template <coder_mode Mode = safe_mode>
        static constexpr decode_skip_result<Mode> decode_skip(bytes b) {
            decode_value<uint<8>> decode_len;
            if (!Mode::get_value_from_result(varint_coder<uint<8>>::decode<Mode>(b), decode_len)) {
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

    template <typename T>
    struct wire_type_impl<embedded_message_coder<T>> : std::integral_constant<uint<1>, 2> {};

    /// Type alias for embedded message fields
    template <basic_fixed_string S, uint<4> N, typename T, attribute A = singular, typename Container = std::vector<T>>
    using message_field = field<S, N, embedded_message_coder<T>, A, Container>;

    /// @brief Type alias for singular embedded message fields whose value is stored on the heap
    /// (in a @ref indirect), identical to a singular @ref message_field on the wire.
    ///
    /// This is required for *singular* self-referencing fields, since `std::optional<T>` cannot
    /// hold an incomplete `T`:
    /// ```
    /// struct TreeNode : pp::message<
    ///     pp::string_field<"tag", 1>,
    ///     pp::indirect_message_field<"left", 2, TreeNode>,
    ///     pp::indirect_message_field<"right", 3, TreeNode>> {};
    /// ```
    /// (note: the attribute enumerator is named `boxed` rather than `indirect`, since
    /// `attribute` is an unscoped enum whose enumerators live directly in namespace `pp`,
    /// where the name would collide with the @ref indirect class template)
    template <basic_fixed_string S, uint<4> N, typename T>
    using indirect_message_field = field<S, N, embedded_message_coder<T>, boxed>;
}

#endif //PROTOPUF_MESSAGE_H
