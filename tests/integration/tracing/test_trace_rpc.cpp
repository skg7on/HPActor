// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/trace_manager.hpp>

using namespace hpactor;

TEST(TraceRpcTest, PendingCallStoresTraceContext) {
    tracing::TraceConfig cfg;
    cfg.enabled = true;
    cfg.sampler = tracing::SamplerKind::kAlwaysOn;
    auto* memory = new tracing::MemoryExporter();
    tracing::TraceManager manager(cfg, nullptr,
                                  std::unique_ptr<tracing::SpanExporter>(memory));
    manager.start();

    TraceContext parent = manager.create_root_context("rpc-test");
    PendingCall call;
    call.msg_id = generate_message_id();
    call.target = ActorAddress{LocalEndpoint, ActorType{1}, ActorId{2}, 0};
    call.has_trace_context = true;
    call.trace_context = parent;
    EXPECT_TRUE(call.has_trace_context);
    EXPECT_EQ(call.trace_context.trace_id, parent.trace_id);
    manager.stop();
}