# HPActor Python Wheel Packaging

Hermetic native dependency tooling for building cp311-abi3 wheels.

## Quick start

```bash
# Build checksum-locked native dependencies
python3 bindings/python/packaging/build_native_deps.py \
  --prefix build/wheel-deps/prefix \
  --cache build/wheel-deps/cache \
  --build-dir build/wheel-deps/build \
  --deployment-target 12.0 \
  --jobs 8

# Configure CMake with the wheel prefix
cmake -S . -B build/wheel -GNinja \
  -DENABLE_PYTHON_BINDINGS=ON \
  -DHPACTOR_PYTHON_WHEEL_BUILD=ON \
  -DHPACTOR_WHEEL_DEPS_PREFIX=build/wheel-deps/prefix \
  -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF

# Build the extension
ninja -C build/wheel _hpactor
```

## Files

- `native-deps.lock.json` — immutable dependency manifest (versions, URLs, SHA-256)
- `fetch_source.py` — HTTPS download with digest verification and anti-rollback cache
- `build_native_deps.py` — platform-correct static builds (OpenSSL, Abseil, protobuf)

## Dependency versions

| Library | Version | License |
|---------|---------|---------|
| OpenSSL | 3.5.5 | Apache-2.0 |
| Abseil | 20260107.1 | Apache-2.0 |
| protobuf | 35.0 | BSD-3-Clause |

## Security

All sources are fetched over HTTPS with SHA-256 verification. Archives are
cached by digest prefix; existing verified archives are reused without
re-downloading. Redirects to different hosts are rejected.
