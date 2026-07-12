#!/usr/bin/env python3
# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Regenerate checked-in Python CLI protobuf modules.

Usage:
    generate_client_protos.py --protoc <path> --output <dir>
    generate_client_protos.py --protoc <path> --output <dir> --check

In check mode, exits 0 if generated files match the checked-in copies,
1 otherwise.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

PROTO_FILES = [
    "protos/hpactor/cli.proto",
    "protos/hpactor/cli_messages.proto",
]

GENERATED_NAMES = [
    "cli_pb2.py",
    "cli_messages_pb2.py",
]


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate CLI protobufs")
    parser.add_argument("--protoc", required=True, help="Path to protoc binary")
    parser.add_argument(
        "--output", required=True, help="Output directory for generated files"
    )
    parser.add_argument(
        "--check", action="store_true",
        help="Exit with non-zero status if generated files differ",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[3]
    protoc = Path(args.protoc)
    output = Path(args.output)

    if not protoc.exists():
        print(f"protoc not found at {protoc}", file=sys.stderr)
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        proto_dir = repo_root / "protos"

        cmd = [
            str(protoc),
            f"-I={proto_dir}",
            f"--python_out={tmp}",
        ] + [str(repo_root / p) for p in PROTO_FILES]

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"protoc failed:\n{result.stderr}", file=sys.stderr)
            sys.exit(1)

        gen_dir = tmp / "hpactor"
        if not gen_dir.is_dir():
            print("protoc did not produce hpactor/ directory", file=sys.stderr)
            sys.exit(1)

        if args.check:
            mismatches = []
            for name in GENERATED_NAMES:
                gen_file = gen_dir / name
                existing = output / name
                if not existing.exists():
                    mismatches.append(f"{name}: missing in {output}")
                    continue
                gen_bytes = gen_file.read_bytes()
                exist_bytes = existing.read_bytes()
                if gen_bytes != exist_bytes:
                    mismatches.append(f"{name}: differs from generated")
            if mismatches:
                for m in mismatches:
                    print(f"MISMATCH: {m}", file=sys.stderr)
                sys.exit(1)
            print(f"All {len(GENERATED_NAMES)} generated files match.")
            return

        # Copy generated files to output
        output.mkdir(parents=True, exist_ok=True)
        for name in GENERATED_NAMES:
            src = gen_dir / name
            dst = output / name
            dst.write_bytes(src.read_bytes())
            print(f"Wrote {dst}")

    print("Done.")


if __name__ == "__main__":
    main()
