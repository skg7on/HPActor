// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/actor/actor_type_registry.hpp>
#include <hpactor/actor/spawn.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/msg/request_handle.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

#include <gtest/gtest.h>

#include <string>

namespace hpactor {
namespace {

// ============================================================================
// AsyncActor lifecycle (RequestHandle<ActorRef>)
// ============================================================================

TEST(SpawnUnitTest, AsyncActorDefaultConstruction) {
    AsyncActor async;
    EXPECT_FALSE(async.ready());
}

TEST(SpawnUnitTest, AsyncActorResolveWithResult) {
    AsyncActor async;

    // Resolve the async handle with a success result
    async.resolve(result<ActorRef>::make(ActorRef{}));
    EXPECT_TRUE(async.ready());

    auto r = async.get();
    EXPECT_TRUE(r.ok());
}

TEST(SpawnUnitTest, AsyncActorResolveWithError) {
    AsyncActor async;

    async.resolve_error(error(errors::timeout, "spawn timed out"));
    EXPECT_TRUE(async.ready());

    auto r = async.get();
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), errors::timeout);
}

TEST(SpawnUnitTest, AsyncActorCancel) {
    AsyncActor async;

    async.cancel();
    EXPECT_TRUE(async.ready());

    auto r = async.get();
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), errors::cancelled);
}

TEST(SpawnUnitTest, AsyncActorCancelAfterResolveIsNoOp) {
    AsyncActor async;

    async.resolve(result<ActorRef>::make(ActorRef{}));
    EXPECT_TRUE(async.ready());

    // Cancel after resolve should be a no-op — original result preserved
    async.cancel();
    auto r = async.get();
    EXPECT_TRUE(r.ok());
}

// ============================================================================
// SpawnRequest protobuf roundtrip
// ============================================================================

TEST(SpawnUnitTest, SpawnRequestProtobufRoundtrip) {
    ProtoTypeRegistry registry;
    registry.register_system_types();

    // Build a SpawnRequest via protobuf
    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("test_actor");
    pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
    pb_req.set_serialized_args("hello=world");

    auto* sup = pb_req.mutable_supervisor();
    auto* sup_global = sup->mutable_global_addr();
    sup_global->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000001);
    sup_global->mutable_endpoint()->mutable_ipv4()->set_port(8080);
    auto* sup_local = sup_global->mutable_local_addr();
    sup_local->set_actor_type(10);
    sup_local->set_actor_id(42);
    sup_local->set_incarnation(1);

    // Serialize
    StreamBuffer encoded = registry.serialize(pb_req);
    EXPECT_FALSE(encoded.empty());

    // Deserialize
    auto decoded = registry.deserialize(TypeTag::SpawnRequestTag, encoded);
    ASSERT_NE(decoded, nullptr);
    auto* decoded_req =
        static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    EXPECT_EQ(decoded_req->actor_type_name(), "test_actor");
    EXPECT_EQ(decoded_req->serialized_args(), "hello=world");
    EXPECT_EQ(decoded_req->supervisor().global_addr().local_addr().actor_id(), 42u);
}

TEST(SpawnUnitTest, SpawnResponseProtobufRoundtrip) {
    ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnResponseMessage pb_resp;
    auto* addr = pb_resp.mutable_actor_addr();
    auto* addr_global = addr->mutable_global_addr();
    addr_global->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000003);
    addr_global->mutable_endpoint()->mutable_ipv4()->set_port(9090);
    auto* addr_local = addr_global->mutable_local_addr();
    addr_local->set_actor_type(20);
    addr_local->set_actor_id(200);
    addr_local->set_incarnation(2);
    pb_resp.set_error_code(spawn_errors::success);

    StreamBuffer encoded = registry.serialize(pb_resp);
    ASSERT_FALSE(encoded.empty());

    auto decoded = registry.deserialize(TypeTag::SpawnResponseTag, encoded);
    ASSERT_NE(decoded, nullptr);
    auto* decoded_resp =
        static_cast<::hpactor::SpawnResponseMessage*>(decoded.get());
    EXPECT_EQ(decoded_resp->actor_addr().global_addr().local_addr().actor_id(),
              200u);
    EXPECT_EQ(decoded_resp->error_code(), spawn_errors::success);
}

// ============================================================================
// ActorTypeRegistry registration
// ============================================================================

TEST(SpawnUnitTest, ActorTypeRegistryRegisterType) {
    ActorTypeRegistry registry;

    // Initially empty
    EXPECT_FALSE(registry.has("test_actor"));

    // Register a factory type
    registry.register_factory("test_actor", nullptr);
    EXPECT_TRUE(registry.has("test_actor"));
}

TEST(SpawnUnitTest, ActorTypeRegistryTypeIdMapping) {
    ActorTypeRegistry registry;

    registry.register_factory("echo", nullptr);
    registry.register_factory("calculator", nullptr);

    EXPECT_TRUE(registry.has("echo"));
    EXPECT_TRUE(registry.has("calculator"));
    EXPECT_FALSE(registry.has("nonexistent"));

    // Type IDs should be distinct
    ActorType id1 = registry.type_id("echo");
    ActorType id2 = registry.type_id("calculator");
    EXPECT_NE(id1, ActorType{0});
    EXPECT_NE(id2, ActorType{0});
    EXPECT_NE(id1, id2);

    // Reverse lookup
    EXPECT_EQ(registry.type_name(id1), "echo");
    EXPECT_EQ(registry.type_name(id2), "calculator");
}

TEST(SpawnUnitTest, ActorTypeRegistrySpawnUnknownType) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    ActorTypeRegistry registry;
    StreamBuffer empty_args;
    auto r = registry.spawn(system, "nonexistent", empty_args, TypeTag::Invalid);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.is_error());
}

// ============================================================================
// SpawnReceiver message handling
// ============================================================================

TEST(SpawnUnitTest, SpawnErrorCodeMapping) {
    // Verify spawn error codes map to correct failure reasons
    EXPECT_EQ(failure_reason(spawn_errors::success), FailureReason::Unknown);
    EXPECT_EQ(failure_reason(spawn_errors::unknown_type), FailureReason::NoRoute);
    EXPECT_EQ(failure_reason(spawn_errors::deserialization_failed),
              FailureReason::SerializationError);
    EXPECT_EQ(failure_reason(spawn_errors::node_unreachable),
              FailureReason::NodeUnavailable);
    EXPECT_EQ(failure_reason(spawn_errors::timeout), FailureReason::Timeout);
    EXPECT_EQ(failure_reason(spawn_errors::spawn_receiver_not_running),
              FailureReason::ActorNotReady);

    // Unknown spawn error code maps to SpawnFailed
    EXPECT_EQ(failure_reason(999u), FailureReason::SpawnFailed);
}

// ============================================================================
// Remote spawn configuration
// ============================================================================

TEST(SpawnUnitTest, SpawnRequestStructDefaultValues) {
    SpawnRequest req{};
    EXPECT_TRUE(req.actor_type_name.empty());
    EXPECT_EQ(req.args_type, TypeTag::Invalid);
    EXPECT_TRUE(req.serialized_args.empty());
}

TEST(SpawnUnitTest, SpawnResponseStructDefaultValues) {
    SpawnResponse resp;
    EXPECT_EQ(resp.error_code, 0u);
}

TEST(SpawnUnitTest, RegisterAndLookupMultipleTypes) {
    ActorTypeRegistry registry;

    registry.register_factory("a", nullptr);
    registry.register_factory("b", nullptr);
    registry.register_factory("c", nullptr);

    EXPECT_TRUE(registry.has("a"));
    EXPECT_TRUE(registry.has("b"));
    EXPECT_TRUE(registry.has("c"));

    // Type IDs should all be distinct and non-zero
    ActorType id_a = registry.type_id("a");
    ActorType id_b = registry.type_id("b");
    ActorType id_c = registry.type_id("c");

    EXPECT_NE(id_a, ActorType{0});
    EXPECT_NE(id_b, ActorType{0});
    EXPECT_NE(id_c, ActorType{0});
    EXPECT_NE(id_a, id_b);
    EXPECT_NE(id_b, id_c);
    EXPECT_NE(id_a, id_c);
}

// ============================================================================
// Spawn with constructor args
// ============================================================================

TEST(SpawnUnitTest, SpawnRequestWithConstructorArgs) {
    ProtoTypeRegistry registry;
    registry.register_system_types();

    const std::string args_data = "key=value;name=test";

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("ArgsActor");
    pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
    pb_req.set_serialized_args(args_data);

    StreamBuffer encoded = registry.serialize(pb_req);
    ASSERT_FALSE(encoded.empty());

    auto decoded = registry.deserialize(TypeTag::SpawnRequestTag, encoded);
    ASSERT_NE(decoded, nullptr);
    auto* decoded_req =
        static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    EXPECT_EQ(decoded_req->actor_type_name(), "ArgsActor");
    EXPECT_EQ(decoded_req->args_type(), static_cast<uint32_t>(TypeTag::User));
    EXPECT_EQ(decoded_req->serialized_args(), args_data);
}

TEST(SpawnUnitTest, SpawnRequestSpawnErrorResponse) {
    ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnResponseMessage pb_resp;
    pb_resp.set_error_code(spawn_errors::unknown_type);

    StreamBuffer encoded = registry.serialize(pb_resp);
    ASSERT_FALSE(encoded.empty());

    auto decoded = registry.deserialize(TypeTag::SpawnResponseTag, encoded);
    ASSERT_NE(decoded, nullptr);
    auto* decoded_resp =
        static_cast<::hpactor::SpawnResponseMessage*>(decoded.get());
    EXPECT_EQ(decoded_resp->error_code(), spawn_errors::unknown_type);
}

} // anonymous namespace
} // namespace hpactor
