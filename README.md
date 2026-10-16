# ![puffer](asset/puffer.png) Protocol Puffers
[![Codacy](https://api.codacy.com/project/badge/Grade/31e7d1d7bcbe43959aaec3b86093b843)](https://app.codacy.com/gh/PragmaTwice/protopuf?utm_source=github.com&utm_medium=referral&utm_content=PragmaTwice/protopuf&utm_campaign=Badge_Grade)
[![Github Actions](https://github.com/PragmaTwice/protopuf/workflows/BuildAndTest/badge.svg)](https://github.com/PragmaTwice/protopuf/actions)
[![Vcpkg Port](https://img.shields.io/vcpkg/v/protopuf)](https://github.com/microsoft/vcpkg/blob/master/ports/protopuf/vcpkg.json)
[![Conan Center](https://img.shields.io/conan/v/protopuf)](https://conan.io/center/recipes/protopuf)
[![Codecov](https://codecov.io/gh/PragmaTwice/protopuf/branch/master/graph/badge.svg?token=4EPLZ6Z4J5)](https://codecov.io/gh/PragmaTwice/protopuf)

*A little, highly templated, and protobuf-compatible serialization/deserialization library written in C++20*

:closed_book: [Documentation](https://protopuf.surge.sh)

## Requirements

- a compiler and a standard library implementation with C++20 support
    - GCC 11 or above, or
    - Clang 12 or above, or
    - MSVC 14.29 (Visual Studio 2019 Version 16.9) or above
    - Apple Clang 17 (Xcode 16.4) or above
- CMake 3
- GoogleTest (optional, for unit tests)
- vcpkg (optional, `vcpkg install protopuf` to install)

## Features

- Data structures are described using type in C++ instead of DSLs like the Protocol Buffer Language (`.proto`)
- Fully compatible with encoding of the Protocol Buffers, capable of mutual serialization/deserialization
- Extensive compile-time operations aimed to improving run-time performance
- `oneof` fields, wire-compatible with plain optional fields and constructible with an active member (enabling `const`/`constexpr` messages)
- Self-referencing (recursive) message types via `pp::indirect`, a C++26 `std::indirect`-style heap value wrapper
- Zero-copy `string`/`bytes` fields over views and user-defined string types
- Uniform reflection (`for_each`/`fold`) across plain fields and oneof members, usable in constant expressions

## An Example
For the following data structure described using `.proto`:
```proto
message Student {
    uint32 id = 1;
    string name = 3;
}

message Class {
    string name = 8;
    repeated Student students = 3;
}
```
We can use *protopuf* to describe it as C++ types:
```c++
using namespace pp;

using Student = message<
    uint32_field<"id", 1>,
    string_field<"name", 3>
>;

using Class = message<
    string_field<"name", 8>,
    message_field<"students", 3, Student, repeated>
>;
```
Subsequently, both serialization and deserialization become so easy to do:
```c++
// serialization
Student twice {123, "twice"}, tom{456, "tom"}, jerry{123456, "jerry"};
Class myClass {"class 101", {tom, jerry}};
myClass["students"_f].push_back(twice);

array<byte, 64> buffer{};
auto result = message_coder<Class>::encode(myClass, buffer);
assert(result.has_value());
const auto& bufferEnd = *result;
assert(begin_diff(bufferEnd, buffer) == 45);

// deserialization
auto result2 = message_coder<Class>::decode(buffer);
assert(result2.has_value());
const auto& [yourClass, bufferEnd2] = *result2;
assert(yourClass["name"_f] == "class 101");
assert(yourClass["students"_f][2]["name"_f] == "twice");
assert(yourClass["students"_f][2]["id"_f] == 123);
assert(yourClass["students"_f][1] == (Student{123456, "jerry"}));
assert(yourClass == myClass);
assert(begin_diff(bufferEnd2, bufferEnd) == 0);
```
More examples can be found in our test cases ([test/message.cpp](https://github.com/PragmaTwice/protopuf/blob/master/test/message.cpp)).

## Supported Field Types
Category|	Supported Types
--------|------------------
Varint  | int32, int64, uint32, uint64, sint32, sint64, bool, enum
64-bit  | fixed64, sfixed64, double
Length-delimited| string, bytes, embedded messages, packed repeated fields, string-like/view types (zero-copy), indirect message fields (wire-identical to embedded)
32-bit 	| fixed32, sfixed32, float
oneof   | oneof_field members can be of any singular (non-repeated) type mentioned above

## Advanced Features

### `oneof` fields

`pp::oneof_field<Name, Members...>` models the protobuf `oneof`: a named group of alternative fields of which at most one is set, stored internally as a private `std::variant` whose alternatives carry the member identity, so the active member is dispatched with a single `std::visit`. Members are ordinary singular field declarations identified by name and number (several members may share a value type). A oneof has no wire representation of its own - the active member is encoded exactly as if it were a plain singular field, so messages using oneofs are byte-for-byte compatible with messages declaring the same members as plain optional fields; on decoding, the last member seen on the wire wins.

```c++
using AnyValue = pp::message<pp::oneof_field<"value",
    pp::string_field<"string_value", 1>,
    pp::bool_field<"bool_value", 2>,
    pp::int64_field<"int_value", 3>>>;

AnyValue v;
v["value"_f].emplace<"int_value">(42);            // set a member by name (or number)
v["value"_f].holds<"int_value">();                // true
v["value"_f].get<"int_value">();                  // 42
v["value"_f].visit(pp::overloaded{ /* ... */ });  // std::visit over the alternatives

// members are also addressable directly on the message, by their own name or
// number, through a zero-copy field proxy (the oneof's own name yields the oneof):
v["int_value"_f].has_value();                     // true
*v["int_value"_f];                                // 42, read in place
v["string_value"_f] = "abc";                      // writes through: activates string_value
v[3_i].has_value();                               // false now (int_value was replaced)
```

A oneof can also be constructed with a member already active, selected by a *name or number tag* (unambiguous even when members share a value type), so whole messages can be built in one expression and declared `const` - or `constexpr` when the active member's value type is a literal type:

```c++
constexpr AnyValue cv{pp::oneof_field<"value", /*...*/>("int_value"_f, 42)};
static_assert(cv["value"_f].get<"int_value">() == 42);
```

Being wire-identical, the two shapes are also *assignable* to each other: a message declaring plain optional fields can be assigned to a message grouping (some of) the same field numbers into oneofs, and vice versa, matched by field number (a plain field set in the source activates the corresponding oneof member - the last one in member declaration order wins, mirroring the wire; an inactive member reads as an empty plain field):

```c++
using PlainValue = pp::message<
    pp::string_field<"string_value", 1>,
    pp::bool_field<"bool_value", 2>,
    pp::int64_field<"int_value", 3>>;

PlainValue p;
p["int_value"_f] = 42;
AnyValue v;
v = p;                                            // plain -> oneof
assert(v["value"_f].holds<"int_value">());
p = v;                                            // oneof -> plain
```

### Self-referencing (recursive) message types

Messages can refer to themselves (and to each other, mutually), which protobuf allows but value-semantic C++ types normally cannot express. Declare a struct deriving from its message type; repeated self-references are held by value (`std::vector` supports incomplete element types), and singular ones live on the heap in a `pp::indirect` via `pp::indirect_message_field`:

```c++
struct Node : pp::message<
    pp::string_field<"name", 1>,
    pp::message_field<"children", 2, Node, pp::repeated>,  // repeated self-reference
    pp::indirect_message_field<"parent", 3, Node>> {       // singular self-reference (heap)
    using message_type::message_type;
};
```

`pp::indirect<T>` is a nullable, deep-copying, heap-allocating value wrapper usable with incomplete `T` - the same idea as C++26 `std::indirect`, after which it is named (unlike `std::indirect`, it is nullable by design, since an empty `indirect` models an unset optional field). An `indirect_message_field` is wire-identical to a plain singular `message_field`.

### Zero-copy string-like fields

`pp::string_like_coder` encodes protobuf `string`/`bytes` fields from *any* contiguous, sized range of one-byte elements, and decodes into any type constructible from `(const CharT*, size)` - wire-compatible with `pp::string_field` in both directions:

- `pp::string_view_field` / `pp::bytes_view_field` encode directly from external storage and decode as views *into the wire buffer* - no excessive copies of characters in either direction (`pp::bytes_view` is a `std::span<const std::byte>` with content equality, so messages holding byte views stay comparable);
- `pp::string_like_field<Name, N, YourString>` plugs in user-defined string types (interned, reference-counted, rope, ...) - anything satisfying `pp::string_buildable` works for both encoding and decoding, letting shared strings flow from application storage to the wire without ever materializing a `std::string`.

### Reflection over oneofs, `constexpr` messages

`message::for_each` and `message::fold` treat a oneof as its member fields: each member is presented as an ordinary singular field through a zero-copy `pp::oneof_member_ref` proxy (the active member set, all others empty - the protobuf `oneof` model), so generic callbacks constrained on `pp::field_c` work uniformly across plain fields and oneof members. Reads reference the value stored inside the oneof; mutable proxies write through immediately. `message::for_each_component` / `message::fold_component` iterate raw components (whole oneofs) instead. All of it - construction, member access, iteration, folding - is usable in constant expressions (ref to the standard-library requirement in Known issues below).

### An Example
An example for all advanced features combined in one project can be found in the example/otel folder.


## Known issues
- There is [a known bug](https://developercommunity2.visualstudio.com/t/Wrong-compile-error-in-MSVC:-identifier-/1270794) in Visual Studio 2019 Version 16.8 related to template parameter lists of lambda expressions, which can produce a wrong compilation error while compiling protopuf
- Although class type in NTTP ([P0732R2](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0732r2.pdf)) is implemented in GCC 10, there is a CTAD bug ([PR96331](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96331), exists until GCC 10.2) to reject valid NTTP usage, which prevent protopuf to compile successfully
- Clang 14's frontend crashes (segmentation fault, not a diagnostic) on the *name-tag UDL* form of oneof construction — `OneofValue("int_value"_f, 42)` — when it appears in certain contexts, such as templated function bodies (e.g. `TYPED_TEST`s) or alongside other template-heavy code; the bug is fixed in Clang 15. **Workaround:** use one of the semantically identical spellings, which compile everywhere: the named factory `OneofValue::of<"int_value">(42)`, the number tag `OneofValue(3_i, 42)`, or an explicit tag object `OneofValue(pp::constant<pp::basic_fixed_string("int_value")>{}, 42)`. The bundled tests use `of<...>` in the affected contexts. Note also that Clang before 17 cannot parse libstdc++ 14 headers at all (unrelated to protopuf); pair older Clang with libstdc++ 12/13
- Compile-time (`constexpr`) messages require a standard library with complete C++20 constexpr support for `std::string`/`std::vector` (and constexpr destruction of variant alternatives, [P2231](https://wg21.link/p2231)) — e.g. libstdc++ from GCC 12. On GCC 11 the library and all run-time functionality work; only compile-time message constants are unavailable

## Acknowledgement
- to @dngrudin for the contribution of out-of-bounds checking and unsafe/safe mode and for the review of the advanced features
- to [JetBrains](https://www.jetbrains.com/?from=protopuf) for its [Open Source License](https://www.jetbrains.com/community/opensource/?from=protopuf) of All Products Pack
