#include <cassert>
#include <cstdio>
#include <fstream>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_sink.hpp>
#include <string>
#include <sys/stat.h>

using namespace hpactor;

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// --- stderr sink ---
void test_stderr_write() {
    auto sink = log::make_stderr_sink();
    assert(sink != nullptr);
    assert(sink->write("hello_stderr_test").has_value());
    printf("  PASSED test_stderr_write\n");
}

void test_stderr_flush() {
    auto sink = log::make_stderr_sink();
    assert(sink->flush().has_value());
    printf("  PASSED test_stderr_flush\n");
}

// --- file sink ---
void test_file_write() {
    const char* path = "/tmp/hpactor_test_file_sink.log";
    std::remove(path);
    auto sink = log::make_file_sink(path);
    assert(sink != nullptr);
    assert(sink->write("hello_file").has_value());
    assert(sink->write("world_file").has_value());
    assert(sink->flush().has_value());
    std::string content = read_file(path);
    assert(content.find("hello_file") != std::string::npos);
    assert(content.find("world_file") != std::string::npos);
    std::remove(path);
    printf("  PASSED test_file_write\n");
}

void test_file_flush() {
    const char* path = "/tmp/hpactor_test_file_flush.log";
    std::remove(path);
    auto sink = log::make_file_sink(path);
    sink->write("flush_test");
    assert(sink->flush().has_value());
    assert(read_file(path).find("flush_test") != std::string::npos);
    std::remove(path);
    printf("  PASSED test_file_flush\n");
}

void test_file_factory() {
    assert(log::make_file_sink("/tmp/test.log") != nullptr);
    printf("  PASSED test_file_factory\n");
}

// --- rotating file sink ---
void test_rotating_write_below_threshold() {
    const char* path = "/tmp/hpactor_test_rotating.log";
    std::remove(path);
    log::RotatingFileConfig cfg;
    cfg.path = path;
    cfg.max_bytes = 1024;
    cfg.max_files = 3;
    auto sink = log::make_rotating_file_sink(cfg);
    assert(sink != nullptr);
    assert(sink->write("short_line").has_value());
    assert(sink->flush().has_value());
    assert(read_file(path).find("short_line") != std::string::npos);
    std::remove(path);
    printf("  PASSED test_rotating_write_below_threshold\n");
}

void test_rotating_triggers_rotation() {
    const char* path = "/tmp/hpactor_test_rotate_trigger.log";
    std::remove(path);
    std::remove((std::string(path) + ".1").c_str());
    std::remove((std::string(path) + ".2").c_str());
    log::RotatingFileConfig cfg;
    cfg.path = path;
    cfg.max_bytes = 10;
    cfg.max_files = 2;
    auto sink = log::make_rotating_file_sink(cfg);
    assert(sink != nullptr);
    // First write: 25 bytes with newline, exceeds max_bytes=10 -> triggers
    // rotation
    assert(sink->write("hello_world_long_enough").has_value());
    // After rotation the data is in .1 and a fresh file is open.
    // Second write must be <= 9 bytes (8 chars + newline) to avoid another
    // rotation.
    assert(sink->write("short").has_value());
    assert(sink->flush().has_value());
    std::string rotated_path = std::string(path) + ".1";
    assert(file_exists(rotated_path));
    std::string content = read_file(rotated_path);
    assert(content.find("hello_world_long_enough") != std::string::npos);
    std::string current_content = read_file(path);
    assert(current_content.find("short") != std::string::npos);
    std::remove(path);
    std::remove(rotated_path.c_str());
    printf("  PASSED test_rotating_triggers_rotation\n");
}

void test_rotating_flush() {
    const char* path = "/tmp/hpactor_test_rotating_flush.log";
    std::remove(path);
    log::RotatingFileConfig cfg;
    cfg.path = path;
    cfg.max_bytes = 1024;
    cfg.max_files = 2;
    auto sink = log::make_rotating_file_sink(cfg);
    sink->write("flush_me");
    assert(sink->flush().has_value());
    assert(read_file(path).find("flush_me") != std::string::npos);
    std::remove(path);
    printf("  PASSED test_rotating_flush\n");
}

void test_rotating_factory() {
    log::RotatingFileConfig cfg;
    cfg.path = "/tmp/test_rot.log";
    cfg.max_bytes = 1024;
    cfg.max_files = 2;
    assert(log::make_rotating_file_sink(cfg) != nullptr);
    printf("  PASSED test_rotating_factory\n");
}

int main() {
    printf("Log sinks tests:\n");
    test_stderr_write();
    test_stderr_flush();
    test_file_write();
    test_file_flush();
    test_file_factory();
    test_rotating_write_below_threshold();
    test_rotating_triggers_rotation();
    test_rotating_flush();
    test_rotating_factory();
    printf("All Log sinks tests PASSED\n");
    return 0;
}
