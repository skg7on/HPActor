# HPActor Fuzz Tests

Coverage-guided fuzz testing for input-facing parsers and decoders using
[libFuzzer](https://llvm.org/docs/LibFuzzer.html).

## Quick Start

```bash
# Configure with fuzz support (requires Clang)
cmake -S . -B build -GNinja -DENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
ninja -C build fuzz_frame fuzz_protobuf_decode fuzz_http_parser fuzz_toml fuzz_cli_lexer fuzz_admin_api

# Quick smoke (10 seconds each)
for target in fuzz_frame fuzz_protobuf_decode fuzz_http_parser fuzz_toml fuzz_cli_lexer fuzz_admin_api; do
    echo "=== $target ==="
    ./build/tests/fuzz/${target} -max_total_time=10 2>&1 | tail -5
done

# Deep run (1 hour, 4 parallel workers)
./build/tests/fuzz/fuzz_frame tests/fuzz/fuzz_corpus/frame/ \
    -max_total_time=3600 -jobs=4 -workers=4

# Minimize a crashing input
./build/tests/fuzz/fuzz_frame crash-abc123.bin -minimize_crash=1

# Merge new coverage corpus
./build/tests/fuzz/fuzz_frame -merge=1 tests/fuzz/fuzz_corpus/frame/ \
    tests/fuzz/fuzz_corpus/frame_new/
```

## Fuzz Targets

| Target | Subsystem | Entry Point | Input Source |
|--------|-----------|-------------|--------------|
| `fuzz_frame` | Wire Protocol | `try_decode_wireframe()` | Network bytes |
| `fuzz_protobuf_decode` | Protobuf | `WireEnvelope::ParseFromString()` | Network payload |
| `fuzz_http_parser` | HTTP | `HttpParser::execute()` | HTTP bytes |
| `fuzz_toml` | Config | `toml::parse_file()` | Config files |
| `fuzz_cli_lexer` | CLI | `Lexer::tokenize()` | CLI commands |
| `fuzz_admin_api` | Admin | `AdminApiActor::handle()` | JSON bodies |

## Seed Corpus

Each target has a seed corpus in `tests/fuzz/fuzz_corpus/<target>/`.
Seeds cover valid inputs, edge cases (empty, max-size), and known
malformed inputs (truncated, wrong magic, invalid UTF-8).

To regenerate the corpus from coverage data:
```bash
./build/tests/fuzz/fuzz_frame -merge=1 old_corpus/ new_corpus/
```

## Triage Workflow

1. **Crash found:** libFuzzer saves the crashing input as `crash-<hash>`.
2. **Minimize:**
   ```bash
   ./build/tests/fuzz/fuzz_frame crash-<hash> -minimize_crash=1
   ```
3. **Reproduce under debugger:**
   ```bash
   lldb -- ./build/tests/fuzz/fuzz_frame minimized_crash
   ```
4. **Convert to regression test:** Write a `TEST(FuzzRegression, *)` case
   in the relevant subsystem test file (e.g., `tests/unit/net/test_frame.cpp`)
   that replays the minimized crash input and asserts the expected outcome
   (typically: no crash, no sanitizer violation).
5. **File a bug** with the minimized input attached.

## CI Smoke (Future)

Not yet wired into CI. Manual runbook for now:
```bash
# All targets, 60 seconds each, fail on first crash
for t in frame protobuf_decode http_parser toml cli_lexer admin_api; do
    ./build/tests/fuzz/fuzz_${t} -max_total_time=60 -runs=100000 || exit 1
done
```

## Design Doc

See `docs/superpowers/specs/2026-08-04-tst-004-fuzz-tests-design.md`.
