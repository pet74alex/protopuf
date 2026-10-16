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

#ifndef OTEL_TRACER_H
#define OTEL_TRACER_H

//
// A deliberately small span *collection* API (this example only collects and encodes;
// it does not talk to a collector).
//
// Zero-copy properties:
//  * span names / attribute keys / resource strings are `shared_string`s — starting a
//    span with an interned name is a refcount bump, and encoding reads the shared
//    characters in place;
//  * trace/span ids live inside `collected_span` as fixed arrays; the protopuf `Span`
//    message aliases them through `bytes_view_field`s, so building the OTLP message
//    copies no id bytes either.
//

#include <chrono>
#include <cstdint>
#include <deque>
#include <random>
#include <span>
#include <vector>

#include "otel_trace.h"

namespace otel {

    using trace_id_t = std::array<std::byte, 16>;
    using span_id_t = std::array<std::byte, 8>;

    /// a finished span, owning its ids; strings are shared, not copied
    struct collected_span {
        trace_id_t trace_id{};
        span_id_t span_id{};
        span_id_t parent_span_id{}; // all-zero ⇒ root span
        shared_string name;
        SpanKind kind = SpanKind::internal;
        std::uint64_t start_time_unix_nano = 0;
        std::uint64_t end_time_unix_nano = 0;
        std::vector<KeyValue> attributes;
        StatusCode status_code = StatusCode::unset;
        shared_string status_message;

        bool is_root() const {
            return parent_span_id == span_id_t{};
        }
    };

    /// collects spans of a single trace and turns them into an OTLP `TracesData` message
    class tracer {
    public:
        explicit tracer(shared_string service_name,
                        shared_string scope_name,
                        shared_string scope_version)
            : service_name_(std::move(service_name)),
              scope_name_(std::move(scope_name)),
              scope_version_(std::move(scope_version)),
              rng_(std::random_device{}()) {
            fill_random(trace_id_);
        }

        const trace_id_t& trace_id() const { return trace_id_; }

        /// an in-flight span handle; `end()` (or destruction) files it with the tracer
        class span {
        public:
            span(const span&) = delete;
            span& operator=(const span&) = delete;
            span(span&& o) noexcept : t_(o.t_), s_(std::move(o.s_)), ended_(o.ended_) {
                o.ended_ = true;
            }

            ~span() { end(); }

            const span_id_t& id() const { return s_.span_id; }

            /// attach an attribute (key is shared, value is any `AnyValue`)
            span& set_attribute(shared_string key, AnyValue value) {
                s_.attributes.push_back(make_kv(std::move(key), std::move(value)));
                return *this;
            }

            template <typename V>
            span& set_attribute(shared_string key, V&& value) {
                return set_attribute(std::move(key), make_value(std::forward<V>(value)));
            }

            span& set_status(StatusCode code, shared_string message = {}) {
                s_.status_code = code;
                s_.status_message = std::move(message);
                return *this;
            }

            span& set_kind(SpanKind kind) {
                s_.kind = kind;
                return *this;
            }

            /// finish the span and hand it to the tracer (idempotent)
            void end() {
                if (ended_) return;
                ended_ = true;
                s_.end_time_unix_nano = now_unix_nano();
                t_.finished_.push_back(std::move(s_));
            }

        private:
            friend class tracer;

            span(tracer& t, shared_string name, const span_id_t* parent) : t_(t) {
                s_.trace_id = t.trace_id_;
                t.fill_random(s_.span_id);
                if (parent) s_.parent_span_id = *parent;
                s_.name = std::move(name);
                s_.start_time_unix_nano = now_unix_nano();
            }

            tracer& t_;
            collected_span s_;
            bool ended_ = false;
        };

        span start_span(shared_string name) {
            return span(*this, std::move(name), nullptr);
        }

        span start_span(shared_string name, const span& parent) {
            return span(*this, std::move(name), &parent.id());
        }

        std::size_t finished_count() const { return finished_.size(); }

        /// assemble the OTLP `TracesData` message over the collected spans.
        ///
        /// the returned message *aliases* the tracer's storage (ids via byte views,
        /// strings via shared refcounts) — encode it while the tracer is alive.
        TracesData build_traces_data() const {
            TracesData data;

            ResourceSpans rs;

            Resource resource;
            resource["attributes"_f].push_back(
                make_kv(shared_string::literal("service.name"), make_value(service_name_)));
            rs["resource"_f] = std::move(resource);

            ScopeSpans ss;
            InstrumentationScope scope;
            scope["name"_f] = scope_name_;
            scope["version"_f] = scope_version_;
            ss["scope"_f] = std::move(scope);

            for (const collected_span& c : finished_) {
                Span s;
                s["trace_id"_f] = std::span<const std::byte>(c.trace_id);
                s["span_id"_f] = std::span<const std::byte>(c.span_id);
                if (!c.is_root()) {
                    s["parent_span_id"_f] = std::span<const std::byte>(c.parent_span_id);
                }
                s["name"_f] = c.name;
                s["kind"_f] = c.kind;
                s["start_time_unix_nano"_f] = c.start_time_unix_nano;
                s["end_time_unix_nano"_f] = c.end_time_unix_nano;
                s["attributes"_f] = c.attributes;
                if (c.status_code != StatusCode::unset) {
                    Status status;
                    status["message"_f] = c.status_message;
                    status["code"_f] = c.status_code;
                    s["status"_f] = std::move(status);
                }
                ss["spans"_f].push_back(std::move(s));
            }

            rs["scope_spans"_f].push_back(std::move(ss));
            data["resource_spans"_f].push_back(std::move(rs));
            return data;
        }

    private:
        static std::uint64_t now_unix_nano() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        template <std::size_t N>
        void fill_random(std::array<std::byte, N>& a) {
            std::uniform_int_distribution<unsigned> d(0, 255);
            for (auto& b : a) b = static_cast<std::byte>(d(rng_));
        }

        shared_string service_name_, scope_name_, scope_version_;
        trace_id_t trace_id_{};
        std::mt19937_64 rng_;

        /// deque: finished spans never move, so the byte views handed to the
        /// protopuf message stay valid as more spans are collected
        std::deque<collected_span> finished_;
    };

}

#endif // OTEL_TRACER_H
