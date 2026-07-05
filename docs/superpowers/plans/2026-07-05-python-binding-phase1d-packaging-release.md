# Python Binding Phase 1D Packaging and Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Ship the Phase 1A through Phase 1C in-process binding as an installable hpactor distribution with CPython 3.11 ABI3 wheels for Linux x86_64/ARM64 and macOS x86_64/ARM64, clean-environment compatibility evidence, manual-aligned examples, performance gates, and a trusted release workflow.

**Architecture:** A root pyproject.toml uses scikit-build-core to drive the existing CMake project and installs hpactor._hpactor plus its private native runtime libraries into one wheel. cibuildwheel builds one cp311-abi3 wheel per supported platform/architecture, auditwheel or delocate repairs it, abi3audit and a dependency-closure checker validate it, and separate clean environments test the same artifact across supported CPython minors. Publishing is isolated behind a tag/version gate, four-platform acceptance, GitHub environments, and PyPI trusted publishing.

**Tech Stack:** CPython 3.11 Stable ABI, Py_LIMITED_API=0x030B0000, pyproject.toml, scikit-build-core, setuptools-scm, CMake/Ninja, cibuildwheel, auditwheel, delocate, abi3audit, build, twine, GitHub Actions, PyPI trusted publishing, Sphinx, protobuf 7.35.x Python runtime, protobuf 35.0 C++ runtime, Abseil 20260107.1, and OpenSSL 3.5.5 LTS.

## Global Constraints

- Phases 1A, 1B, and 1C must be implemented and passing before Phase 1D begins.
- Distribution name is hpactor; import package is hpactor; native module is hpactor._hpactor.
- Python requires CPython 3.11 or newer. PyPy, free-threaded CPython, Windows, musllinux, iOS, Android, and WebAssembly are outside Phase 1D.
- Every compiled wheel uses Python tag cp311 and ABI tag abi3, produced from Py_LIMITED_API=0x030B0000 and CMake Development.SABIModule.
- Supported wheel targets are manylinux_2_28_x86_64, manylinux_2_28_aarch64, macosx_12_0_x86_64, and macosx_12_0_arm64. Universal2 is not produced.
- Linux wheels build in official manylinux_2_28 containers on native x86_64 and ARM64 runners. No cross-architecture wheel is accepted without a native smoke test.
- macOS wheels build and test on native Intel and Apple Silicon runners with MACOSX_DEPLOYMENT_TARGET=12.0.
- Runtime Python dependency is protobuf>=7.35.0,<8. The minimum and newest available version below 8 are both tested.
- Wheel-native C++ dependency versions are protobuf 35.0, Abseil 20260107.1, and OpenSSL 3.5.5. They are source-built from a checksum-locked manifest with position-independent code and no shared dependency on a developer machine.
- C++ protobuf generated code and libprotobuf use the exact same protobuf release. No C++ protobuf ABI skew is permitted.
- Wheel repair must leave no unresolved HPActor, protobuf, Abseil, OpenSSL, or non-policy C++ runtime dependency.
- Importing hpactor or hpactor._hpactor performs no thread creation, actor-system construction, network initialization, or file-descriptor registration.
- Runtime threads begin only when ActorSystem starts and are joined before ActorSystem context exit returns.
- Wheel tests run from an empty temporary directory with the source checkout absent from sys.path.
- The wheel contains the package, type marker, license, metadata, and required private native libraries only. Tests, build trees, object files, static archives, CMake caches, and source-only internal headers are excluded.
- Public Python annotations remain complete and py.typed stays in the wheel.
- Examples use generated protobuf messages with explicit application TypeTags; no example introduces a JSON or pickle actor-message path.
- Performance comparisons run candidate and reference on the same runner. Results from different hardware are never compared.
- Once the first stable baseline exists, a regression greater than 20 percent in empty-handler throughput, dispatch wait, handler latency, or end-to-end p50/p95/p99 fails the gate.
- PyPI publishing uses trusted publishing with id-token: write, a protected environment, and no long-lived API token.
- A tag version, project metadata version, CMake project version, hpactor.__version__, wheel metadata version, and GitHub release version must match exactly.
- Release artifacts are immutable. A bad release is yanked and replaced with a new version; a published filename is never overwritten.
- No manual or README claim of an official packaged binding is made until every four-platform wheel and clean-environment gate passes.

## File Structure

### Package metadata and CMake install layout

- Create: pyproject.toml
- Create: bindings/python/README.md
- Create: bindings/python/hpactor/_version.py
- Modify: bindings/python/hpactor/__init__.py
- Modify: bindings/python/hpactor/py.typed
- Modify: CMakeLists.txt
- Modify: cmake/dependencies.cmake
- Modify: src/CMakeLists.txt
- Modify: bindings/python/native/CMakeLists.txt
- Create: cmake/python_wheel_install.cmake
- Modify: .gitignore

### Hermetic dependencies and binary audit

- Create: bindings/python/packaging/native-deps.lock.json
- Create: bindings/python/packaging/fetch_source.py
- Create: bindings/python/packaging/build_native_deps.py
- Create: bindings/python/packaging/verify_wheel.py
- Create: bindings/python/packaging/dependency-policy.json
- Create: bindings/python/packaging/README.md
- Create: bindings/python/tests/packaging/test_lock_manifest.py
- Create: bindings/python/tests/packaging/test_wheel_contents.py
- Create: bindings/python/tests/packaging/test_binary_policy.py

### Wheel smoke, compatibility, and CI

- Create: bindings/python/tests/wheel/test_import_quiescent.py
- Create: bindings/python/tests/wheel/test_metadata.py
- Create: bindings/python/tests/wheel/test_echo_smoke.py
- Create: bindings/python/tests/wheel/test_reliability_smoke.py
- Create: bindings/python/tests/wheel/test_typing.py
- Create: bindings/python/tests/wheel/run_clean_smoke.py
- Create: bindings/python/tests/wheel/requirements-smoke.txt
- Modify: bindings/python/tests/CMakeLists.txt
- Create: .github/workflows/python-wheels.yml
- Create: .github/workflows/python-publish.yml

### Manual, examples, and API reference

- Create: bindings/python/examples/echo.py
- Create: bindings/python/examples/request_response.py
- Create: bindings/python/examples/supervision.py
- Create: bindings/python/examples/operations.py
- Create: bindings/python/examples/README.md
- Create: docs/manual/python/index.rst
- Create: docs/manual/python/installation.rst
- Create: docs/manual/python/first-actor.rst
- Create: docs/manual/python/message-passing.rst
- Create: docs/manual/python/lifecycle.rst
- Create: docs/manual/python/operations.rst
- Create: docs/manual/python/deployment.rst
- Create: docs/manual/python/api.rst
- Modify: docs/manual/index.rst
- Modify: docs/manual/getting-started/installation.rst
- Modify: docs/manual/limitations.rst
- Modify: docs/manual/conf.py
- Modify: .github/workflows/deploy-docs-manual.yml

### Performance and release evidence

- Create: bindings/python/benchmarks/bench_actor_runtime.py
- Create: bindings/python/benchmarks/schema.json
- Create: bindings/python/benchmarks/baselines/README.md
- Create: tools/compare_python_binding_perf.py
- Create: bindings/python/tests/packaging/test_perf_compare.py
- Modify: CLAUDE_MEMORY.md
- Modify: docs/superpowers/specs/2026-07-03-python-language-binding-design.md

---

### Task 1: Define package metadata, versioning, and source-distribution contents

**Files:**
- Create: pyproject.toml
- Create: bindings/python/README.md
- Create: bindings/python/hpactor/_version.py
- Modify: bindings/python/hpactor/__init__.py
- Modify: .gitignore
- Create: bindings/python/tests/packaging/test_package_metadata.py

**Interfaces:**
- Consumes: the Phase 1B hpactor package, project version 0.1.0, Apache-2.0 LICENSE, and the repository-root CMake project.
- Produces: a PEP 517 package named hpactor, dynamic setuptools-scm version metadata, Python >=3.11 requirement, protobuf>=7.35.0,<8 dependency, cp311 ABI3 wheel intent, and deterministic sdist include/exclude rules.

- [ ] **Step 1: Write failing metadata and sdist-content tests**

~~~python
class PackageMetadataTest(unittest.TestCase):
    def test_pyproject_declares_supported_contract(self) -> None:
        data = tomllib.loads(Path("pyproject.toml").read_text())
        project = data["project"]
        self.assertEqual(project["name"], "hpactor")
        self.assertEqual(project["requires-python"], ">=3.11")
        self.assertEqual(project["dependencies"], ["protobuf>=7.35.0,<8"])
        self.assertEqual(data["tool"]["scikit-build"]["wheel"]["py-api"],
                         "cp311")

    def test_sdist_has_inputs_but_no_build_outputs(self) -> None:
        archive = build_sdist()
        names = tar_names(archive)
        self.assertIn("pyproject.toml", names)
        self.assertIn("CMakeLists.txt", names)
        self.assertIn("bindings/python/hpactor/__init__.py", names)
        self.assertIn("protos/hpactor/python_binding_internal.proto", names)
        self.assertFalse(any("/build/" in name for name in names))
        self.assertFalse(any(name.endswith((".o", ".a", ".so", ".dylib"))
                             for name in names))
~~~

Add a test that hpactor.__version__ uses importlib.metadata.version("hpactor") and falls back to "0+unknown" only when the distribution is not installed.

- [ ] **Step 2: Run packaging metadata tests to verify RED**

~~~bash
python3 -m unittest discover \
  -s bindings/python/tests/packaging \
  -p 'test_package_metadata.py' -v
~~~

Expected: pyproject.toml, package README, and version module do not exist.

- [ ] **Step 3: Create exact build-system and project metadata**

Create pyproject.toml with these values:

~~~toml
[build-system]
requires = [
  "scikit-build-core>=0.11,<0.13",
  "setuptools-scm>=8,<10",
]
build-backend = "scikit_build_core.build"

[project]
name = "hpactor"
dynamic = ["version"]
description = "Asyncio-first Python language binding for the HPActor actor runtime"
readme = "bindings/python/README.md"
requires-python = ">=3.11"
license = { file = "LICENSE" }
authors = [{ name = "HPActor Contributors" }]
dependencies = ["protobuf>=7.35.0,<8"]
classifiers = [
  "Development Status :: 3 - Alpha",
  "License :: OSI Approved :: Apache Software License",
  "Operating System :: MacOS :: MacOS X",
  "Operating System :: POSIX :: Linux",
  "Programming Language :: C++",
  "Programming Language :: Python :: 3",
  "Programming Language :: Python :: 3 :: Only",
  "Programming Language :: Python :: 3.11",
  "Programming Language :: Python :: 3.12",
  "Programming Language :: Python :: 3.13",
  "Programming Language :: Python :: 3.14",
  "Programming Language :: Python :: Implementation :: CPython",
  "Typing :: Typed",
]

[project.urls]
Homepage = "https://github.com/skg7on/HPActor"
Documentation = "https://skg7on.github.io/HPActor/"
Issues = "https://github.com/skg7on/HPActor/issues"
Source = "https://github.com/skg7on/HPActor"

[tool.scikit-build]
minimum-version = "build-system.requires"
cmake.version = ">=3.26"
build-dir = "build/python/{wheel_tag}"
wheel.py-api = "cp311"
wheel.packages = ["bindings/python/hpactor"]
wheel.license-files = ["LICENSE"]
sdist.include = [
  "CMakeLists.txt",
  "cmake/**",
  "include/**",
  "src/**",
  "protos/**",
  "third_party/llhttp/**",
  "third_party/linenoise/**",
  "bindings/python/**",
  "LICENSE",
]
sdist.exclude = [
  ".claude/**",
  ".git/**",
  ".github/**",
  ".worktrees/**",
  "apps/**",
  "build/**",
  "docs/**",
  "examples/**",
  "tests/**",
  "tools/**",
]

[tool.scikit-build.cmake.define]
ENABLE_PYTHON_BINDINGS = "ON"
ENABLE_TESTS = "OFF"
ENABLE_EXAMPLES = "OFF"
ENABLE_APPS = "OFF"
ENABLE_PROACTOR = "OFF"
HPACTOR_PYTHON_WHEEL_BUILD = "ON"

[tool.scikit-build.metadata.version]
provider = "scikit_build_core.metadata.setuptools_scm"

[tool.setuptools_scm]
fallback_version = "0.1.0"
version_scheme = "guess-next-dev"
local_scheme = "node-and-date"
~~~

The sdist excludes tests because wheel smoke sources are copied by CI before isolated install. It includes every native source needed for an end-user source build.

- [ ] **Step 4: Add runtime version lookup and package README**

Implement _version.py:

~~~python
from importlib.metadata import PackageNotFoundError, version

try:
    __version__ = version("hpactor")
except PackageNotFoundError:
    __version__ = "0+unknown"

__all__ = ["__version__"]
~~~

Import __version__ in hpactor.__init__.py and add it to __all__. The package README states the four supported wheel targets, CPython >=3.11, protobuf message requirement, asyncio execution model, build-tree versus installed usage, and the Phase 1D alpha stability level.

Add dist/, wheelhouse/, bindings/python/.venv/, and wheel audit output directories to .gitignore.

- [ ] **Step 5: Build the sdist and run metadata tests to verify GREEN**

~~~bash
python3 -m pip install \
  'build>=1.2,<2' 'scikit-build-core>=0.11,<0.13' \
  'setuptools-scm>=8,<10'
python3 -m build --sdist
python3 -m unittest discover \
  -s bindings/python/tests/packaging \
  -p 'test_package_metadata.py' -v
~~~

Expected: one hpactor source archive is produced, metadata tests pass, and no generated/build output is present.

- [ ] **Step 6: Commit package metadata**

~~~bash
git add pyproject.toml .gitignore bindings/python/README.md \
  bindings/python/hpactor/_version.py bindings/python/hpactor/__init__.py \
  bindings/python/tests/packaging/test_package_metadata.py
git commit -m "build: define hpactor Python distribution"
~~~

### Task 2: Install the limited-API module and private HPActor libraries into the wheel

**Files:**
- Modify: CMakeLists.txt
- Modify: cmake/dependencies.cmake
- Modify: src/CMakeLists.txt
- Modify: bindings/python/native/CMakeLists.txt
- Create: cmake/python_wheel_install.cmake
- Create: bindings/python/tests/packaging/test_cmake_wheel_layout.py

**Interfaces:**
- Consumes: Phase 1B _hpactor target, hpactor_python_native, hpactor_lib, hpactor_proto, SKBUILD_SABI_COMPONENT, and scikit-build-core install staging.
- Produces: hpactor/_hpactor with cp311 ABI3 naming, hpactor/.libs private runtime libraries, relative runtime search paths, hidden symbols, and an install component containing no developer artifacts.

- [ ] **Step 1: Write failing staged-install layout tests**

~~~python
class CMakeWheelLayoutTest(unittest.TestCase):
    def test_staged_install_contains_only_runtime_files(self) -> None:
        root = cmake_install_component("python-wheel")
        self.assertEqual(list((root / "hpactor").glob("_hpactor*.so")).__len__(),
                         1)
        self.assertTrue((root / "hpactor" / ".libs").is_dir())
        self.assertFalse(any(root.rglob("*.a")))
        self.assertFalse(any(root.rglob("CMakeCache.txt")))

    def test_extension_is_limited_api(self) -> None:
        extension = installed_extension()
        self.assertIn("abi3", expected_wheel_tag(extension))
        self.assertEqual(read_exported_python_init(extension),
                         "PyInit__hpactor")
~~~

Add platform tests for relative RPATH/install-name and hidden non-PyInit symbols.

- [ ] **Step 2: Configure the staged install to verify RED**

~~~bash
cmake -S . -B build/python-layout -GNinja \
  -DENABLE_PYTHON_BINDINGS=ON \
  -DHPACTOR_PYTHON_WHEEL_BUILD=ON \
  -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build/python-layout _hpactor
cmake --install build/python-layout \
  --component python-wheel \
  --prefix build/python-layout/stage
~~~

Expected: the component or correct hpactor wheel layout does not exist.

- [ ] **Step 3: Enforce the Stable ABI CMake path**

Add HPACTOR_PYTHON_WHEEL_BUILD default OFF. When enabled:

~~~cmake
if(CMAKE_VERSION VERSION_LESS 3.26)
  message(FATAL_ERROR
    "Python ABI3 wheel builds require CMake 3.26 or newer")
endif()
find_package(Python 3.11 REQUIRED
  COMPONENTS Interpreter Development.SABIModule)
~~~

In the binding CMake file use Python_add_library with USE_SABI 3.11 and define Py_LIMITED_API=0x030B0000. Set OUTPUT_NAME _hpactor, CXX_VISIBILITY_PRESET hidden, VISIBILITY_INLINES_HIDDEN ON, PREFIX "", and prohibit exceptions/RTTI. Fail configuration if SKBUILD_SABI_COMPONENT is empty during a wheel build.

- [ ] **Step 4: Create the private shared-library install layout**

Include python_wheel_install.cmake only for HPACTOR_PYTHON_WHEEL_BUILD. Install:

~~~cmake
install(TARGETS _hpactor
  LIBRARY DESTINATION hpactor
  COMPONENT python-wheel)
install(TARGETS hpactor_lib hpactor_proto
  LIBRARY DESTINATION hpactor/.libs
  COMPONENT python-wheel)
~~~

For Linux set _hpactor BUILD_RPATH and INSTALL_RPATH to $ORIGIN/.libs, and private HPActor libraries to $ORIGIN. For macOS use @loader_path/.libs and @loader_path. Set MACOSX_RPATH ON and remove absolute build/install paths.

The wheel build makes hpactor_lib and hpactor_proto private implementation details: do not install headers, CMake package exports, pkg-config files, executables, static libraries, or CLI tools.

- [ ] **Step 5: Make wheel dependency linkage private**

During wheel builds, link OpenSSL, protobuf, Abseil, hpactor_proto, and hpactor_lib privately below _hpactor. Preserve normal public linkage for the C++ SDK build. Set POSITION_INDEPENDENT_CODE ON for all static dependencies consumed by wheel shared objects.

Add one architecture check that fails if an installed .so/.dylib contains an absolute RPATH under the checkout, build directory, Homebrew prefix, or dependency staging prefix.

- [ ] **Step 6: Run staged-install and symbol tests**

~~~bash
ninja -C build/python-layout _hpactor
cmake --install build/python-layout \
  --component python-wheel \
  --prefix build/python-layout/stage
python3 -m unittest \
  bindings.python.tests.packaging.test_cmake_wheel_layout -v
~~~

Expected: one extension, private HPActor libraries, relative runtime paths, one PyInit export, and no developer/build artifacts.

- [ ] **Step 7: Commit wheel install layout**

~~~bash
git add CMakeLists.txt cmake/dependencies.cmake \
  cmake/python_wheel_install.cmake src/CMakeLists.txt \
  bindings/python/native/CMakeLists.txt \
  bindings/python/tests/packaging/test_cmake_wheel_layout.py
git commit -m "build: install Python ABI3 wheel layout"
~~~

### Task 3: Build checksum-locked native dependencies for wheel targets

**Files:**
- Create: bindings/python/packaging/native-deps.lock.json
- Create: bindings/python/packaging/fetch_source.py
- Create: bindings/python/packaging/build_native_deps.py
- Create: bindings/python/packaging/README.md
- Create: bindings/python/tests/packaging/test_lock_manifest.py
- Modify: cmake/dependencies.cmake

**Interfaces:**
- Consumes: OpenSSL 3.5.5, Abseil 20260107.1, protobuf 35.0 source releases, CMake/Ninja, platform compilers, and HPACTOR_WHEEL_DEPS_PREFIX.
- Produces: a verified per-platform prefix containing PIC static OpenSSL/Abseil/protobuf libraries and matching protoc, with no network access after source fetch.

- [ ] **Step 1: Write failing lock and offline-rebuild tests**

~~~python
class NativeDependencyLockTest(unittest.TestCase):
    def test_every_source_is_https_and_sha256_locked(self) -> None:
        lock = json.loads(LOCK.read_text())
        self.assertEqual(set(lock), {"openssl", "abseil", "protobuf"})
        for entry in lock.values():
            self.assertTrue(entry["url"].startswith("https://"))
            self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")

    def test_versions_are_exact(self) -> None:
        lock = json.loads(LOCK.read_text())
        self.assertEqual(lock["openssl"]["version"], "3.5.5")
        self.assertEqual(lock["abseil"]["version"], "20260107.1")
        self.assertEqual(lock["protobuf"]["version"], "35.0")
~~~

Add a fixture with a corrupted archive and assert fetch_source exits nonzero before extraction. Add an offline second-build test using only the verified download cache.

- [ ] **Step 2: Run lock tests to verify RED**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_lock_manifest -v
~~~

Expected: lock, verified fetcher, and native dependency builder are missing.

- [ ] **Step 3: Add the immutable dependency manifest and verified fetcher**

The manifest contains exactly version, upstream release URL, SHA256, license
identifier, and source directory name:

~~~json
{
  "openssl": {
    "version": "3.5.5",
    "url": "https://github.com/openssl/openssl/releases/download/openssl-3.5.5/openssl-3.5.5.tar.gz",
    "sha256": "1bfb42db18c62f7bd8fd8dbf1a1c10999fc01fdc70f1bfe1a94602ae08eef32b",
    "license": "Apache-2.0",
    "source_dir": "openssl-3.5.5"
  },
  "abseil": {
    "version": "20260107.1",
    "url": "https://github.com/abseil/abseil-cpp/archive/refs/tags/20260107.1.tar.gz",
    "sha256": "3dba011bff767d027db68562d032cfee098e7ae9bf3766e57a60ca22a7124d0c",
    "license": "Apache-2.0",
    "source_dir": "abseil-cpp-20260107.1"
  },
  "protobuf": {
    "version": "35.0",
    "url": "https://github.com/protocolbuffers/protobuf/releases/download/v35.0/protobuf-35.0.tar.gz",
    "sha256": "8f907baca4b34a3b4854103ba5811e418fb6e2ff11fe0d8df9e8280b11d79926",
    "license": "BSD-3-Clause",
    "source_dir": "protobuf-35.0"
  }
}
~~~

fetch_source.py downloads to a content-addressed cache, streams SHA256 verification, rejects redirects away from HTTPS, extracts into a fresh directory with path traversal protection, and writes a verified stamp containing the digest. Existing unverified files are deleted.

- [ ] **Step 4: Build platform-correct static dependencies**

build_native_deps.py accepts --prefix, --cache, --build-dir, --deployment-target, and --jobs. It performs:

1. OpenSSL Configure with no-shared, no-tests, and the exact target linux-x86_64, linux-aarch64, darwin64-x86_64-cc, or darwin64-arm64-cc.
2. Abseil CMake with ABSL_BUILD_TESTING=OFF, ABSL_PROPAGATE_CXX_STD=ON, BUILD_SHARED_LIBS=OFF, and CMAKE_POSITION_INDEPENDENT_CODE=ON.
3. protobuf CMake with protobuf_BUILD_TESTS=OFF, protobuf_BUILD_SHARED_LIBS=OFF, protobuf_ABSL_PROVIDER=package, and the same C++20/compiler/deployment target.

For macOS pass CMAKE_OSX_DEPLOYMENT_TARGET=12.0 and the matching CMAKE_OSX_ARCHITECTURES value to every dependency. For Linux build inside manylinux_2_28. Write dependency-build.json with versions, digests, compilers, target, and flags.

- [ ] **Step 5: Make CMake consume only the wheel prefix**

When HPACTOR_PYTHON_WHEEL_BUILD is ON, require HPACTOR_WHEEL_DEPS_PREFIX and prepend it to CMAKE_PREFIX_PATH. Require Protobuf_VERSION 35.0 and verify protoc --version reports 35.0. Resolve OpenSSL and Abseil only from the prefix with CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH disabled for those calls.

Normal non-wheel CMake behavior remains unchanged.

- [ ] **Step 6: Run verified dependency build tests**

~~~bash
python3 bindings/python/packaging/build_native_deps.py \
  --prefix build/wheel-deps/prefix \
  --cache build/wheel-deps/cache \
  --build-dir build/wheel-deps/build \
  --deployment-target 12.0 \
  --jobs 8
python3 -m unittest \
  bindings.python.tests.packaging.test_lock_manifest -v
~~~

Expected: exact dependency versions build, a second offline run succeeds from verified cache, corruption fails before extraction, and the prefix contains only the intended target architecture.

- [ ] **Step 7: Commit hermetic dependency tooling**

~~~bash
git add bindings/python/packaging \
  bindings/python/tests/packaging/test_lock_manifest.py \
  cmake/dependencies.cmake
git commit -m "build: lock Python wheel native dependencies"
~~~

### Task 4: Repair and audit wheels for ABI and dependency closure

**Files:**
- Create: bindings/python/packaging/verify_wheel.py
- Create: bindings/python/packaging/dependency-policy.json
- Create: bindings/python/tests/packaging/test_wheel_contents.py
- Create: bindings/python/tests/packaging/test_binary_policy.py
- Modify: pyproject.toml

**Interfaces:**
- Consumes: raw scikit-build-core wheel, auditwheel, delocate, abi3audit, wheel ZIP metadata, platform binary inspection tools, and private HPActor libraries.
- Produces: repaired policy-tagged wheel, ABI3 proof, exact content manifest, dependency report, and a hard failure for unresolved or forbidden libraries.

- [ ] **Step 1: Write failing wheel-content and policy tests**

~~~python
class WheelContentsTest(unittest.TestCase):
    def test_wheel_has_one_native_module_and_type_marker(self) -> None:
        wheel = load_repaired_wheel()
        names = wheel.namelist()
        self.assertEqual(len(match(names, "hpactor/_hpactor*")), 1)
        self.assertIn("hpactor/py.typed", names)
        self.assertEqual(len(match(names, "hpactor-*.dist-info/LICENSE*")), 1)
        self.assertFalse(any(name.endswith((".a", ".o", ".pyc"))
                             for name in names))

    def test_metadata_and_filename_are_abi3(self) -> None:
        wheel = load_repaired_wheel()
        self.assertRegex(wheel.filename, r"-cp311-abi3-")
        self.assertEqual(wheel.metadata["Requires-Python"], ">=3.11")
        self.assertIn("protobuf<8,>=7.35.0",
                      wheel.metadata.get_all("Requires-Dist"))
~~~

Binary policy tests inject a fake absolute RPATH and a fake unresolved libprotobuf reference and assert verification fails.

- [ ] **Step 2: Build a raw wheel and verify RED**

~~~bash
python3 -m build --wheel
python3 -m unittest discover \
  -s bindings/python/tests/packaging \
  -p 'test_*wheel*.py' -v
~~~

Expected: repair/audit reports and strict dependency policy are absent or fail.

- [ ] **Step 3: Define exact dependency policy**

dependency-policy.json contains:

- required private libraries: hpactor_lib and hpactor_proto;
- forbidden unresolved prefixes: hpactor, protobuf, absl, ssl, crypto;
- allowed Linux policy libraries copied from the manylinux_2_28 policy at tool install time;
- allowed macOS system roots: /usr/lib and /System/Library/Frameworks;
- forbidden absolute roots: checkout, build, dependency prefix, /opt/homebrew, /usr/local, and runner tool cache;
- required architectures for each target.

Do not hard-code an auditwheel policy list that can silently drift. verify_wheel.py records the auditwheel/delocate version and the resolved policy libraries in the report.

- [ ] **Step 4: Implement platform repair and strict verification**

Linux:

~~~bash
if test "$(uname -m)" = "x86_64"; then
  auditwheel repair --plat manylinux_2_28_x86_64 \
    --wheel-dir wheelhouse dist/hpactor-*.whl
else
  test "$(uname -m)" = "aarch64"
  auditwheel repair --plat manylinux_2_28_aarch64 \
    --wheel-dir wheelhouse dist/hpactor-*.whl
fi
abi3audit --strict wheelhouse/hpactor-*.whl
~~~

macOS:

~~~bash
if test "$(uname -m)" = "x86_64"; then
  delocate-wheel --require-archs x86_64 \
    --wheel-dir wheelhouse --verbose dist/hpactor-*.whl
else
  test "$(uname -m)" = "arm64"
  delocate-wheel --require-archs arm64 \
    --wheel-dir wheelhouse --verbose dist/hpactor-*.whl
fi
abi3audit --strict wheelhouse/hpactor-*.whl
~~~

verify_wheel.py opens the repaired wheel, validates RECORD hashes, metadata, tags, one native module, py.typed, license, package files, private libraries, architecture, deployment target, RPATH/install names, and dependency closure. It emits wheel-audit.json and exits nonzero on any violation.

- [ ] **Step 5: Prove import is side-effect free**

Run a subprocess with python -I that captures threading.enumerate(), open file-descriptor count where supported, and native thread count before and after importing hpactor and hpactor._hpactor. The only allowed delta is imported module memory; thread count and descriptor count must not increase.

- [ ] **Step 6: Run repaired-wheel audits**

~~~bash
python3 bindings/python/packaging/verify_wheel.py \
  --wheel wheelhouse/hpactor-*-cp311-abi3-*.whl \
  --policy bindings/python/packaging/dependency-policy.json \
  --report build/wheel-audit.json
python3 -m unittest discover \
  -s bindings/python/tests/packaging \
  -p 'test_*policy.py' -v
~~~

Expected: ABI3, content, architecture, deployment target, RPATH, metadata, and dependency closure pass; every injected violation fails.

- [ ] **Step 7: Commit repair and audit tooling**

~~~bash
git add pyproject.toml bindings/python/packaging \
  bindings/python/tests/packaging/test_wheel_contents.py \
  bindings/python/tests/packaging/test_binary_policy.py
git commit -m "test: audit Python wheel binary policy"
~~~

### Task 5: Add clean-environment wheel smoke and cross-minor compatibility tests

**Files:**
- Create: bindings/python/tests/wheel/test_import_quiescent.py
- Create: bindings/python/tests/wheel/test_metadata.py
- Create: bindings/python/tests/wheel/test_echo_smoke.py
- Create: bindings/python/tests/wheel/test_reliability_smoke.py
- Create: bindings/python/tests/wheel/test_typing.py
- Create: bindings/python/tests/wheel/run_clean_smoke.py
- Create: bindings/python/tests/wheel/requirements-smoke.txt
- Modify: bindings/python/tests/CMakeLists.txt

**Interfaces:**
- Consumes: one repaired wheel, CPython 3.11 through the newest supported stable minor, protobuf 7.35.x, public hpactor API, and Phase 1C runtime behavior.
- Produces: isolated import, echo, ask/reply, delivery, supervision, operations, typing, and deterministic shutdown evidence with no source-tree leakage.

- [ ] **Step 1: Write wheel metadata and import tests**

test_metadata.py asserts distribution/import/version identity, Requires-Python, protobuf bounds, py.typed, and that importlib.metadata.files contains no tests/build artifacts.

test_import_quiescent.py runs in a subprocess and asserts:

~~~python
before_threads = {thread.ident for thread in threading.enumerate()}
before_fds = fd_count()
import hpactor
import hpactor._hpactor
after_threads = {thread.ident for thread in threading.enumerate()}
after_fds = fd_count()
assert after_threads == before_threads
assert after_fds == before_fds
~~~

- [ ] **Step 2: Add a real installed-wheel echo and ask workflow**

~~~python
@actor("wheel-echo")
class Echo(Actor):
    def behavior(self) -> Behavior:
        return Behavior().on_request(
            StringValue, StringValue, self.echo)

    async def echo(self, message: StringValue,
                   ctx: ActorContext) -> StringValue:
        return StringValue(value=message.value)


class WheelEchoTest(unittest.IsolatedAsyncioTestCase):
    async def test_installed_wheel_ask_and_shutdown(self) -> None:
        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)
        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(Echo, name="echo")
            reply = await system.ask(
                ref, StringValue(value="wheel"),
                response_type=StringValue, timeout=5.0)
            self.assertEqual(reply.value, "wheel")
        self.assertEqual(active_hpactor_threads(), [])
~~~

The test imports google.protobuf.wrappers_pb2 from the installed runtime and never imports from the checkout.

- [ ] **Step 3: Add reliability and operations smoke**

Exercise one delivery receipt, one scheduled/cancelled message, one handler failure followed by restart, /python status snapshot access, and context-manager shutdown. Assert no late callback, actor thread, notifier descriptor, or outstanding future after exit.

Keep the wheel smoke below 30 seconds and use asyncio.wait_for only as a deadlock guard.

- [ ] **Step 4: Add installed typing verification**

Install mypy>=1.11,<2 in the test environment and type-check a small consumer importing every public value, defining an Actor, registering protobuf messages, spawning, sending, asking, and reading DeliveryResult. Use strict mode and no source-tree MYPYPATH.

- [ ] **Step 5: Implement isolated environment orchestration**

run_clean_smoke.py accepts --python, --wheel, and --protobuf. It creates a fresh venv outside the checkout, upgrades pip, installs exactly the wheel plus requested protobuf version, copies only wheel tests into a separate temporary directory, changes cwd there, clears PYTHONPATH, and runs python -I -m unittest discover.

Run:

- CPython 3.11 with protobuf 7.35.0;
- CPython 3.11 with newest protobuf below 8;
- every newer supported CPython minor with newest protobuf below 8;
- one CPython 3.10 negative install proving Requires-Python rejection.

- [ ] **Step 6: Run local clean smoke**

~~~bash
python3 bindings/python/tests/wheel/run_clean_smoke.py \
  --python python3.11 \
  --wheel wheelhouse/hpactor-*-cp311-abi3-*.whl \
  --protobuf 7.35.0
~~~

Expected: metadata, quiescent import, echo, ask/reply, reliability, operations, typing, and shutdown pass outside the checkout.

- [ ] **Step 7: Commit wheel smoke coverage**

~~~bash
git add bindings/python/tests/wheel bindings/python/tests/CMakeLists.txt
git commit -m "test: verify installed hpactor wheel"
~~~

### Task 6: Build and test the four-platform wheel matrix in CI

**Files:**
- Create: .github/workflows/python-wheels.yml
- Modify: pyproject.toml
- Create: bindings/python/tests/packaging/test_ci_matrix.py

**Interfaces:**
- Consumes: cibuildwheel 4.1.0, native GitHub-hosted x86_64/ARM64 runners, hermetic dependency builder, repair/audit scripts, and clean smoke suite.
- Produces: four cp311-abi3 wheels, per-wheel audit/checksum artifacts, cross-minor compatibility results, and a reusable workflow output for publishing.

- [ ] **Step 1: Write a failing static CI matrix test**

~~~python
class WheelCiMatrixTest(unittest.TestCase):
    def test_exact_supported_targets(self) -> None:
        workflow = load_yaml(".github/workflows/python-wheels.yml")
        rows = workflow["jobs"]["build-wheels"]["strategy"]["matrix"]["include"]
        targets = {(row["os"], row["arch"], row["platform"]) for row in rows}
        self.assertEqual(targets, {
            ("ubuntu-24.04", "x86_64", "manylinux_2_28_x86_64"),
            ("ubuntu-24.04-arm", "aarch64", "manylinux_2_28_aarch64"),
            ("macos-15-intel", "x86_64", "macosx_12_0_x86_64"),
            ("macos-14", "arm64", "macosx_12_0_arm64"),
        })
~~~

Also assert build selector cp311-*, wheel.py-api cp311, native architectures only, artifact retention, and audit steps.

- [ ] **Step 2: Run the matrix test to verify RED**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_ci_matrix -v
~~~

Expected: python-wheels.yml does not exist.

- [ ] **Step 3: Add the reusable build workflow**

Trigger on pull requests affecting binding/package/CMake/protobuf/manual files, pushes to main, workflow_call, and workflow_dispatch. Use concurrency cancellation for pull requests and permissions contents: read.

The matrix has exactly the four rows above. Each job:

1. checks out full history for setuptools-scm;
2. sets up Python 3.11 for cibuildwheel;
3. installs cibuildwheel==4.1.0 and audit tools;
4. builds checksum-locked native dependencies for its target;
5. invokes pypa/cibuildwheel@v4.1.0 with build cp311-* and arch native;
6. repairs and runs abi3audit plus verify_wheel.py;
7. runs the clean smoke on native hardware;
8. creates SHA256SUMS and uploads wheel, audit JSON, dependency-build JSON, and checksums.

Linux selects manylinux_2_28 for x86_64/aarch64. macOS sets MACOSX_DEPLOYMENT_TARGET=12.0 and never produces universal2.

- [ ] **Step 4: Add cross-minor compatibility jobs**

After all wheel builds, one job per platform downloads its wheel and runs clean smoke against every CPython minor available from 3.11 through the newest stable supported by actions/setup-python. The matrix explicitly lists 3.11, 3.12, 3.13, and 3.14; prereleases are not enabled.

Only CPython 3.11 repeats the protobuf minimum/latest submatrix. Newer minors use latest protobuf below 8.

- [ ] **Step 5: Aggregate acceptance without publishing**

Add wheel-acceptance depending on every build and compatibility job. It downloads all artifacts, verifies exactly four distinct platform wheels, verifies their SHA256 files and identical version, and uploads one accepted-wheel-set artifact.

Pull requests and main pushes never publish to an index.

- [ ] **Step 6: Run workflow syntax and matrix tests**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_ci_matrix -v
python3 -m yamllint .github/workflows/python-wheels.yml
~~~

Expected: exact target set, native tests, ABI3 selector, artifact aggregation, and no publish permissions pass.

- [ ] **Step 7: Commit wheel CI**

~~~bash
git add .github/workflows/python-wheels.yml pyproject.toml \
  bindings/python/tests/packaging/test_ci_matrix.py
git commit -m "ci: build Python ABI3 wheel matrix"
~~~

### Task 7: Add manual-aligned Python examples and API documentation

**Files:**
- Create: bindings/python/examples/echo.py
- Create: bindings/python/examples/request_response.py
- Create: bindings/python/examples/supervision.py
- Create: bindings/python/examples/operations.py
- Create: bindings/python/examples/README.md
- Create: docs/manual/python/index.rst
- Create: docs/manual/python/installation.rst
- Create: docs/manual/python/first-actor.rst
- Create: docs/manual/python/message-passing.rst
- Create: docs/manual/python/lifecycle.rst
- Create: docs/manual/python/operations.rst
- Create: docs/manual/python/deployment.rst
- Create: docs/manual/python/api.rst
- Modify: docs/manual/index.rst
- Modify: docs/manual/getting-started/installation.rst
- Modify: docs/manual/conf.py
- Modify: .github/workflows/deploy-docs-manual.yml
- Create: bindings/python/tests/packaging/test_examples.py

**Interfaces:**
- Consumes: Phase 1B public API, Phase 1C reliability/operations API, installed hpactor wheel, generated protobuf classes, and existing manual concepts.
- Produces: install guide, first actor, message passing, lifecycle, operations, deployment, API reference, and executable examples mirroring the C++ manual without importing build-tree internals.

- [ ] **Step 1: Write failing documentation and example coverage tests**

~~~python
class PythonDocsTest(unittest.TestCase):
    def test_manual_has_complete_python_section(self) -> None:
        index = Path("docs/manual/index.rst").read_text()
        for page in (
            "python/installation",
            "python/first-actor",
            "python/message-passing",
            "python/lifecycle",
            "python/operations",
            "python/deployment",
            "python/api",
        ):
            self.assertIn(page, index)

    def test_examples_use_explicit_type_tags(self) -> None:
        for path in Path("bindings/python/examples").glob("*.py"):
            text = path.read_text()
            self.assertNotIn("pickle", text)
            self.assertNotIn("json.dumps(message", text)
        self.assertIn("type_tag=0x1000",
                      Path("bindings/python/examples/echo.py").read_text())
~~~

Add tests that execute every example against the installed wheel with a 30-second timeout and assert clean shutdown.

- [ ] **Step 2: Run documentation tests to verify RED**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_examples -v
sphinx-build -W -b html docs/manual docs/manual/_build/html
~~~

Expected: Python manual pages and examples do not exist.

- [ ] **Step 3: Add installation and first-actor pages**

installation.rst documents:

- pip install hpactor;
- CPython >=3.11 and four supported wheel targets;
- how to verify hpactor.__version__;
- protobuf>=7.35.0,<8;
- no threads at import;
- source-build prerequisites and HPACTOR_WHEEL_DEPS_PREFIX;
- unsupported platform behavior;
- clean uninstall.

first-actor.rst mirrors the C++ "Your First Actor" progression using Actor, Behavior, ActorContext, MessageRegistry, generated protobuf StringValue, explicit tag 0x1000, async with ActorSystem, spawn, ask, and deterministic shutdown.

- [ ] **Step 4: Add messaging, lifecycle, and operations pages**

message-passing.rst covers generated protobuf registration, deterministic serialization, fixed TypeTags, fire-and-forget, ask/reply, ActorError, DeliveryOptions/Receipt/Result, scheduling, timeout, cancellation, and local/remote compatibility.

lifecycle.rst covers on_start/on_stop, supervision policy, restart generation, links, monitors, ExitEvent/DownEvent, quarantine, context lifetime, and immutable restart arguments.

operations.rst documents all eleven metrics, structured failure fields, python.actor.handle spans, /python CLI commands, health/readiness behavior, DLQ evidence, and bounded inspection.

- [ ] **Step 5: Add deployment and API reference pages**

deployment.rst covers queue/config sizing, asyncio loop lag, process scaling rather than subinterpreter sharding, wheel target selection, graceful shutdown, container health probes, deployment-target/glibc policy, and the unsupported-platform matrix.

api.rst lists every public symbol in hpactor.__all__ with exact signature, return type, lifecycle validity, exception outcomes, and thread/loop ownership. It is written from public annotations and does not import the native module during Sphinx build.

Add a Python Binding toctree section to manual index and a cross-link from the general installation page.

- [ ] **Step 6: Add executable manual examples**

echo.py is the five-minute first actor. request_response.py demonstrates typed ask/reply and ActorError. supervision.py demonstrates restart and generation fencing. operations.py prints a bounded runtime snapshot and shows clean shutdown.

Each example uses asyncio.run(main()), explicit MessageRegistry tags, no sleeps as proof, and exits with no runtime thread.

- [ ] **Step 7: Harden documentation CI**

Install the docs requirements plus packaging metadata dependencies, run sphinx-build -W, run the example tests against an accepted wheel artifact, and fail on broken references or warnings. Do not import a build-tree _hpactor.

- [ ] **Step 8: Run docs and example verification**

~~~bash
sphinx-build -W -b html docs/manual docs/manual/_build/html
python3 -m unittest \
  bindings.python.tests.packaging.test_examples -v
~~~

Expected: warning-free manual build and every installed-wheel example completes successfully.

- [ ] **Step 9: Commit manual and examples**

~~~bash
git add bindings/python/examples docs/manual \
  .github/workflows/deploy-docs-manual.yml \
  bindings/python/tests/packaging/test_examples.py
git commit -m "docs: add Python binding manual"
~~~

### Task 8: Add same-runner Python binding performance baselines and regression gates

**Files:**
- Create: bindings/python/benchmarks/bench_actor_runtime.py
- Create: bindings/python/benchmarks/schema.json
- Create: bindings/python/benchmarks/baselines/README.md
- Create: tools/compare_python_binding_perf.py
- Create: bindings/python/tests/packaging/test_perf_compare.py
- Modify: .github/workflows/python-wheels.yml

**Interfaces:**
- Consumes: installed candidate/reference wheels, monotonic high-resolution clocks, fixed protobuf payloads, same-runner execution, and accepted wheel artifacts.
- Produces: versioned JSON measurements for empty-handler throughput, dispatch queue wait, handler latency, end-to-end p50/p95/p99, and a 20-percent regression decision.

- [ ] **Step 1: Write failing benchmark-schema and comparison tests**

~~~python
class PerfCompareTest(unittest.TestCase):
    def test_rejects_more_than_twenty_percent_regression(self) -> None:
        reference = result(throughput=1000, p95_ns=100)
        candidate = result(throughput=790, p95_ns=121)
        report = compare(reference, candidate, threshold=0.20)
        self.assertFalse(report.passed)
        self.assertIn("throughput", report.regressions)
        self.assertIn("p95_ns", report.regressions)

    def test_rejects_different_runner_fingerprint(self) -> None:
        with self.assertRaises(FingerprintMismatch):
            compare(result(cpu="a"), result(cpu="b"), threshold=0.20)
~~~

- [ ] **Step 2: Run comparator tests to verify RED**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_perf_compare -v
~~~

Expected: benchmark schema and comparator do not exist.

- [ ] **Step 3: Implement deterministic benchmark scenarios**

bench_actor_runtime.py accepts --wheel, --output, --warmup, --iterations, and --payload-bytes. It creates a fresh venv, installs the wheel, pins CPU affinity when supported, disables debug instrumentation, warms up, and measures:

- empty fire-and-forget handler throughput;
- dispatch enqueue-to-handler-start wait;
- handler start-to-finish duration;
- ask/reply end-to-end latency;
- p50, p95, and p99 using nearest-rank percentiles.

Use a fixed 64-byte protobuf payload, one runtime thread, fixed queue capacities, 10000 warmup operations, and 100000 measured operations. Store raw sample count, medians from five runs, wheel SHA256, compiler, OS, architecture, CPU model, logical CPU count, and runner image.

- [ ] **Step 4: Implement strict same-runner comparison**

tools/compare_python_binding_perf.py validates schema, runner fingerprint, scenario parameters, and sample counts. Throughput regression is (reference-candidate)/reference; latency regression is (candidate-reference)/reference. Any metric greater than 0.20 exits nonzero and writes a Markdown/JSON report.

Never compare x86_64 with ARM64, Linux with macOS, or different runner fingerprints.

- [ ] **Step 5: Establish the first stable baseline**

On the controlled release runner, execute five candidate runs after all correctness gates pass. Review variance; require coefficient of variation below 5 percent for throughput and p95. Store the median JSON under a filename containing platform, architecture, and runner fingerprint only after the first stable release candidate is accepted.

Before that file exists, CI records and uploads performance artifacts but does not claim a regression comparison. After it exists, absence of a comparison is a failure.

- [ ] **Step 6: Add CI performance job**

Run performance on one designated native Linux x86_64 runner after wheel acceptance. For later releases, build/install the latest main/reference wheel and candidate wheel on the same job, alternate run order, compare medians, and upload reports. Keep performance separate from wheel correctness so noise cannot hide a functional failure.

- [ ] **Step 7: Run comparator and benchmark smoke**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_perf_compare -v
python3 bindings/python/benchmarks/bench_actor_runtime.py \
  --wheel wheelhouse/hpactor-*-cp311-abi3-*.whl \
  --output build/python-perf.json \
  --warmup 1000 --iterations 10000 --payload-bytes 64
~~~

Expected: schema validation passes, the reduced local smoke completes, and synthetic >20-percent regressions fail.

- [ ] **Step 8: Commit performance gates**

~~~bash
git add bindings/python/benchmarks tools/compare_python_binding_perf.py \
  bindings/python/tests/packaging/test_perf_compare.py \
  .github/workflows/python-wheels.yml
git commit -m "perf: gate Python binding regressions"
~~~

### Task 9: Publish accepted artifacts through protected trusted publishing

**Files:**
- Create: .github/workflows/python-publish.yml
- Modify: pyproject.toml
- Create: bindings/python/tests/packaging/test_publish_workflow.py
- Create: bindings/python/packaging/RELEASING.md

**Interfaces:**
- Consumes: accepted-wheel-set artifact, verified sdist, vX.Y.Z tag, setuptools-scm, GitHub protected environments, PyPI/TestPyPI trusted publishers, and twine.
- Produces: an immutable four-wheel plus sdist release, matching GitHub release/checksums, provenance-preserving OIDC publication, and a documented yank/recovery procedure.

- [ ] **Step 1: Write failing publication safety tests**

~~~python
class PublishWorkflowTest(unittest.TestCase):
    def test_publish_requires_tag_and_acceptance(self) -> None:
        workflow = load_yaml(".github/workflows/python-publish.yml")
        publish = workflow["jobs"]["publish-pypi"]
        self.assertIn("wheel-acceptance", publish["needs"])
        self.assertEqual(publish["permissions"]["id-token"], "write")
        self.assertEqual(publish["environment"]["name"], "pypi")
        self.assertNotIn("password", json.dumps(workflow).lower())

    def test_release_has_exact_artifact_count(self) -> None:
        self.assertReleaseSet(wheels=4, sdists=1,
                              checksums=True, audits=True)
~~~

- [ ] **Step 2: Run publication tests to verify RED**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_publish_workflow -v
~~~

Expected: protected publish workflow and release guide do not exist.

- [ ] **Step 3: Add tag and version consistency gates**

The workflow accepts vMAJOR.MINOR.PATCH tags and workflow_dispatch with repository=TestPyPI only. Before build/publish, verify:

- setuptools-scm resolves exactly MAJOR.MINOR.PATCH;
- CMake project version matches;
- sdist and all wheels share that version;
- hpactor.__version__ in a clean install matches;
- tag commit is reachable from main;
- the version is absent from the selected package index.

Any mismatch stops before requesting an OIDC token.

- [ ] **Step 4: Build once and publish the accepted artifact set**

Call the wheel workflow through workflow_call, build one sdist from the tagged checkout, run twine check, verify four wheels plus one sdist and all checksums/audits, then publish using pypa/gh-action-pypi-publish with trusted publishing.

TestPyPI uses protected environment testpypi. PyPI uses protected environment pypi with required reviewers. permissions are contents: read and id-token: write only on the publish job.

- [ ] **Step 5: Verify the published package before creating the release**

Resolve `RELEASE_VERSION` from the validated tag, create clean native environments, run `pip install "hpactor==$RELEASE_VERSION"` from the selected index on each supported platform, and rerun metadata/import/echo/shutdown smoke. Only then create a GitHub release attaching four wheels, one sdist, SHA256SUMS, wheel audit reports, and performance report.

- [ ] **Step 6: Document release and recovery**

RELEASING.md lists prerequisites, tag format, TestPyPI rehearsal, environment approvals, artifact review, PyPI publish, post-publish smoke, GitHub release, documentation deploy, and issue milestone update.

Recovery says never replace an artifact: stop the workflow before publish when possible; after publish, yank the bad version with a reason, fix on a new commit, and release a new patch version.

- [ ] **Step 7: Run workflow safety tests**

~~~bash
python3 -m unittest \
  bindings.python.tests.packaging.test_publish_workflow -v
python3 -m yamllint .github/workflows/python-publish.yml
~~~

Expected: tag/version, artifact-count, acceptance dependency, environment, OIDC, and no-secret tests pass.

- [ ] **Step 8: Commit trusted publishing**

~~~bash
git add .github/workflows/python-publish.yml pyproject.toml \
  bindings/python/packaging/RELEASING.md \
  bindings/python/tests/packaging/test_publish_workflow.py
git commit -m "ci: publish accepted hpactor wheels"
~~~

### Task 10: Complete Phase 1D acceptance, documentation status, and release evidence

**Files:**
- Modify: docs/manual/limitations.rst
- Modify: bindings/python/README.md
- Modify: CLAUDE_MEMORY.md
- Modify: docs/superpowers/specs/2026-07-03-python-language-binding-design.md

**Interfaces:**
- Consumes: complete Phase 1D package, four-platform CI, clean-minor compatibility, docs/examples, performance, and protected release evidence.
- Produces: honest official-binding documentation, final Phase 1D status, exact artifact/test counts, and a clean boundary before Phase 1E topology.

- [ ] **Step 1: Audit every packaging requirement**

Map design sections 15, 17.5, acceptance criterion 10, and Phase 1D delivery text to exact tasks, CI jobs, wheel names, audit reports, smoke tests, manual pages, and release evidence. Resolve every missing mapping before changing the manual limitation.

- [ ] **Step 2: Run the full source-tree build and tests**

~~~bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
git diff --check
~~~

Expected: complete C++/Python/architecture suite passes and git diff --check prints no output.

- [ ] **Step 3: Run local package, docs, and clean-wheel verification**

~~~bash
python3 -m build --sdist --wheel
python3 -m twine check dist/*
sphinx-build -W -b html docs/manual docs/manual/_build/html
python3 -m unittest discover \
  -s bindings/python/tests/packaging -p 'test_*.py' -v
python3 bindings/python/tests/wheel/run_clean_smoke.py \
  --python python3.11 \
  --wheel wheelhouse/hpactor-*-cp311-abi3-*.whl \
  --protobuf 7.35.0
~~~

Expected: metadata, sdist, wheel, docs, package tests, and clean smoke pass.

- [ ] **Step 4: Require the four-platform remote acceptance**

Do not mark Phase 1D complete until python-wheels.yml records:

- four successful native wheel build/repair/audit jobs;
- four native wheel smoke jobs;
- CPython 3.11/3.12/3.13/3.14 compatibility for each platform;
- protobuf 7.35.0 and latest-below-8 compatibility on CPython 3.11;
- one accepted-wheel-set with exactly four cp311-abi3 wheels;
- one same-runner performance report with no >20-percent regression;
- no unresolved binary dependency or architecture/deployment-target mismatch.

- [ ] **Step 5: Update the manual only after package availability is proven**

Replace the Language Bindings limitation with a support matrix:

~~~text
HPActor provides an official alpha Python binding for CPython 3.11 and newer
on manylinux_2_28 x86_64/ARM64 and macOS 12.0 x86_64/ARM64. The binding uses
generated protobuf messages and explicit TypeTags. Windows, musllinux, PyPy,
free-threaded CPython, and native remote-node participation are not supported
in Phase 1D.
~~~

Link to the Python manual, package index, issue #426, and compatibility policy. Keep Java, Go, Rust, and other languages listed as unsupported.

- [ ] **Step 6: Record exact Phase 1D evidence**

Add a dated Python Binding Phase 1D Packaging and Release entry to CLAUDE_MEMORY.md containing:

- package/version and protobuf dependency bounds;
- exact wheel filenames, sizes, SHA256 values, and audit tool versions;
- platform/architecture/deployment policy;
- CPython minor/protobuf compatibility matrix;
- clean smoke, package, docs, source-tree, and performance results;
- release URL and trusted-publishing run;
- remaining Phase 1E declarative-topology limitation.

Change the design status only after all gates pass:

~~~markdown
**Status:** Approved design; Phases 1A through 1D implemented and packaged
~~~

- [ ] **Step 7: Verify installed release and commit acceptance status**

~~~bash
RELEASE_VERSION="$(python3 -m setuptools_scm)"
python3 -m venv build/release-check
build/release-check/bin/pip install "hpactor==$RELEASE_VERSION"
build/release-check/bin/python -I -m unittest discover \
  -s bindings/python/tests/wheel -p 'test_*.py' -v
git diff --check
git status --short
~~~

Expected: the published artifact, not the checkout, passes and only intended documentation/status files remain.

~~~bash
git add docs/manual/limitations.rst bindings/python/README.md \
  CLAUDE_MEMORY.md \
  docs/superpowers/specs/2026-07-03-python-language-binding-design.md
git commit -m "docs: record Python wheel release status"
~~~

## Plan Completion Checklist

- [ ] Phases 1A, 1B, and 1C are implemented and passing before Phase 1D begins.
- [ ] pyproject.toml defines hpactor, Python >=3.11, protobuf>=7.35.0,<8, dynamic versioning, and wheel.py-api=cp311.
- [ ] hpactor.__version__, tag, project, wheel, sdist, and release versions match.
- [ ] _hpactor builds with Development.SABIModule, Py_LIMITED_API=0x030B0000, and cp311-abi3 tags.
- [ ] Wheel import starts no thread, runtime, network resource, or notifier.
- [ ] Wheel staging contains one native module, py.typed, license/metadata, and required private runtime libraries only.
- [ ] OpenSSL 3.5.5, Abseil 20260107.1, and protobuf 35.0 sources are checksum locked and built for the target.
- [ ] C++ protoc/generated/runtime protobuf versions match exactly.
- [ ] auditwheel/delocate repair succeeds and abi3audit --strict passes.
- [ ] No unresolved HPActor, protobuf, Abseil, OpenSSL, or forbidden C++ runtime dependency remains.
- [ ] Linux wheels are manylinux_2_28 x86_64 and aarch64 and pass on native runners.
- [ ] macOS wheels are separate macosx_12_0 x86_64 and arm64 and pass on native runners.
- [ ] The same repaired wheel passes clean installs on CPython 3.11, 3.12, 3.13, and 3.14 for each platform.
- [ ] Protobuf 7.35.0 and newest-below-8 runtime tests pass.
- [ ] CPython 3.10 installation is rejected by package metadata.
- [ ] Installed-wheel echo, ask/reply, delivery, restart, operations, typing, and shutdown smoke pass outside the checkout.
- [ ] Sphinx builds the complete Python manual with warnings as errors.
- [ ] Every published example runs against the installed wheel and uses explicit protobuf TypeTags.
- [ ] Same-runner performance evidence covers throughput, queue wait, handler latency, and end-to-end p50/p95/p99.
- [ ] Once the first stable baseline exists, every metric stays within the 20-percent regression budget.
- [ ] Pull-request/main wheel workflows have no publish permission.
- [ ] PyPI/TestPyPI publishing uses protected environments and OIDC trusted publishing without an API token.
- [ ] Published releases contain exactly four wheels, one sdist, checksums, audits, and performance evidence.
- [ ] Bad artifacts are yanked and replaced by a new version, never overwritten.
- [ ] The official-binding manual claim is changed only after four-platform clean-install evidence passes.
- [ ] Remaining Phase 1E declarative topology and unsupported platforms are documented explicitly.

## Execution Handoff

Plan complete. Execute only after the Phase 1A, Phase 1B, and Phase 1C acceptance checklists pass. Use superpowers:subagent-driven-development for one fresh implementer and review gate per task, or superpowers:executing-plans for inline batches with review checkpoints.
