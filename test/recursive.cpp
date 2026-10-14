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

#include <array>
#include <string>

#include "test_fixture.h"

using namespace pp;
using namespace std;

// pp::indirect<T> basics ------------------------------------------------------------

TEST(indirect, basics) {
    pp::indirect<int> b;
    ASSERT_FALSE(b.has_value());
    ASSERT_FALSE(b);
    ASSERT_EQ(b, pp::indirect<int>{});

    b = 42;
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(*b, 42);
    ASSERT_EQ(b.value(), 42);
    ASSERT_EQ(b, 42);
    ASSERT_NE(b, pp::indirect<int>{});

    // deep copy
    pp::indirect<int> c = b;
    ASSERT_EQ(c, b);
    *c = 7;
    ASSERT_EQ(*b, 42);
    ASSERT_EQ(*c, 7);
    ASSERT_NE(c, b);

    // move leaves source empty
    pp::indirect<int> d = std::move(c);
    ASSERT_EQ(*d, 7);
    ASSERT_FALSE(c.has_value()); // NOLINT(bugprone-use-after-move)

    d.emplace(100);
    ASSERT_EQ(*d, 100);

    d.reset();
    ASSERT_FALSE(d.has_value());

    static_assert(pp::is_indirect<pp::indirect<int>>);
    static_assert(!pp::is_indirect<int>);
    static_assert(pp::indirect_c<pp::indirect<std::string>>);
}

TEST(indirect, incomplete_type) {
    // indirect must be usable with an incomplete type at declaration time
    struct node {
        int value = 0;
        pp::indirect<node> next;

        bool operator==(const node& o) const {
            return value == o.value && next == o.next;
        }
    };

    node a{1, {}};
    a.next.emplace(node{2, {}});
    a.next->next.emplace(node{3, {}});

    node b = a; // deep copy of the whole chain
    ASSERT_EQ(a, b);
    b.next->next->value = 4;
    ASSERT_NE(a, b);
    ASSERT_EQ(a.next->next->value, 3);
}

// recursive messages -------------------------------------------------------

// a tree node referencing itself both through a repeated field (children)
// and a singular indirect (heap) field (a detached subtree), exercising both storage forms
struct Node : message<
    string_field<"name", 1>,
    message_field<"children", 2, Node, repeated>,
    indirect_message_field<"detached", 3, Node>,
    uint32_field<"weight", 4>> {
    using message_type::message_type; // inherit constructors
};

static_assert(message_c<Node>);
static_assert(message_c<Node::message_type>);
static_assert(std::same_as<message_base_t<Node>, Node::message_type>);

template<typename T>
using recursive_coding = test_fixture<T>;
TYPED_TEST_SUITE(recursive_coding, coder_mode_types, test_name_generator);

TYPED_TEST(recursive_coding, tree_round_trip) {
    Node root;
    root["name"_f] = "root";
    root["weight"_f] = 1u;

    Node child1;
    child1["name"_f] = "c1";
    child1["weight"_f] = 2u;

    Node grandchild;
    grandchild["name"_f] = "g1";
    grandchild["weight"_f] = 3u;
    child1["children"_f].push_back(grandchild);

    Node child2;
    child2["name"_f] = "c2";

    Node detached;
    detached["name"_f] = "aside";
    child2["detached"_f] = detached;

    root["children"_f].push_back(child1);
    root["children"_f].push_back(child2);

    array<byte, 256> buf{};
    bytes rest;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<Node>::encode<TypeParam>(root, buf), rest));
    const auto n = begin_diff(rest, buf);
    ASSERT_GT(n, 0u);
    ASSERT_EQ(skipper<message_coder<Node>>::encode_skip(root), n);

    decode_value<Node> dv;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<Node>::decode<TypeParam>(bytes(buf.begin(), n)), dv));

    const Node& decoded = dv.first;
    ASSERT_EQ(decoded, root);
    ASSERT_EQ(decoded["name"_f], "root");
    ASSERT_EQ(decoded["children"_f].size(), 2u);
    ASSERT_EQ(decoded["children"_f][0]["children"_f][0]["name"_f], "g1");
    ASSERT_TRUE(decoded["children"_f][1]["detached"_f].has_value());
    ASSERT_EQ((*decoded["children"_f][1]["detached"_f])["name"_f], "aside");
    ASSERT_FALSE(decoded["detached"_f].has_value());
}

TYPED_TEST(recursive_coding, deep_chain_round_trip) {
    // a linked list through the indirect singular field
    constexpr unsigned depth = 20;

    Node head;
    head["name"_f] = "n0";
    head["weight"_f] = 0u;
    Node* cur = &head;
    for (unsigned i = 1; i < depth; ++i) {
        Node next;
        next["name"_f] = "n" + std::to_string(i);
        next["weight"_f] = i;
        (*cur)["detached"_f] = std::move(next);
        cur = &*(*cur)["detached"_f];
    }

    array<byte, 1024> buf{};
    bytes rest;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<Node>::encode<TypeParam>(head, buf), rest));
    const auto n = begin_diff(rest, buf);

    decode_value<Node> dv;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<Node>::decode<TypeParam>(bytes(buf.begin(), n)), dv));
    ASSERT_EQ(dv.first, head);

    const Node* p = &dv.first;
    for (unsigned i = 0; i < depth; ++i) {
        ASSERT_EQ((*p)["name"_f], "n" + std::to_string(i));
        ASSERT_EQ((*p)["weight"_f], i);
        if (i + 1 < depth) {
            ASSERT_TRUE((*p)["detached"_f].has_value());
            p = &*(*p)["detached"_f];
        } else {
            ASSERT_FALSE((*p)["detached"_f].has_value());
        }
    }
}

TYPED_TEST(recursive_coding, indirect_field_wire_identical_to_message_field) {
    // an indirect (heap-stored) embedded field must be wire-identical to a plain (optional) embedded field
    using Inner = message<uint32_field<"id", 1>>;
    using BoxedMsg = message<indirect_message_field<"inner", 1, Inner>, uint32_field<"x", 2>>;
    using PlainMsg = message<message_field<"inner", 1, Inner>, uint32_field<"x", 2>>;

    BoxedMsg bm;
    bm["inner"_f] = Inner{9u};
    bm["x"_f] = 5u;

    PlainMsg pm;
    pm["inner"_f] = Inner{9u};
    pm["x"_f] = 5u;

    array<byte, 32> bb{}, pb{};
    bytes rb, rp;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<BoxedMsg>::encode<TypeParam>(bm, bb), rb));
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<PlainMsg>::encode<TypeParam>(pm, pb), rp));

    const auto nb = begin_diff(rb, bb), np = begin_diff(rp, pb);
    ASSERT_EQ(nb, np);
    ASSERT_TRUE(equal(bb.begin(), bb.begin() + nb, pb.begin()));

    // cross-decode
    decode_value<PlainMsg> dp;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<PlainMsg>::decode<TypeParam>(bytes(bb.begin(), nb)), dp));
    ASSERT_EQ(dp.first, pm);

    decode_value<BoxedMsg> db;
    ASSERT_TRUE(TypeParam::get_value_from_result(
        message_coder<BoxedMsg>::decode<TypeParam>(bytes(pb.begin(), np)), db));
    ASSERT_EQ(db.first, bm);
}
