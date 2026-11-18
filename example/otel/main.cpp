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

//
// OpenTelemetry trace collection & OTLP encoding with protopuf
// ------------------------------------------------------------
// Collects a small trace (root span + children with attributes, including nested
// array/kvlist values through the recursive `AnyValue` oneof), encodes it into an
// OTLP `TracesData` wire message, hex-dumps the bytes, then decodes them back and
// verifies the round trip. Nothing is sent anywhere.
//

#include <cassert>
#include <cstdio>
#include <vector>

#include <protopuf/reflection.h>

#include "tracer.h"

using namespace otel;
using pp::operator""_f;

// interned string table: created once, shared everywhere (refcount bumps only)
namespace strings {
    const auto attr_http_method = shared_string::literal("http.request.method");
    const auto attr_http_route  = shared_string::literal("http.route");
    const auto attr_http_status = shared_string::literal("http.response.status_code");
    const auto attr_db_system   = shared_string::literal("db.system");
    const auto attr_db_statement= shared_string::literal("db.statement");
    const auto attr_retries     = shared_string::literal("retry.delays_ms");
    const auto attr_peer        = shared_string::literal("peer.info");
    const auto peer_host        = shared_string::literal("host");
    const auto peer_port        = shared_string::literal("port");

    const auto span_handle_req  = shared_string::literal("GET /api/users/{id}");
    const auto span_db_query    = shared_string::literal("SELECT users");
    const auto span_render      = shared_string::literal("render response");
}

static void hex_dump(pp::bytes b) {
    for (std::size_t i = 0; i < b.size(); i += 16) {
        std::printf("%08zx  ", i);
        for (std::size_t j = 0; j < 16; ++j) {
            if (i + j < b.size())
                std::printf("%02x ", static_cast<unsigned>(b[i + j]));
            else
                std::printf("   ");
            if (j == 7) std::printf(" ");
        }
        std::printf(" |");
        for (std::size_t j = 0; j < 16 && i + j < b.size(); ++j) {
            const char c = static_cast<char>(b[i + j]);
            std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        std::printf("|\n");
    }
}

int main() {
    // ---- 1. collect a trace -------------------------------------------------

    tracer t(shared_string::literal("user-service"),
             shared_string::literal("example.instrumentation"),
             shared_string::literal("1.2.3"));

    {
        auto root = t.start_span(strings::span_handle_req);
        root.set_kind(SpanKind::server)
            .set_attribute(strings::attr_http_method, shared_string::literal("GET"))
            .set_attribute(strings::attr_http_route, shared_string::literal("/api/users/{id}"))
            .set_attribute(strings::attr_http_status, std::int64_t(200));

        {
            auto db = t.start_span(strings::span_db_query, root);
            db.set_kind(SpanKind::client)
              .set_attribute(strings::attr_db_system, shared_string::literal("postgresql"))
              .set_attribute(strings::attr_db_statement,
                             shared_string::literal("SELECT * FROM users WHERE id = $1"))
              // a nested *array* attribute (recursive AnyValue -> ArrayValue -> AnyValue)
              .set_attribute(strings::attr_retries,
                             make_array(std::int64_t(10), std::int64_t(20), std::int64_t(40)))
              .set_status(StatusCode::ok);
        } // db.end()

        {
            auto render = t.start_span(strings::span_render, root);
            // a nested *kvlist* attribute (AnyValue -> KeyValueList -> KeyValue -> AnyValue)
            KeyValueList peer;
            peer["values"_f].push_back(
                make_kv(strings::peer_host, make_value(shared_string::literal("10.0.0.7"))));
            peer["values"_f].push_back(
                make_kv(strings::peer_port, make_value(std::int64_t(5432))));
            render.set_attribute(strings::attr_peer, make_value(std::move(peer)));
        } // render.end()

        root.set_status(StatusCode::ok);
    } // root.end()

    std::printf("collected %zu spans; interned span name '%.*s' is shared, use_count-free "
                "(literal) storage\n\n",
                t.finished_count(),
                int(strings::span_db_query.size()), strings::span_db_query.data());

    // ---- 2. build the OTLP message and encode it ----------------------------

    const TracesData data = t.build_traces_data();

    const std::size_t wire_size = pp::skipper<pp::message_coder<TracesData>>::encode_skip(data);
    std::vector<std::byte> buffer(wire_size);

    const auto encode_result = pp::message_coder<TracesData>::encode<pp::safe_mode>(data, buffer);
    assert(encode_result.has_value());
    const std::size_t written = pp::begin_diff(*encode_result, buffer);
    assert(written == wire_size);

    std::printf("encoded OTLP TracesData: %zu bytes\n", written);
    hex_dump(pp::bytes(buffer.data(), written));

    // ---- 3. decode it back and verify ---------------------------------------

    const auto decode_result = pp::message_coder<TracesData>::decode<pp::safe_mode>(
        pp::bytes(buffer.data(), written));
    assert(decode_result.has_value());
    const TracesData& decoded = decode_result->first;

    // deep equality over the whole tree (oneofs, boxes, views and shared strings included);
    // note the decoded byte views point *into* `buffer` — no id bytes were copied
    assert(decoded == data);

    const auto& spans =
        decoded["resource_spans"_f][0]["scope_spans"_f][0]["spans"_f];
    std::printf("\ndecoded %zu spans (byte-identical round trip):\n", spans.size());

    for (const auto& s : spans) {
        std::printf("  span '%.*s'\n",
                    int(s["name"_f]->size()), s["name"_f]->data());
        // decoded strings alias the wire buffer? no — shared_string copies on decode;
        // but the *views* (ids) alias it:
        const auto id = s["span_id"_f].value();
        assert(id.data() >= buffer.data() && id.data() < buffer.data() + buffer.size());

        for (const auto& kv : s["attributes"_f]) {
            const auto& v = kv["value"_f];
            assert(v.has_value());
            std::printf("    %-28.*s : ",
                        int(kv["key"_f]->size()), kv["key"_f]->data());
            (*v)["value"_f].visit(pp::overloaded{
                [](const shared_string& x) { std::printf("\"%.*s\"\n", int(x.size()), x.data()); },
                [](bool x) { std::printf(x ? "true\n" : "false\n"); },
                [](std::int64_t x) { std::printf("%lld\n", static_cast<long long>(x)); },
                [](double x) { std::printf("%g\n", x); },
                [](const ArrayValue& x) { std::printf("[array, %zu values]\n", x["values"_f].size()); },
                [](const KeyValueList& x) { std::printf("{kvlist, %zu entries}\n", x["values"_f].size()); },
                [](std::span<const std::byte> x) { std::printf("<%zu bytes>\n", x.size()); },
                [](std::monostate) { std::printf("<empty>\n"); },
            });
        }
    }

    std::printf("\nround trip OK\n");
    return 0;
}
