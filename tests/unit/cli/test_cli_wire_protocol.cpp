#include <gtest/gtest.h>
#include <hpactor/cli.pb.h>

TEST(CliWireProtocol, CliCommandDefaultConstruction) {
    hpactor::cli::CliCommand cmd;
    EXPECT_TRUE(cmd.path().empty());
    EXPECT_TRUE(cmd.rpc_method().empty());
}

TEST(CliWireProtocol, CliCommandSetPath) {
    hpactor::cli::CliCommand cmd;
    cmd.set_path("system/stats");
    EXPECT_EQ(cmd.path(), "system/stats");
}

TEST(CliWireProtocol, CliCommandSetRpcMethod) {
    hpactor::cli::CliCommand cmd;
    cmd.set_rpc_method("inspect");
    EXPECT_EQ(cmd.rpc_method(), "inspect");
}

TEST(CliWireProtocol, CliCommandParamsRoundTrip) {
    hpactor::cli::CliCommand cmd;
    (*cmd.mutable_params())["format"] = "json";
    (*cmd.mutable_params())["verbose"] = "true";
    EXPECT_EQ(cmd.params().at("format"), "json");
    EXPECT_EQ(cmd.params().at("verbose"), "true");
}

TEST(CliWireProtocol, CliCommandArgsRoundTrip) {
    hpactor::cli::CliCommand cmd;
    cmd.add_args("42");
    cmd.add_args("--verbose");
    ASSERT_EQ(cmd.args_size(), 2);
    EXPECT_EQ(cmd.args(0), "42");
    EXPECT_EQ(cmd.args(1), "--verbose");
}

TEST(CliWireProtocol, CliResponseDefaultConstruction) {
    hpactor::cli::CliResponse resp;
    EXPECT_FALSE(resp.is_error());
    EXPECT_FALSE(resp.is_structured());
}

TEST(CliWireProtocol, CliResponseStructuredFlag) {
    hpactor::cli::CliResponse resp;
    resp.set_is_structured(true);
    resp.set_content_type("application/x-protobuf");
    resp.set_payload("binary-data");
    EXPECT_TRUE(resp.is_structured());
    EXPECT_EQ(resp.content_type(), "application/x-protobuf");
}

TEST(CliWireProtocol, CliCommandRoundTrip) {
    hpactor::cli::CliCommand original;
    original.set_path("actor/42/show");
    (*original.mutable_params())["format"] = "json";
    original.add_args("42");

    std::string wire = original.SerializeAsString();
    hpactor::cli::CliCommand decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.path(), "actor/42/show");
    EXPECT_EQ(decoded.params().at("format"), "json");
    ASSERT_EQ(decoded.args_size(), 1);
    EXPECT_EQ(decoded.args(0), "42");
}

TEST(CliWireProtocol, CliResponseRoundTrip) {
    hpactor::cli::CliResponse original;
    original.set_content_type("text/plain");
    original.set_payload("Actor 42: Worker-1 (Running)");
    original.set_is_error(false);

    std::string wire = original.SerializeAsString();
    hpactor::cli::CliResponse decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.content_type(), "text/plain");
    EXPECT_EQ(decoded.payload(), "Actor 42: Worker-1 (Running)");
    EXPECT_FALSE(decoded.is_error());
}

TEST(CliWireProtocol, CliCommandRpcDispatchMode) {
    hpactor::cli::CliCommand cmd;
    cmd.set_rpc_method("inspect");
    cmd.set_rpc_request("serialized-proto-bytes");
    EXPECT_EQ(cmd.rpc_method(), "inspect");
    EXPECT_EQ(cmd.rpc_request(), "serialized-proto-bytes");
    EXPECT_TRUE(cmd.path().empty());
}
