#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/cli/json_formatter.hpp>
#include <hpactor/cli/tabular_formatter.hpp>
#include <hpactor/cli/output_formatter.hpp>

#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

static void test_pretty_key_value() {
    PrettyFormatter f;
    f.key_value({{"State", "Running"}, {"Uptime", "12m 03s"}});
    auto out = f.finalize();
    assert(out.find("State") != std::string::npos);
    assert(out.find("Running") != std::string::npos);
}

static void test_pretty_table() {
    PrettyFormatter f;
    f.table({"ID", "Type", "State"},
            {{"0x0001", "EchoActor", "Running"}, {"0x0002", "Worker", "Idle"}});
    auto out = f.finalize();
    assert(out.find("ID") != std::string::npos);
    assert(out.find("EchoActor") != std::string::npos);
    assert(out.find("Idle") != std::string::npos);
}

static void test_pretty_header() {
    PrettyFormatter f;
    f.header("Actor 0x0005");
    auto out = f.finalize();
    assert(out.find("Actor 0x0005") != std::string::npos);
}

static void test_pretty_error() {
    PrettyFormatter f;
    f.error("actor not found");
    auto out = f.finalize();
    assert(out.find("Error") != std::string::npos);
    assert(out.find("actor not found") != std::string::npos);
}

static void test_json_key_value() {
    JsonFormatter f;
    f.key_value({{"actor_id", "5"}, {"state", "Running"}});
    auto out = f.finalize();
    assert(out.find("\"actor_id\"") != std::string::npos);
    assert(out.find("\"Running\"") != std::string::npos);
    assert(out[0] == '{');
}

static void test_json_table() {
    JsonFormatter f;
    f.table({"id", "type"}, {{"1", "EchoActor"}});
    auto out = f.finalize();
    assert(out[0] == '[');
    assert(out.find("\"id\"") != std::string::npos);
    assert(out.find("\"EchoActor\"") != std::string::npos);
}

static void test_json_error() {
    JsonFormatter f;
    f.error("something went wrong");
    auto out = f.finalize();
    assert(out.find("\"error\"") != std::string::npos);
}

static void test_tabular_no_ansi() {
    TabularFormatter f;
    f.key_value({{"key", "value"}});
    auto out = f.finalize();
    assert(out.find('\033') == std::string::npos);
    assert(out.find("key: value") != std::string::npos);
}

static void test_tabular_table() {
    TabularFormatter f;
    f.table({"ID", "Type"}, {{"1", "Echo"}, {"2", "Worker"}});
    auto out = f.finalize();
    assert(out.find("ID") != std::string::npos);
    assert(out.find("Echo") != std::string::npos);
    assert(out.find('\033') == std::string::npos);
}

static void test_factory() {
    assert(OutputFormatter::create("pretty") != nullptr);
    assert(OutputFormatter::create("json") != nullptr);
    assert(OutputFormatter::create("tabular") != nullptr);
    assert(OutputFormatter::create("bogus") != nullptr);  // fallback to pretty
}

int main() {
    test_pretty_key_value();
    test_pretty_table();
    test_pretty_header();
    test_pretty_error();
    test_json_key_value();
    test_json_table();
    test_json_error();
    test_tabular_no_ansi();
    test_tabular_table();
    test_factory();
    std::printf("test_formatters: PASSED\n");
    return 0;
}
