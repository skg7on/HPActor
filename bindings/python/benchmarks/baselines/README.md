# Performance Baselines

This directory holds baseline performance results for the hpactor Python
binding.  Baselines are platform-specific and stored separately for each
supported target.

## Baseline files

- `macosx_12_0_arm64.json` — macOS ARM64 baseline
- `manylinux_2_28_x86_64.json` — Linux x86_64 baseline
- `manylinux_2_28_aarch64.json` — Linux ARM64 baseline

## Creating a baseline

```bash
python3 bindings/python/benchmarks/bench_actor_runtime.py \
  --wheel wheelhouse/hpactor-*-cp311-abi3-*.whl \
  --output bindings/python/benchmarks/baselines/<platform>.json \
  --warmup 10000 --iterations 100000 --payload-bytes 64
```

## Regression gate

Any metric regressing more than 20% from the baseline on the same
runner fails the CI performance gate.  Comparisons across different
hardware or platforms are never made.
