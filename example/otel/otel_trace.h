//   Copyright 2026 pet74alex
//
//   The OTLP message and field definitions in this file are derived from the
//   OpenTelemetry protocol buffer schemas
//   (https://github.com/open-telemetry/opentelemetry-proto):
//   Copyright The OpenTelemetry Authors, SPDX-License-Identifier: Apache-2.0.
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

#ifndef OTEL_TRACE_H
#define OTEL_TRACE_H

//
// A subset of the OpenTelemetry protocol (OTLP) trace schema, expressed as protopuf
// messages — field names and numbers follow the official .proto files:
//
//   opentelemetry/proto/common/v1/common.proto   (AnyValue, ArrayValue, KeyValueList,
//                                                 KeyValue, InstrumentationScope)
//   opentelemetry/proto/resource/v1/resource.proto (Resource)
//   opentelemetry/proto/trace/v1/trace.proto     (TracesData, ResourceSpans, ScopeSpans,
//                                                 Span, Status)
//
// so the encoded bytes are wire-compatible with any OTLP consumer.
//
// It exercises the three protopuf advanced features:
//   * `pp::oneof_field`             — AnyValue.value / Status is a oneof in spirit
//   * self-referencing messages     — AnyValue ⇄ ArrayValue ⇄ KeyValueList mutual recursion
//                                     (repeated fields hold incomplete types by value,
//                                      singular ones through `pp::indirect_message_field`)
//   * `pp::string_like_field`       — all strings are zero-copy `otel::shared_string`s,
//     / `pp::bytes_view_field`        trace/span ids are encoded straight out of the
//                                     span's own id storage
//

#include <protopuf/message.h>
#include <protopuf/string_like.h>

#include "shared_string.h"

namespace otel {

    using pp::operator""_f;

    // ---- common.proto -----------------------------------------------------

    struct AnyValue; // AnyValue ⇄ ArrayValue ⇄ KeyValueList are mutually recursive

    /// ArrayValue { repeated AnyValue values = 1; }
    ///
    /// `AnyValue` is still incomplete here: a *repeated* field may hold it by value,
    /// since `std::vector` supports incomplete element types.
    struct ArrayValue : pp::message<
        pp::message_field<"values", 1, AnyValue, pp::repeated>> {
        using message_type::message_type;
    };

    /// KeyValue { string key = 1; AnyValue value = 2; }
    ///
    /// a *singular* field of the incomplete `AnyValue` must live on the heap:
    /// `pp::indirect_message_field` stores it in a `pp::indirect` (wire-identical to a
    /// plain singular embedded field).
    struct KeyValue : pp::message<
        shared_string_field<"key", 1>,
        pp::indirect_message_field<"value", 2, AnyValue>> {
        using message_type::message_type;
    };

    /// KeyValueList { repeated KeyValue values = 1; }
    struct KeyValueList : pp::message<
        pp::message_field<"values", 1, KeyValue, pp::repeated>> {
        using message_type::message_type;
    };

    /// the `value` oneof of AnyValue, named so that values can be built in a
    /// single expression: `AnyValueOneof("int_value"_f, 42)`
    using AnyValueOneof = pp::oneof_field<"value",
        shared_string_field<"string_value", 1>,
        pp::bool_field<"bool_value", 2>,
        pp::int64_field<"int_value", 3>,
        pp::double_field<"double_value", 4>,
        pp::message_field<"array_value", 5, ArrayValue>,
        pp::message_field<"kvlist_value", 6, KeyValueList>,
        pp::bytes_view_field<"bytes_value", 7>>;

    /// AnyValue { oneof value { string string_value = 1; bool bool_value = 2;
    ///            int64 int_value = 3; double double_value = 4; ArrayValue array_value = 5;
    ///            KeyValueList kvlist_value = 6; bytes bytes_value = 7; } }
    struct AnyValue : pp::message<AnyValueOneof> {
        using message_type::message_type;
    };

    /// InstrumentationScope { string name = 1; string version = 2; }
    using InstrumentationScope = pp::message<
        shared_string_field<"name", 1>,
        shared_string_field<"version", 2>>;

    // ---- resource.proto ---------------------------------------------------

    /// Resource { repeated KeyValue attributes = 1; }
    using Resource = pp::message<
        pp::message_field<"attributes", 1, KeyValue, pp::repeated>>;

    // ---- trace.proto ------------------------------------------------------

    /// Status::StatusCode
    enum class StatusCode : pp::uint<1> {
        unset = 0,
        ok = 1,
        error = 2,
    };

    /// Status { string message = 2; StatusCode code = 3; }
    using Status = pp::message<
        shared_string_field<"message", 2>,
        pp::enum_field<"code", 3, StatusCode>>;

    /// Span::SpanKind
    enum class SpanKind : pp::uint<1> {
        unspecified = 0,
        internal = 1,
        server = 2,
        client = 3,
        producer = 4,
        consumer = 5,
    };

    /// Span (subset) {
    ///   bytes trace_id = 1; bytes span_id = 2; bytes parent_span_id = 4;
    ///   string name = 5; SpanKind kind = 6;
    ///   fixed64 start_time_unix_nano = 7; fixed64 end_time_unix_nano = 8;
    ///   repeated KeyValue attributes = 9; Status status = 15;
    /// }
    ///
    /// the id fields are `pp::bytes_view_field`s: they encode directly from (and, on
    /// decoding, point directly into) external storage — the span object owns fixed-size
    /// id arrays and the views alias them, so ids are never copied on the way to the wire.
    using Span = pp::message<
        pp::bytes_view_field<"trace_id", 1>,
        pp::bytes_view_field<"span_id", 2>,
        pp::bytes_view_field<"parent_span_id", 4>,
        shared_string_field<"name", 5>,
        pp::enum_field<"kind", 6, SpanKind>,
        pp::fixed64_field<"start_time_unix_nano", 7>,
        pp::fixed64_field<"end_time_unix_nano", 8>,
        pp::message_field<"attributes", 9, KeyValue, pp::repeated>,
        pp::message_field<"status", 15, Status>>;

    /// ScopeSpans { InstrumentationScope scope = 1; repeated Span spans = 2;
    ///              string schema_url = 3; }
    using ScopeSpans = pp::message<
        pp::message_field<"scope", 1, InstrumentationScope>,
        pp::message_field<"spans", 2, Span, pp::repeated>,
        shared_string_field<"schema_url", 3>>;

    /// ResourceSpans { Resource resource = 1; repeated ScopeSpans scope_spans = 2;
    ///                 string schema_url = 3; }
    using ResourceSpans = pp::message<
        pp::message_field<"resource", 1, Resource>,
        pp::message_field<"scope_spans", 2, ScopeSpans, pp::repeated>,
        shared_string_field<"schema_url", 3>>;

    /// TracesData { repeated ResourceSpans resource_spans = 1; }
    using TracesData = pp::message<
        pp::message_field<"resource_spans", 1, ResourceSpans, pp::repeated>>;

    // ---- convenience constructors for AnyValue ----------------------------

    // values are built in a single expression via the oneof's tagged constructor,
    // so results can be bound to const (and, for literal value types, constexpr)

    inline AnyValue make_value(shared_string s) {
        return AnyValue{AnyValueOneof("string_value"_f, std::move(s))};
    }

    inline AnyValue make_value(bool b) {
        return AnyValue{AnyValueOneof("bool_value"_f, b)};
    }

    inline AnyValue make_value(std::int64_t i) {
        return AnyValue{AnyValueOneof("int_value"_f, i)};
    }

    inline AnyValue make_value(double d) {
        return AnyValue{AnyValueOneof("double_value"_f, d)};
    }

    inline AnyValue make_value(ArrayValue a) {
        return AnyValue{AnyValueOneof("array_value"_f, std::move(a))};
    }

    inline AnyValue make_value(KeyValueList l) {
        return AnyValue{AnyValueOneof("kvlist_value"_f, std::move(l))};
    }

    template <typename... Vs>
    inline AnyValue make_array(Vs&&... vs) {
        ArrayValue a;
        /// Note: the below line is a workaround for MSVC rejecting UDLs inside this template context
        /// instead of a simpler one: (a["values"_f].push_back(make_value(std::forward<Vs>(vs))), ...);
        (a[pp::constant<pp::basic_fixed_string{"values"}>{}].push_back(make_value(std::forward<Vs>(vs))), ...);
        return make_value(std::move(a));
    }

    inline KeyValue make_kv(shared_string key, AnyValue value) {
        KeyValue kv;
        kv["key"_f] = std::move(key);
        kv["value"_f] = std::move(value);
        return kv;
    }

}

#endif // OTEL_TRACE_H
