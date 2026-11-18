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

#ifndef PROTOPUF_ONEOF_H
#define PROTOPUF_ONEOF_H

#include <variant>

#include "coder.h"
#include "varint.h"
#include "skip.h"
#include "field.h"

namespace pp {

    /// @brief A concept satisfied while `T` describes a member (an alternative) of a oneof.
    ///
    /// Oneof members are declared with the very same aliases as ordinary message fields
    /// (`string_field`, `uint32_field`, `message_field`, ...), except that they must be
    /// @ref singular (`oneof` cannot contain repeated fields in protobuf). Only the static
    /// metadata of the field type (name, number, key, coder) is used: the storage of all
    /// members is provided by a single variant owned by @ref oneof_field.
    template <typename T>
    concept oneof_member_c = field_c<T> && (T::attr == singular);

    template <typename F, typename... Ms>
    struct oneof_alternative_index_impl;

    template <typename F, typename... Ms>
    struct oneof_alternative_index_impl<F, F, Ms...> : std::integral_constant<std::size_t, 1> {};

    template <typename F, typename M, typename... Ms>
    struct oneof_alternative_index_impl<F, M, Ms...>
        : std::integral_constant<std::size_t, 1 + oneof_alternative_index_impl<F, Ms...>::value> {};

    /// The (1-based) index of member `F` in the member list `Ms...`
    /// (index 0 of the underlying storage variant is `std::monostate`, meaning "no member set")
    template <typename F, typename... Ms>
    constexpr inline std::size_t oneof_alternative_index = oneof_alternative_index_impl<F, Ms...>::value;

    template <typename... M>
    constexpr bool oneof_members_unique() {
        constexpr uint<4> numbers[] = { M::number... };
        for (std::size_t i = 0; i < sizeof...(M); ++i)
            for (std::size_t j = i + 1; j < sizeof...(M); ++j)
                if (numbers[i] == numbers[j]) return false;
        return true;
    }

    /// @brief The storage of one member (alternative) of a @ref oneof_field: the member's
    /// value, wrapped in a type carrying the member's static identity (@ref field_type).
    ///
    /// Wrapping each alternative in a *distinct* type (rather than storing the raw value
    /// types, several of which may legally coincide) lets `std::visit` over the storage
    /// variant recover the member - not just its value - so the active member is
    /// dispatched by a single visitation instead of an index comparison chain.
    template <oneof_member_c M>
    struct oneof_member_value {
        /// the member field type this alternative belongs to
        using field_type = M;
        /// the value type of the member
        using value_type = typename M::coder::value_type;

        value_type value;

        /// construct the member's value in place from `args`
        template <typename... Args>
            requires std::constructible_from<value_type, Args&&...>
        constexpr explicit oneof_member_value(std::in_place_t, Args&&... args)
            : value(std::forward<Args>(args)...) {}

        /// two alternatives are equal when their values are (user-provided rather than
        /// defaulted, so the comparison is instantiated lazily - a defaulted comparison
        /// would be defined eagerly with the enclosing oneof, breaking self-referencing
        /// member value types that are still incomplete at that point)
        constexpr bool operator==(const oneof_member_value& other) const {
            return value == other.value;
        }
    };

    /// A concept satisfied while a value declared as `V` can be initialized from `U` without
    /// narrowing - the same guard the `std::variant` converting operations use (P0608R3), so
    /// that e.g. `bool` alternatives are only selected by genuinely boolean arguments
    template <typename U, typename V>
    concept oneof_non_narrowing_into = requires (U&& u) {
        std::type_identity_t<V[]>{std::forward<U>(u)};
    };

    /// @brief An overload set used to select a oneof member from a *value* the way
    /// `std::variant`'s converting constructor selects an alternative: by ordinary
    /// overload resolution over `select(member_value_type)` functions, each of which
    /// participates only if the initialization would not be narrowing.
    ///
    /// If no member (or more than one member equally well) accepts the value, the call
    /// expression is ill-formed - which the converting operations of @ref oneof_field
    /// turn into a clean constraint failure.
    template <oneof_member_c...>
    struct oneof_member_overloads {
        static void select();  // sentinel: never viable for the one-value calls below
    };

    template <oneof_member_c First, oneof_member_c... Rest>
    struct oneof_member_overloads<First, Rest...> : oneof_member_overloads<Rest...> {
        using oneof_member_overloads<Rest...>::select;

        template <typename U>
            requires oneof_non_narrowing_into<U, typename First::coder::value_type>
        static std::type_identity<First> select(typename First::coder::value_type, std::type_identity<U>);
    };

    /// Checks whether the type is a *member selection tag* (a member name or number tag,
    /// as produced by the `_f` / `_i` literals), used to keep the tagged constructors of
    /// @ref oneof_field out of the reach of its value-converting constructor
    template <typename T>
    constexpr inline bool is_oneof_member_tag = false;

    template <basic_fixed_string S>
    constexpr inline bool is_oneof_member_tag<constant<S>> = true;

    template <uint<4> N>
    constexpr inline bool is_oneof_member_tag<std::integral_constant<uint<4>, N>> = true;

    /// @brief Checks whether the field number `N` of member-shaped field `F` is covered by
    /// message type `Other` with a singular source whose value can initialize `F`'s value
    /// type - the per-member building block of cross-shape message assignment
    /// (ref to `message::operator=` for structurally equivalent messages).
    ///
    /// The check is staged on `Other::contains_number<N>` first, so `Other`'s accessors are
    /// never even named for numbers the other message does not declare.
    template <typename Other, typename F,
              bool = std::remove_cvref_t<Other>::template contains_number<F::number>>
    constexpr inline bool oneof_member_assignable_from_message = false;

    template <typename Other, typename F>
    constexpr inline bool oneof_member_assignable_from_message<Other, F, true> =
        requires (const std::remove_cvref_t<Other>& o) {
            { o.template get<F::number>().has_value() } -> std::convertible_to<bool>;
            requires std::constructible_from<typename F::coder::value_type,
                decltype(o.template get<F::number>().value())>;
        };

    /// @brief A lightweight, zero-copy *proxy* presenting one member of a @ref oneof_field
    /// as an ordinary singular field.
    ///
    /// @param M the member field type whose identity (name/number/coder/attribute) the
    ///          proxy carries
    /// @param O the (possibly const) @ref oneof_field type the proxy refers into
    ///
    /// The proxy holds only a reference to the oneof: reading the active member's value
    /// through it (`value()`, `operator*`, `operator->`) yields a reference to the value
    /// stored *inside the oneof* - nothing is copied. It models the optional-like surface
    /// of a singular field (`has_value()`, `operator bool`, `reset()`, assignment,
    /// `emplace()`), carries the member's static metadata (`name`, `number`, `key`,
    /// `attr`, `coder`), and satisfies @ref field_c, so generic callbacks written against
    /// fields work unchanged.
    ///
    /// Mutations write through to the oneof *immediately*: assigning or emplacing makes
    /// `M` the active member (replacing whatever was active before), `reset()` empties the
    /// whole oneof if `M` is the active member (and is a no-op otherwise). Accessing
    /// `value()` while `M` is not active throws `std::bad_variant_access`.
    ///
    /// Like any reference, a proxy must not outlive the oneof it refers into.
    template <oneof_member_c M, typename O>
    class oneof_member_ref {
    public:
        /// the member field type this proxy stands in for
        using field_type = M;
        /// the value type of the member
        using value_type = typename M::coder::value_type;

        static constexpr auto name = M::name;
        static constexpr auto number = M::number;
        static constexpr auto key = M::key;
        static constexpr auto attr = M::attr;
        using coder = typename M::coder;

        constexpr explicit oneof_member_ref(O& oneof) : oneof_(&oneof) {}

        constexpr oneof_member_ref(const oneof_member_ref&) = default;

        /// proxies are references: they cannot be reseated (assign a *value* instead)
        oneof_member_ref& operator=(const oneof_member_ref&) = delete;

        /// whether this member is the active one
        [[nodiscard]] constexpr bool has_value() const {
            return oneof_->template holds<M::name>();
        }

        constexpr explicit operator bool() const { return has_value(); }

        /// reference to the value stored inside the oneof (zero-copy);
        /// throws `std::bad_variant_access` if this member is not the active one
        constexpr decltype(auto) value() const {
            return oneof_->template raw_value<M>();
        }

        constexpr decltype(auto) operator*() const { return value(); }

        constexpr auto operator->() const { return std::addressof(value()); }

        /// assign a value, making this member the active one (writes through immediately);
        /// assigns in place when already active, otherwise activates the member
        template <typename U>
            requires (!std::is_const_v<O>)
                && std::constructible_from<value_type, U&&>
                && std::assignable_from<value_type&, U&&>
        constexpr oneof_member_ref& operator=(U&& u) {
            if (has_value()) {
                oneof_->template raw_value<M>() = std::forward<U>(u);
            } else {
                oneof_->template raw_emplace<M>(std::forward<U>(u));
            }
            return *this;
        }

        /// construct the value in place, making this member the active one
        template <typename... Args>
            requires (!std::is_const_v<O>) && std::constructible_from<value_type, Args&&...>
        constexpr value_type& emplace(Args&&... args) {
            return oneof_->template raw_emplace<M>(std::forward<Args>(args)...);
        }

        /// empty the whole oneof if this member is the active one; no-op otherwise
        constexpr void reset() requires (!std::is_const_v<O>) {
            if (has_value()) {
                oneof_->reset();
            }
        }

    private:
        O* oneof_;
    };

    /// @brief The oneof field type: a named group of alternative fields of which at most one is set.
    ///
    /// @param S the name of the oneof itself (i.e. `"value"`)
    /// @param M the member (alternative) field types, i.e. `string_field<"string_value", 2>`
    ///
    /// The storage is a *private* `std::variant` data member holding `std::monostate`
    /// ("no member set") or one @ref oneof_member_value per member; the variant interface
    /// is deliberately **not** exposed - a oneof is accessed through its own name/number
    /// based API (`holds`, `get`, `get_if`, `emplace`, `visit`, ...), and the active member
    /// is dispatched internally with `std::visit`.
    ///
    /// On the wire a oneof has **no representation of its own**: whichever member is active is
    /// encoded exactly as if it were a plain singular field with the same field number, so a
    /// message using a oneof is byte-for-byte compatible with a message declaring the same
    /// members as plain optional fields.
    template <basic_fixed_string S, oneof_member_c... M>
        requires (sizeof...(M) > 0) && (oneof_members_unique<M...>())
    struct oneof_field {

        /// name of the oneof
        static constexpr basic_fixed_string name = S;

        /// type of name of the oneof
        using name_type = decltype(name);

        /// the number of members
        static constexpr uint<4> size = sizeof...(M);

        constexpr oneof_field() = default;

        /// get the member (field) type by its field name
        template <basic_fixed_string MS>
        using member_by_name = field_name_selector<MS, M...>;

        /// get the member (field) type by its field number
        template <uint<4> N>
        using member_by_number = field_number_selector<N, M...>;

        /// @brief Construct the oneof with the member named `MS` active, its value
        /// constructed in place from `args`, i.e. `OneofValue("int_value"_f, 42u)`.
        ///
        /// Selecting the member by *name tag* (rather than by value type, as the
        /// value-converting constructor below does) is unambiguous even when several
        /// members share the same value type, and - being `constexpr` - lets whole
        /// messages containing oneofs be built in a single expression and declared
        /// `const` or even `constexpr`.
        template <basic_fixed_string MS, typename... Args>
            requires std::constructible_from<typename member_by_name<MS>::coder::value_type, Args...>
        constexpr oneof_field(constant<MS>, Args&&... args)
            : storage_(std::in_place_index<index_of<member_by_name<MS>>>,
                       std::in_place, std::forward<Args>(args)...) {}

        /// same as above, selecting the member by its field *number* tag,
        /// i.e. `OneofValue(3_i, 42u)`
        template <uint<4> N, typename... Args>
            requires std::constructible_from<typename member_by_number<N>::coder::value_type, Args...>
        constexpr oneof_field(std::integral_constant<uint<4>, N>, Args&&... args)
            : storage_(std::in_place_index<index_of<member_by_number<N>>>,
                       std::in_place, std::forward<Args>(args)...) {}

        /// @brief Named factory, equivalent to the name-tagged constructor:
        /// `OneofValue::of<"int_value">(42u)`
        template <basic_fixed_string MS, typename... Args>
            requires std::constructible_from<typename member_by_name<MS>::coder::value_type, Args...>
        static constexpr oneof_field of(Args&&... args) {
            return oneof_field(constant<MS>{}, std::forward<Args>(args)...);
        }

        /// the member selected for a value of type `U` by @ref oneof_member_overloads
        /// (variant-style converting selection: best overload-resolution match among the
        /// members' value types, narrowing conversions excluded)
        template <typename U>
        using member_for = typename decltype(oneof_member_overloads<M...>::select(
            std::declval<U>(), std::type_identity<U>{}))::type;

        /// whether a value of type `U` unambiguously selects exactly one member
        template <typename U>
        static constexpr bool selects_member = requires {
            oneof_member_overloads<M...>::select(std::declval<U>(), std::type_identity<U>{});
        };

        /// @brief Converting constructor: construct the oneof from a member *value*,
        /// activating the member selected the way `std::variant`'s converting
        /// constructor selects an alternative (ill-formed when no member or several
        /// members match, e.g. when members share the value type - use the tagged
        /// constructors or @ref of in that case).
        template <typename U>
            requires (!std::same_as<std::remove_cvref_t<U>, oneof_field>)
                && (!is_oneof_member_tag<std::remove_cvref_t<U>>)
                && (!std::same_as<std::remove_cvref_t<U>, std::monostate>)
                && selects_member<U>
                && std::constructible_from<typename member_for<U>::coder::value_type, U&&>
        constexpr oneof_field(U&& u)
            : storage_(std::in_place_index<index_of<member_for<U>>>,
                       std::in_place, std::forward<U>(u)) {}

        /// converting assignment, selecting the member exactly like the converting
        /// constructor: assigns in place when the selected member is already active,
        /// otherwise activates it
        template <typename U>
            requires (!std::same_as<std::remove_cvref_t<U>, oneof_field>)
                && (!is_oneof_member_tag<std::remove_cvref_t<U>>)
                && (!std::same_as<std::remove_cvref_t<U>, std::monostate>)
                && selects_member<U>
                && std::constructible_from<typename member_for<U>::coder::value_type, U&&>
                && std::assignable_from<typename member_for<U>::coder::value_type&, U&&>
        constexpr oneof_field& operator=(U&& u) {
            using F = member_for<U>;
            if (storage_.index() == index_of<F>) {
                std::get<index_of<F>>(storage_).value = std::forward<U>(u);
            } else {
                storage_.template emplace<index_of<F>>(std::in_place, std::forward<U>(u));
            }
            return *this;
        }

        /// the (1-based) storage alternative index of member `F`
        template <oneof_member_c F>
        static constexpr std::size_t index_of = oneof_alternative_index<F, M...>;

        /// whether no member is set
        [[nodiscard]] constexpr bool empty() const {
            return storage_.index() == 0 || storage_.valueless_by_exception();
        }

        /// clear the oneof, so that no member is set afterwards
        constexpr void reset() {
            storage_ = std::monostate{};
        }

        /// whether the member named `MS` is the currently set one
        template <basic_fixed_string MS>
        [[nodiscard]] constexpr bool holds() const {
            return storage_.index() == index_of<member_by_name<MS>>;
        }

        /// whether the member with field number `N` is the currently set one
        template <uint<4> N>
        [[nodiscard]] constexpr bool holds() const {
            return storage_.index() == index_of<member_by_number<N>>;
        }

        /// get the value of the member named `MS` (throws `std::bad_variant_access` if it is not set)
        template <basic_fixed_string MS>
        constexpr decltype(auto) get() {
            return raw_value<member_by_name<MS>>();
        }

        template <basic_fixed_string MS>
        constexpr decltype(auto) get() const {
            return raw_value<member_by_name<MS>>();
        }

        /// get the value of the member with field number `N` (throws `std::bad_variant_access` if not set)
        template <uint<4> N>
        constexpr decltype(auto) get() {
            return raw_value<member_by_number<N>>();
        }

        template <uint<4> N>
        constexpr decltype(auto) get() const {
            return raw_value<member_by_number<N>>();
        }

        /// get a pointer to the value of the member named `MS`, or `nullptr` if it is not set
        template <basic_fixed_string MS>
        constexpr typename member_by_name<MS>::coder::value_type* get_if() noexcept {
            auto* alt = std::get_if<index_of<member_by_name<MS>>>(&storage_);
            return alt ? std::addressof(alt->value) : nullptr;
        }

        template <basic_fixed_string MS>
        constexpr const typename member_by_name<MS>::coder::value_type* get_if() const noexcept {
            const auto* alt = std::get_if<index_of<member_by_name<MS>>>(&storage_);
            return alt ? std::addressof(alt->value) : nullptr;
        }

        /// set the member named `MS`, constructing its value in place from `args...`
        template <basic_fixed_string MS, typename... Args>
        constexpr decltype(auto) emplace(Args&&... args) {
            return raw_emplace<member_by_name<MS>>(std::forward<Args>(args)...);
        }

        /// set the member with field number `N`, constructing its value in place from `args...`
        template <uint<4> N, typename... Args>
        constexpr decltype(auto) emplace(Args&&... args) {
            return raw_emplace<member_by_number<N>>(std::forward<Args>(args)...);
        }

        /// two oneofs are equal when they hold the same member (or are both empty)
        /// and the held values compare equal (user-provided rather than defaulted, for
        /// lazy instantiation - ref to `oneof_member_value::operator==`)
        constexpr bool operator==(const oneof_field& other) const {
            return storage_ == other.storage_;
        }

        constexpr bool operator!=(const oneof_field& other) const {
            return !(*this == other);
        }

        /// @brief apply visitor `f` to the value of the active member, or to
        /// `std::monostate` when no member is set (a `std::visit` over the members'
        /// values - `f` must accept every member's value type and `std::monostate`)
        template <typename F>
        constexpr decltype(auto) visit(F&& f) const {
            return std::visit([&f]<typename A>(A&& alt) -> decltype(auto) {
                if constexpr (std::same_as<std::remove_cvref_t<A>, std::monostate>) {
                    return std::forward<F>(f)(std::forward<A>(alt));
                } else {
                    return std::forward<F>(f)(std::forward<A>(alt).value);
                }
            }, storage_);
        }

        template <typename F>
        constexpr decltype(auto) visit(F&& f) {
            return std::visit([&f]<typename A>(A&& alt) -> decltype(auto) {
                if constexpr (std::same_as<std::remove_cvref_t<A>, std::monostate>) {
                    return std::forward<F>(f)(std::forward<A>(alt));
                } else {
                    return std::forward<F>(f)(std::forward<A>(alt).value);
                }
            }, storage_);
        }

        /// @brief Left-fold `f` over every *member* of the oneof, each presented as an
        /// ordinary singular field through a zero-copy @ref oneof_member_ref proxy
        /// (ref to @ref for_each_member): `acc = f(acc, member)` in declaration order,
        /// where the accumulator type may change from step to step.
        template <typename U, typename F>
        constexpr decltype(auto) fold_members(U&& init, F&& f) const {
            return fold_members_impl<M...>(std::forward<U>(init), f);
        }

        /// mutable overload of @ref fold_members : the proxies write through to the
        /// oneof immediately, as in the mutable @ref for_each_member
        template <typename U, typename F>
        constexpr decltype(auto) fold_members(U&& init, F&& f) {
            return fold_members_impl<M...>(std::forward<U>(init), f);
        }

    private:
        template <oneof_member_c First, oneof_member_c... Rest, typename U, typename F>
        constexpr decltype(auto) fold_members_impl(U&& acc, F& f) const {
            const oneof_member_ref<First, const oneof_field> member(*this);
            if constexpr (sizeof...(Rest) == 0) {
                return f(std::forward<U>(acc), member);
            } else {
                return fold_members_impl<Rest...>(f(std::forward<U>(acc), member), f);
            }
        }

        template <oneof_member_c First, oneof_member_c... Rest, typename U, typename F>
        constexpr decltype(auto) fold_members_impl(U&& acc, F& f) {
            oneof_member_ref<First, oneof_field> member(*this);
            if constexpr (sizeof...(Rest) == 0) {
                return f(std::forward<U>(acc), member);
            } else {
                return fold_members_impl<Rest...>(f(std::forward<U>(acc), member), f);
            }
        }

    public:

        /// @brief Apply `f` to every *member* of the oneof, each presented as an
        /// ordinary singular field through a zero-copy @ref oneof_member_ref proxy:
        /// the active member's proxy is set (reads reference the value stored inside
        /// the oneof, no copies), all other members' proxies are empty.
        ///
        /// This is how `message::for_each` iterates a oneof, making it uniform with
        /// plain fields: a oneof behaves like its member fields, of which at most
        /// one is set - exactly the protobuf `oneof` model.
        template <typename F>
            requires (std::invocable<F, const oneof_member_ref<M, const oneof_field>&> && ...)
        constexpr void for_each_member(F&& f) const {
            (for_each_member_impl<M>(f), ...);
        }

        /// @brief Mutable overload of @ref for_each_member : the proxies write
        /// through to the oneof *immediately*.
        ///
        /// Assigning a value to a member's proxy makes that member the active one
        /// (if several members are set during one pass, the last one in declaration
        /// order wins); `reset()` on the active member's proxy empties the oneof.
        template <typename F>
            requires (std::invocable<F, oneof_member_ref<M, oneof_field>&> && ...)
        constexpr void for_each_member(F&& f) {
            (for_each_member_impl<M>(f), ...);
        }

    private:
        template <oneof_member_c OM, typename F>
        constexpr void for_each_member_impl(F& f) const {
            const oneof_member_ref<OM, const oneof_field> member(*this);
            f(member);
        }

        template <oneof_member_c OM, typename F>
        constexpr void for_each_member_impl(F& f) {
            oneof_member_ref<OM, oneof_field> member(*this);
            f(member);
        }

    public:

        /// @brief encode the active member (if any) as `key varint + value`, exactly like
        /// a plain singular field
        ///
        /// The active member is dispatched with a single `std::visit` over the storage:
        /// each alternative carries its member type (@ref oneof_member_value), so the
        /// visitor statically knows which member - key, coder - it is encoding.
        template <coder_mode Mode = safe_mode>
        constexpr encode_result<Mode> encode(bytes b) const {
            if (empty()) {
                return encode_result<Mode>{b};
            }

            return std::visit([b]<typename A>(const A& alternative) -> encode_result<Mode> {
                if constexpr (std::same_as<A, std::monostate>) {
                    return encode_result<Mode>{b};
                } else {
                    return encode_alternative<typename A::field_type, Mode>(alternative.value, b);
                }
            }, storage_);
        }

        /// the number of bytes @ref encode would produce, dispatched with `std::visit`
        /// like @ref encode
        [[nodiscard]] constexpr std::size_t encode_skip() const {
            if (empty()) {
                return 0;
            }

            return std::visit([]<typename A>(const A& alternative) -> std::size_t {
                if constexpr (std::same_as<A, std::monostate>) {
                    return 0;
                } else {
                    using F = typename A::field_type;
                    static_assert(coder<typename F::coder>,
                        "the coder of a oneof member must satisfy the pp::coder concept");

                    return skipper<varint_coder<uint<4>>>::encode_skip(F::key)
                         + skipper<typename F::coder>::encode_skip(alternative.value);
                }
            }, storage_);
        }

        /// @brief register one decoder per member into a message decode map
        /// (each member key dispatches into this oneof's storage)
        ///
        /// @param Mode the coder mode
        /// @param Msg the enclosing message type
        /// @param Result the result type of the decode map functions
        /// @param Map the decode map type (`unordered_map<uint<4>, function<Result(Msg&, bytes)>>`)
        template <coder_mode Mode, typename Msg, typename Result, typename Map>
        static void insert_decoders(Map& map) {
            (map.emplace(M::key, [](Msg& m, bytes b) -> Result {
                static_assert(coder<typename M::coder>,
                    "the coder of a oneof member must satisfy the pp::coder concept");

                decode_value<typename M::coder::value_type> decode_v;
                if (Mode::get_value_from_result(M::coder::template decode<Mode>(b), decode_v)) {
                    m.template get<S>().template emplace<M::name>(std::move(decode_v.first));
                    return Result{decode_v.second};
                }
                return Result{};
            }), ...);
        }

        /// whether this oneof can be set from message type `Other` by matching member
        /// field numbers (every member number covered by a singular, value-compatible
        /// source), ref to @ref assign_from_message
        template <typename Other>
        static constexpr bool assignable_from_message =
            (oneof_member_assignable_from_message<Other, M> && ...);

        /// @brief Set this oneof from the equivalent fields of another message: the oneof
        /// is emptied, then every member whose field number is *set* in `other` is
        /// activated in declaration order - so if several source fields are set, the
        /// last member in declaration order wins, mirroring the wire format, where the
        /// last member seen wins. Backs the structural `message::operator=`.
        template <typename Other>
            requires assignable_from_message<Other>
        constexpr void assign_from_message(const Other& other) {
            reset();
            (assign_member_from_message<M>(other), ...);
        }

    private:
        template <oneof_member_c F, typename Other>
        constexpr void assign_member_from_message(const Other& other) {
            decltype(auto) src = other.template get<F::number>();
            if (src.has_value()) {
                raw_emplace<F>(src.value());
            }
        }

        template <oneof_member_c F, coder_mode Mode>
        static constexpr encode_result<Mode> encode_alternative(
                const typename F::coder::value_type& value, bytes b) {
            static_assert(coder<typename F::coder>,
                "the coder of a oneof member must satisfy the pp::coder concept");

            bytes safe_b;
            if (Mode::get_value_from_result(varint_coder<uint<4>>::template encode<Mode>(F::key, b), safe_b)) {
                return F::coder::template encode<Mode>(value, safe_b);
            }
            return {};
        }

        /// the storage: `std::monostate` ("no member set") or one wrapped value per member;
        /// private by design - the variant interface is not part of the oneof's interface
        std::variant<std::monostate, oneof_member_value<M>...> storage_;

        // the member proxies read from / write through to the storage
        template <oneof_member_c, typename> friend class oneof_member_ref;

        /// reference to the value of member `F` (throws `std::bad_variant_access` if `F`
        /// is not the active member)
        template <oneof_member_c F>
        constexpr typename F::coder::value_type& raw_value() {
            return std::get<index_of<F>>(storage_).value;
        }

        template <oneof_member_c F>
        constexpr const typename F::coder::value_type& raw_value() const {
            return std::get<index_of<F>>(storage_).value;
        }

        /// activate member `F`, constructing its value in place from `args...`
        template <oneof_member_c F, typename... Args>
        constexpr typename F::coder::value_type& raw_emplace(Args&&... args) {
            return storage_.template emplace<index_of<F>>(
                std::in_place, std::forward<Args>(args)...).value;
        }
    };

    /// Checks whether the type is a @ref oneof_field type
    template <typename>
    constexpr inline bool is_oneof = false;

    template <basic_fixed_string S, oneof_member_c... M>
    constexpr inline bool is_oneof<oneof_field<S, M...>> = true;

    /// A concept satisfied while type `T` is a @ref oneof_field type
    template <typename T>
    concept oneof_c = is_oneof<T>;

    /// @brief A concept satisfied while `T` can be a direct component of a @ref message:
    /// either a plain @ref field_c or a @ref oneof_c
    template <typename T>
    concept message_component_c = field_c<T> || oneof_c<T>;

    /// a @ref oneof_member_ref models the field concept: generic callbacks constrained
    /// on @ref field_c accept the proxies handed out by `message::for_each`
    template <oneof_member_c M, typename O>
    constexpr inline bool is_field<oneof_member_ref<M, O>> = true;

    template <typename F, typename C>
    constexpr inline bool component_invocable_impl = std::invocable<F, C>;

    template <typename F, basic_fixed_string S, oneof_member_c... M>
    constexpr inline bool component_invocable_impl<F, const oneof_field<S, M...>&> =
        (std::invocable<F, const oneof_member_ref<M, const oneof_field<S, M...>>&> && ...);

    template <typename F, basic_fixed_string S, oneof_member_c... M>
    constexpr inline bool component_invocable_impl<F, oneof_field<S, M...>&> =
        (std::invocable<F, oneof_member_ref<M, oneof_field<S, M...>>&> && ...);

    /// @brief A concept satisfied while `F` is applicable to message component reference
    /// `C` through `message::for_each`: invocable with the component itself for plain
    /// fields, or with *every member field* for oneofs (whose members are visited
    /// individually, ref to `oneof_field::for_each_member`)
    template <typename F, typename C>
    concept component_invocable = component_invocable_impl<F, C>;

    /// Checks whether a oneof is empty (no member set); overload of @ref empty_field for oneofs
    template <oneof_c T>
    constexpr bool empty_field(const T& v) {
        return v.empty();
    }

    /// Merge a oneof into another oneof: overwrite if the source has a member set
    template <merge_mode mode = merge_mode{}, oneof_c D, typename Src>
        requires std::same_as<std::remove_cvref_t<Src>, D>
    constexpr void merge_field(D& f, Src&& v) {
        if constexpr (mode.singular_mode == merge_mode::singular::override) {
            if (!v.empty()) {
                f = std::forward<Src>(v);
            }
        } else {
            if (!v.empty() && f.empty()) {
                f = std::forward<Src>(v);
            }
        }
    }

    /// Checks whether a message component `C` covers the field number `N`
    /// (a field covers its own number; a oneof covers the numbers of all of its members)
    template <uint<4> N, message_component_c C>
    constexpr inline bool component_contains_number = (N == C::number);

    template <uint<4> N, basic_fixed_string S, oneof_member_c... M>
    constexpr inline bool component_contains_number<N, oneof_field<S, M...>> = ((N == M::number) || ...);

    /// Checks whether a message component `C` matches the name `S2`
    /// (a field matches its own name; a oneof matches its own name and the names of all of its members)
    template <basic_fixed_string S2, message_component_c C>
    constexpr inline bool component_matches_name = (S2 == C::name);

    template <basic_fixed_string S2, basic_fixed_string S, oneof_member_c... M>
    constexpr inline bool component_matches_name<S2, oneof_field<S, M...>> = (S2 == S) || ((S2 == M::name) || ...);

    template <uint<4>, message_component_c...>
    struct component_number_selector_impl {
        using type = field_not_found;
    };

    template <uint<4> I, message_component_c C, message_component_c... D>
    struct component_number_selector_impl<I, C, D...> {
        using type = std::conditional_t<component_contains_number<I, C>, C,
            typename component_number_selector_impl<I, D...>::type>;
    };

    /// Find the first component in `C...` which covers the field number `I`
    /// (for a oneof: the oneof containing a member with that number)
    template <uint<4> I, message_component_c... C>
    using component_number_selector = typename component_number_selector_impl<I, C...>::type;

    template <basic_fixed_string, message_component_c...>
    struct component_name_selector_impl {
        using type = field_not_found;
    };

    template <basic_fixed_string S, message_component_c C, message_component_c... D>
    struct component_name_selector_impl<S, C, D...> {
        using type = std::conditional_t<component_matches_name<S, C>, C,
            typename component_name_selector_impl<S, D...>::type>;
    };

    /// Find the first component in `C...` which matches the name `S`
    /// (for a oneof: matched by the oneof name or any of its member names)
    template <basic_fixed_string S, message_component_c... C>
    using component_name_selector = typename component_name_selector_impl<S, C...>::type;

    /// @brief The *member-shaped field* of component `C` covering field number `N`:
    /// the component itself for a plain field, or the member field with that number
    /// for a oneof - i.e. the field type whose value actually travels under number `N`
    template <uint<4> N, message_component_c C>
    struct component_member_field_impl {
        using type = C;
    };

    template <uint<4> N, basic_fixed_string S, oneof_member_c... M>
    struct component_member_field_impl<N, oneof_field<S, M...>> {
        using type = field_number_selector<N, M...>;
    };

    template <uint<4> N, message_component_c C>
    using component_member_field = typename component_member_field_impl<N, C>::type;
}

#endif //PROTOPUF_ONEOF_H
