# Python Binding Phase 2 External SDK Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a bounded, secure-by-default, pure-Python sync/async SDK for HPActor health, metrics, application HTTP gateway, and protobuf CLI surfaces, plus a universal client-only wheel alongside the four native ABI3 wheels.

**Architecture:** Independent typed health, metrics, gateway, and CLI clients share immutable values and failures. Health, metrics, and gateway use bounded HTTPX adapters; CLI uses a pure HPAC codec and distinct blocking/asyncio stream adapters. Thin sync/async bundles expose only configured capabilities, and lazy native imports let the same `hpactor` project publish both native and `py3-none-any` wheels.

**Tech Stack:** Python 3.11+, `httpx>=0.28.1,<0.29`, `protobuf>=7.35.0,<8`, asyncio streams, blocking sockets, generated Python protobuf modules, `unittest`, C++20 HPActor test fixtures, CMake/Ninja, `scikit-build-core>=0.11,<0.13`, `build`, cibuildwheel, auditwheel/delocate, abi3audit, GitHub Actions, and Sphinx.

## Global Constraints

- Execute this plan only after Phases 1A through 1E have produced the package, native module, wheel workflow, Python test layout, and manual pages named below. If those files are absent, execute their approved plans first rather than inventing a parallel layout.
- Work only in the isolated Python-binding worktree and use that worktree's `build/` directory.
- Keep `hpactor.client` independent of `hpactor._hpactor`; importing `hpactor` or `hpactor.client` creates no thread, socket, event loop, or network request.
- Support Python 3.11 or newer. Keep the native runtime matrix at CPython 3.11+ ABI3 on Linux x86_64/ARM64 and macOS x86_64/ARM64.
- Pin runtime dependencies to `protobuf>=7.35.0,<8` and `httpx>=0.28.1,<0.29`. Do not use HTTPX 1.0 pre-releases in Phase 2.
- Keep the default HTTP timeouts at connect/pool 5 seconds and read/write 30 seconds. Keep the default CLI connect timeout at 5 seconds and request timeout at 10 seconds.
- Keep response, inbound frame, and outbound frame defaults at 16 MiB. Store no more than 4 KiB of a response payload in an exception.
- Disable redirects and retries by default. An enabled retry policy has at most three attempts, starts at 100 ms, caps delay at 2 seconds, and never extends the total monotonic deadline.
- Never retry CLI commands. Never retry a non-idempotent gateway request without an explicit per-request idempotency declaration.
- Permit CLI TCP without an insecure-remote opt-in only for `127.0.0.0/8`, `::1`, or exact hostname `localhost`. Treat every other host as remote without consulting DNS to weaken that decision.
- Preserve HTTP semantics: health states are typed data, metrics remain raw exposition text, and gateway calls return fully buffered, bounded `httpx.Response` objects.
- Preserve CLI wire compatibility exactly: ASCII `HPAC`, unsigned big-endian 32-bit payload length, then serialized `CliCommand` or `CliResponse`.
- One CLI connection permits one in-flight request. Charge lock wait to the operation deadline and invalidate the stream after timeout, cancellation, protocol failure, EOF, or socket failure.
- Do not claim an automatic metrics listener or distinct liveness/readiness policy without runtime evidence. Interop tests expose metrics deliberately and record which health path was requested.
- Publish exactly four native ABI3 wheels, one `py3-none-any` client wheel, and one sdist. Native and universal wheels must have identical project name, version, pure-Python files, protobuf files, dependency metadata, and typing surface.
- Use TDD for every task: demonstrate RED, implement the smallest complete contract, demonstrate GREEN, then commit.

## File Structure

### Public package contracts

- Modify: `pyproject.toml` — add the verified HTTPX runtime range and pure-wheel build override inputs.
- Modify: `bindings/python/hpactor/__init__.py` — lazy native exports and client availability without `_hpactor`.
- Modify: `bindings/python/hpactor/_errors.py` — place `NativeBindingUnavailable` on the pure-Python side.
- Create: `bindings/python/hpactor/client/__init__.py` — public Phase 2 exports.
- Create: `bindings/python/hpactor/client/config.py` — frozen validated endpoint, timeout, limit, retry, CLI, and bundle configs.
- Create: `bindings/python/hpactor/client/errors.py` — public external-SDK error hierarchy.
- Create: `bindings/python/hpactor/client/models.py` — shared capability, health, metrics, CLI, ownership, and event values.
- Modify: `bindings/python/hpactor/py.typed` — continue marking the expanded package as typed.

### Bounded HTTP clients

- Create: `bindings/python/hpactor/client/_deadline.py` — monotonic deadline arithmetic shared by HTTP and CLI.
- Create: `bindings/python/hpactor/client/_http.py` — owned/injected sync and async HTTPX adapters, bounded buffering, timeout mapping, and retries.
- Create: `bindings/python/hpactor/client/health.py` — sync/async health parsing and enforcement.
- Create: `bindings/python/hpactor/client/metrics.py` — sync/async raw exposition retrieval and ETag handling.
- Create: `bindings/python/hpactor/client/gateway.py` — sync/async general HTTP gateway facade.

### CLI protocol and clients

- Create: `bindings/python/hpactor/client/_proto/__init__.py` — package marker for generated CLI protobufs.
- Create: `bindings/python/hpactor/client/_proto/cli_pb2.py` — generated from `protos/hpactor/cli.proto`.
- Create: `bindings/python/hpactor/client/_proto/cli_messages_pb2.py` — generated from `protos/hpactor/cli_messages.proto`.
- Create: `bindings/python/tools/generate_client_protos.py` — pinned, reproducible generation/check entry point.
- Create: `bindings/python/hpactor/client/_hpac.py` — pure framing encoder/decoder.
- Create: `bindings/python/hpactor/client/_sync_stream.py` — exact blocking UDS/TCP reads, writes, and close.
- Create: `bindings/python/hpactor/client/_async_stream.py` — exact asyncio UDS/TCP reads, writes, cancellation, and close.
- Create: `bindings/python/hpactor/client/cli.py` — sync/async command-tree and structured CLI APIs.

### Bundles and observability

- Create: `bindings/python/hpactor/client/bundle.py` — configured capability construction and deterministic close.
- Create: `bindings/python/hpactor/client/_events.py` — redacted bounded request-event delivery and hook isolation.

### Tests and runtime interoperability

- Create: `bindings/python/tests/unit/client/` — config, errors, deadline, HTTP, health, metrics, gateway, HPAC, streams, CLI, bundle, events, and public API tests.
- Create: `bindings/python/tests/support/client_contract.py` — the sync/async contract harness.
- Create: `bindings/python/tests/integration/client/test_runtime_interop.py` — launches the real C++ fixture and exercises every client.
- Create: `tests/integration/python/python_external_sdk_fixture.cpp` — real health, explicit metrics, gateway, UDS CLI, and loopback TCP CLI listeners.
- Modify: `tests/integration/python/CMakeLists.txt` — build the fixture.
- Modify: `bindings/python/tests/CMakeLists.txt` — register client unit and interop suites.

### Packaging, CI, docs, and evidence

- Create: `bindings/python/packaging/build_client_wheel.py` — reproducible CMake-free `py3-none-any` build wrapper.
- Create: `bindings/python/tests/packaging/test_client_wheel.py` — universal-wheel content and metadata checks.
- Create: `bindings/python/tests/packaging/test_wheel_selection.py` — installer tag ranking and five-wheel set checks.
- Create: `bindings/python/tests/wheel/test_client_smoke.py` — installed sync/async client-only smoke.
- Modify: `bindings/python/tests/wheel/run_clean_smoke.py` — distinguish native and client-only smoke modes.
- Modify: `bindings/python/packaging/verify_wheel.py` — verify universal and native content policies.
- Modify: `.github/workflows/python-wheels.yml` — build, test, and accept five wheels.
- Modify: `.github/workflows/python-publish.yml` — require five wheels plus one sdist.
- Create: `bindings/python/examples/external_client.py` — executable sync/async client example.
- Create: `docs/manual/python/external-client.rst` — complete client guide and server prerequisites.
- Modify: `docs/manual/python/index.rst` — link the guide.
- Modify: `docs/manual/python/installation.rst` — explain native versus universal selection.
- Modify: `docs/manual/python/api.rst` — add client API reference.
- Modify: `docs/manual/monitoring/health.rst` — correct path-policy claims.
- Modify: `docs/manual/monitoring/metrics.rst` — require proven metrics exposure.
- Modify: `docs/manual/operations/cli-server.rst` — document HPAC client use and insecure TCP boundary.
- Modify: `docs/manual/operations/http-gateway.rst` — distinguish application gateway from admin/health/metrics listeners.
- Modify: `docs/manual/limitations.rst` — separate native and portable-client support.
- Modify: `CLAUDE_MEMORY.md` — record only verified Phase 2 status and test evidence.
- Modify: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md` — record verified delivery status.
- Modify: `docs/superpowers/specs/2026-07-06-python-binding-phase2-external-sdk-design.md` — record acceptance evidence without changing approved contracts.

---

### Task 1: Establish lazy imports and immutable public client contracts

**Files:**
- Modify: `pyproject.toml`
- Modify: `bindings/python/hpactor/__init__.py`
- Modify: `bindings/python/hpactor/_errors.py`
- Create: `bindings/python/hpactor/client/__init__.py`
- Create: `bindings/python/hpactor/client/config.py`
- Create: `bindings/python/hpactor/client/errors.py`
- Create: `bindings/python/hpactor/client/models.py`
- Create: `bindings/python/tests/unit/client/test_public_contract.py`
- Create: `bindings/python/tests/unit/client/test_config.py`

**Interfaces:**
- Consumes: Phase 1 `hpactor` package, `hpactor.__version__`, and `protobuf>=7.35.0,<8` metadata.
- Produces: `HPActorClientError`, its typed subclasses, `HttpTimeouts`, `HttpLimits`, `RetryPolicy`, `HttpEndpointConfig`, `HealthClientConfig`, `MetricsClientConfig`, `GatewayClientConfig`, `CliClientConfig`, `HPActorClientConfig`, `Capability`, `TransportOwnership`, and lazy `NativeBindingUnavailable` behavior.

- [ ] **Step 1: Write failing dependency, import, default, validation, and redaction tests**

```python
class PublicContractTest(unittest.TestCase):
    def test_client_import_does_not_touch_native_module(self) -> None:
        script = (
            "import sys; "
            "sys.path.insert(0, 'bindings/python'); "
            "import hpactor.client; "
            "assert 'hpactor._hpactor' not in sys.modules"
        )
        subprocess.run([sys.executable, "-I", "-c", script], check=True,
                       cwd=REPO_ROOT)

    def test_metadata_has_exact_runtime_ranges(self) -> None:
        project = tomllib.loads(Path("pyproject.toml").read_text())["project"]
        self.assertEqual(project["dependencies"], [
            "protobuf>=7.35.0,<8",
            "httpx>=0.28.1,<0.29",
        ])

class ConfigTest(unittest.TestCase):
    def test_defaults_are_bounded_and_immutable(self) -> None:
        endpoint = HttpEndpointConfig(base_url="https://node.example")
        self.assertEqual(endpoint.timeouts, HttpTimeouts(5.0, 30.0, 30.0, 5.0))
        self.assertEqual(endpoint.limits.max_response_bytes, 16 * 1024 * 1024)
        self.assertFalse(endpoint.follow_redirects)
        with self.assertRaises(FrozenInstanceError):
            endpoint.follow_redirects = True

    def test_remote_cli_requires_explicit_opt_in(self) -> None:
        with self.assertRaises(InsecureTransportError):
            CliClientConfig(uds_path=None, host="node.example", port=7777)
        config = CliClientConfig(
            uds_path=None, host="node.example", port=7777,
            allow_insecure_remote_tcp=True,
        )
        self.assertEqual(config.port, 7777)
```

- [ ] **Step 2: Run the focused tests to verify RED**

Run:

```bash
python3 -m unittest discover -s bindings/python/tests/unit/client -p 'test_config.py' -v
python3 -m unittest bindings.python.tests.unit.client.test_public_contract -v
```

Expected: FAIL because `hpactor.client`, the config values, and the HTTPX dependency do not exist.

- [ ] **Step 3: Implement the error and configuration contracts**

Use frozen, slotted dataclasses and exact defaults:

```python
@dataclass(frozen=True, slots=True)
class HttpTimeouts:
    connect: float = 5.0
    read: float = 30.0
    write: float = 30.0
    pool: float = 5.0

@dataclass(frozen=True, slots=True)
class HttpLimits:
    max_keepalive_connections: int = 20
    max_connections: int = 100
    max_response_bytes: int = 16 * 1024 * 1024

@dataclass(frozen=True, slots=True)
class RetryPolicy:
    attempts: int = 1
    initial_delay: float = 0.1
    max_delay: float = 2.0
    retry_statuses: frozenset[int] = frozenset({429, 502, 503, 504})

@dataclass(frozen=True, slots=True)
class HttpEndpointConfig:
    base_url: str
    headers: tuple[tuple[str, str], ...] = ()
    verify: bool | ssl.SSLContext = True
    cert: str | tuple[str, str] | None = None
    follow_redirects: bool = False
    timeouts: HttpTimeouts = field(default_factory=HttpTimeouts)
    limits: HttpLimits = field(default_factory=HttpLimits)
    retry: RetryPolicy = field(default_factory=RetryPolicy)
```

Validate finite positive timeouts, positive capacities, retry attempts in
`1..3`, absolute HTTP(S) base URLs without userinfo, slash-prefixed health and
metrics paths, exactly one CLI endpoint, port `1..65535`, the three loopback
forms, and at least one bundle capability. Store headers as a tuple, reject
CR/LF, and redact authorization, cookie, proxy-authorization, and certificate
key values from `repr`.

- [ ] **Step 4: Make root native exports lazy and publish the initial client namespace**

Keep pure exports eager and resolve native names through one lazy table:

```python
_NATIVE_EXPORTS = frozenset({"ActorSystem"})

def __getattr__(name: str) -> object:
    if name not in _NATIVE_EXPORTS:
        raise AttributeError(name)
    try:
        native = importlib.import_module("hpactor._hpactor")
    except ImportError as exc:
        raise NativeBindingUnavailable(
            name=name,
            implementation=sys.implementation.name,
            platform=sys.platform,
        ) from exc
    value = getattr(native, name)
    globals()[name] = value
    return value
```

Export only contracts already implemented in this task from
`hpactor.client.__init__`; later tasks extend `__all__` as their clients land.
Keep the Phase 1 value-only `Actor`, `ActorContext`, addresses, behaviors,
messages, delivery values, and errors imported from pure modules. Add a test
that every existing Phase 1 `__all__` name except `ActorSystem` remains
available without importing `_hpactor`.

- [ ] **Step 5: Run tests and verify GREEN**

Run:

```bash
python3 -m unittest discover -s bindings/python/tests/unit/client -p 'test_*.py' -v
python3 -m unittest bindings.python.tests.unit.test_import_quiescent -v
```

Expected: all new contract tests and the existing Phase 1 import-quiescence tests PASS.

- [ ] **Step 6: Commit the public foundation**

```bash
git add pyproject.toml bindings/python/hpactor bindings/python/tests/unit/client
git commit -m "feat(python): add external client contracts"
```

### Task 2: Build the monotonic deadline and bounded HTTP transport core

**Files:**
- Create: `bindings/python/hpactor/client/_deadline.py`
- Create: `bindings/python/hpactor/client/_http.py`
- Create: `bindings/python/tests/unit/client/test_deadline.py`
- Create: `bindings/python/tests/unit/client/test_http_transport.py`

**Interfaces:**
- Consumes: `HttpEndpointConfig`, `HttpLimits`, `RetryPolicy`, `OperationTimeout`, `ResponseLimitError`, `TransportError`, and HTTPX sync/async clients.
- Produces: `Deadline.after(seconds)`, `Deadline.remaining()`, `SyncHttpTransport.request(...) -> httpx.Response`, `AsyncHttpTransport.request(...) -> httpx.Response`, explicit transport ownership, bounded buffering, and retry events.

- [ ] **Step 1: Write failing deadline, body-bound, timeout, retry, and ownership tests**

```python
class HttpTransportTest(unittest.IsolatedAsyncioTestCase):
    async def test_async_stream_stops_before_exceeding_bound(self) -> None:
        async def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"x" * 17)
        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        transport = AsyncHttpTransport(endpoint(max_response_bytes=16), raw)
        with self.assertRaises(ResponseLimitError) as caught:
            await transport.request("GET", "/metrics")
        self.assertEqual(caught.exception.limit, 16)
        self.assertFalse(raw.is_closed)
        await transport.aclose()
        self.assertFalse(raw.is_closed)

    async def test_retry_is_bounded_by_attempt_and_deadline(self) -> None:
        calls = 0
        async def handler(request: httpx.Request) -> httpx.Response:
            nonlocal calls
            calls += 1
            return httpx.Response(503)
        transport = make_async_transport(handler, attempts=3)
        response = await transport.request("GET", "/readyz", idempotent=True)
        self.assertEqual(response.status_code, 503)
        self.assertEqual(calls, 3)
```

Mirror the bound and ownership cases for `SyncHttpTransport`. Add fake-clock
tests proving lock/sleep/request phases cannot extend `Deadline`.

- [ ] **Step 2: Run focused tests to verify RED**

Run:

```bash
python3 -m unittest bindings.python.tests.unit.client.test_deadline -v
python3 -m unittest bindings.python.tests.unit.client.test_http_transport -v
```

Expected: FAIL because `_deadline` and `_http` are absent.

- [ ] **Step 3: Implement deadline arithmetic and exception mapping**

```python
@dataclass(frozen=True, slots=True)
class Deadline:
    expires_at: float
    clock: Callable[[], float] = field(default=time.monotonic, repr=False, compare=False)

    @classmethod
    def after(cls, seconds: float, *, clock=time.monotonic) -> "Deadline":
        if not math.isfinite(seconds) or seconds <= 0:
            raise ValueError("deadline must be positive and finite")
        return cls(clock() + seconds, clock)

    def remaining(self) -> float:
        value = self.expires_at - self.clock()
        if value <= 0:
            raise OperationTimeout(phase="total")
        return value
```

Map `httpx.ConnectError`, `PoolTimeout`, `WriteTimeout`, and `ReadTimeout` to
typed SDK failures while retaining the original exception as `__cause__`.
Never catch `asyncio.CancelledError`.

- [ ] **Step 4: Implement owned/injected sync and async transports**

Construct HTTPX clients only when one is not injected. Use `stream()` and append
chunks only after checking `len(body) + len(chunk) <= max_response_bytes`.
Return a detached response with the original request, status, headers, bounded
content, and extensions. Retry only idempotent requests, close each streamed
response before sleeping, clamp sleep to `Deadline.remaining()`, and emit no
more attempts than `RetryPolicy.attempts`.

```python
def _detached(response: httpx.Response, body: bytes) -> httpx.Response:
    return httpx.Response(
        response.status_code,
        headers=response.headers,
        content=body,
        request=response.request,
        extensions=dict(response.extensions),
    )
```

Close an owned HTTPX client exactly once. Never close an injected client.

- [ ] **Step 5: Run transport tests and the HTTPX supported-range smoke**

Run:

```bash
python3 -m unittest bindings.python.tests.unit.client.test_deadline -v
python3 -m unittest bindings.python.tests.unit.client.test_http_transport -v
python3 -m pip check
```

Expected: tests PASS and `pip check` reports no broken requirements.

- [ ] **Step 6: Commit the bounded HTTP core**

```bash
git add bindings/python/hpactor/client/_deadline.py \
  bindings/python/hpactor/client/_http.py \
  bindings/python/tests/unit/client/test_deadline.py \
  bindings/python/tests/unit/client/test_http_transport.py
git commit -m "feat(python): add bounded HTTP client transport"
```

### Task 3: Implement sync and async health clients

**Files:**
- Create: `bindings/python/hpactor/client/health.py`
- Modify: `bindings/python/hpactor/client/models.py`
- Modify: `bindings/python/hpactor/client/__init__.py`
- Create: `bindings/python/tests/unit/client/test_health.py`

**Interfaces:**
- Consumes: `HealthClientConfig`, sync/async HTTP transports, `HttpResponseError`, `ProtocolError`, and `HealthCheckFailed`.
- Produces: `HealthProbe`, `HealthState`, `HealthCheck`, `HealthResult`, `HealthClient`, and `AsyncHealthClient` with `liveness`, `readiness`, `require_live`, and `require_ready`.

- [ ] **Step 1: Write failing table-driven sync/async health tests**

```python
HEALTH_CASES = (
    (200, None, b"OK", HealthState.HEALTHY, ()),
    (200, "application/json", degraded_json(), HealthState.DEGRADED,
     (HealthCheck("scheduler", HealthState.DEGRADED, "backlog"),)),
    (503, "application/json", unhealthy_json(), HealthState.UNHEALTHY,
     (HealthCheck("network", HealthState.UNHEALTHY, "listener down"),)),
)

class HealthClientTest(unittest.TestCase):
    def test_readiness_preserves_probe_and_current_server_body(self) -> None:
        client = make_health_client(200, None, b"OK")
        result = client.readiness()
        self.assertEqual(result.probe, HealthProbe.READINESS)
        self.assertEqual(result.state, HealthState.HEALTHY)
        self.assertEqual(result.raw_body, b"OK")

    def test_require_ready_raises_for_degraded_data(self) -> None:
        result_client = make_health_client(200, "application/json", degraded_json())
        with self.assertRaises(HealthCheckFailed) as caught:
            result_client.require_ready()
        self.assertEqual(caught.exception.result.state, HealthState.DEGRADED)
```

Add equivalent async cases, malformed advertised JSON, unknown JSON status,
unexpected non-2xx, oversized body, and path assertions for both default paths.

- [ ] **Step 2: Run health tests to verify RED**

Run: `python3 -m unittest bindings.python.tests.unit.client.test_health -v`

Expected: FAIL because health models and clients do not exist.

- [ ] **Step 3: Implement the conservative health parser**

Implement one pure parser used by both clients:

```python
def parse_health(probe: HealthProbe, response: httpx.Response,
                 elapsed: float) -> HealthResult:
    body = response.content
    if 200 <= response.status_code < 300 and body.strip() == b"OK":
        return HealthResult(probe, HealthState.HEALTHY, response.status_code,
                            (), response.headers.get("content-type"), body, elapsed)
    if _is_json(response):
        payload = _decode_health_json(body)
        state = HealthState.from_wire(payload.get("status"))
        checks = tuple(_parse_check(item) for item in payload.get("checks", ()))
        if response.status_code == 503 and state is HealthState.UNHEALTHY:
            return HealthResult(probe, state, 503, checks,
                                response.headers.get("content-type"), body, elapsed)
        if 200 <= response.status_code < 300:
            return HealthResult(probe, state, response.status_code, checks,
                                response.headers.get("content-type"), body, elapsed)
    raise HttpResponseError.from_response(response)
```

Reject non-object JSON, non-array checks, missing/non-string check names, and
malformed JSON with `ProtocolError`. Do not infer probe policy from the path.

- [ ] **Step 4: Implement sync/async clients and enforcement helpers**

Have both clients call the same parser and preserve elapsed monotonic duration.
`require_live` and `require_ready` accept only `HEALTHY`; return the successful
`HealthResult` and raise `HealthCheckFailed(result)` otherwise.

- [ ] **Step 5: Run health and shared import tests**

Run:

```bash
python3 -m unittest bindings.python.tests.unit.client.test_health -v
python3 -m unittest bindings.python.tests.unit.client.test_public_contract -v
```

Expected: PASS for sync, async, parse, enforcement, and public exports.

- [ ] **Step 6: Commit health clients**

```bash
git add bindings/python/hpactor/client bindings/python/tests/unit/client/test_health.py
git commit -m "feat(python): add health clients"
```

### Task 4: Implement raw metrics retrieval with ETag semantics

**Files:**
- Create: `bindings/python/hpactor/client/metrics.py`
- Modify: `bindings/python/hpactor/client/models.py`
- Modify: `bindings/python/hpactor/client/__init__.py`
- Create: `bindings/python/tests/unit/client/test_metrics.py`

**Interfaces:**
- Consumes: `MetricsClientConfig`, bounded HTTP transports, `HttpResponseError`, and `ProtocolError`.
- Produces: `MetricsSnapshot`, `MetricsNotModified`, `MetricsClient.scrape`, and `AsyncMetricsClient.scrape` returning `MetricsSnapshot | MetricsNotModified`.

- [ ] **Step 1: Write failing sync/async metrics tests**

```python
class MetricsClientTest(unittest.TestCase):
    def test_scrape_preserves_exposition_text_exactly(self) -> None:
        body = b'# TYPE hpactor_actor_count gauge\nhpactor_actor_count 3\n'
        client = make_metrics_client(200, "text/plain; version=0.0.4", body,
                                     etag='"snapshot-7"')
        result = client.scrape()
        self.assertIsInstance(result, MetricsSnapshot)
        self.assertEqual(result.text.encode(), body)
        self.assertEqual(result.etag, '"snapshot-7"')

    def test_etag_304_is_not_an_empty_snapshot(self) -> None:
        client = make_metrics_client(304, None, b"")
        result = client.scrape(etag='"snapshot-7"')
        self.assertEqual(result, MetricsNotModified(etag='"snapshot-7"'))
```

Add OpenMetrics media type, case-insensitive parameters, missing/invalid media
type, invalid UTF-8, non-2xx, body bound, request `If-None-Match`, and async
parity cases.

- [ ] **Step 2: Run metrics tests to verify RED**

Run: `python3 -m unittest bindings.python.tests.unit.client.test_metrics -v`

Expected: FAIL because metrics clients and result values do not exist.

- [ ] **Step 3: Implement the pure result parser**

Accept `text/plain` and `application/openmetrics-text` regardless of parameter
order. Decode strict UTF-8 and preserve the resulting text without trimming or
reformatting. Return `MetricsNotModified` only for status 304 with a caller
ETag. Raise `HttpResponseError` for every other non-2xx response.

```python
def _parse_metrics(response: httpx.Response, collected_at: datetime,
                   elapsed: float, requested_etag: str | None):
    if response.status_code == 304 and requested_etag is not None:
        return MetricsNotModified(requested_etag)
    if not 200 <= response.status_code < 300:
        raise HttpResponseError.from_response(response)
    media_type = _metrics_media_type(response.headers.get("content-type"))
    try:
        text = response.content.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ProtocolError("metrics body is not UTF-8") from exc
    return MetricsSnapshot(text, media_type, response.status_code, collected_at,
                           elapsed, response.headers.get("etag"))
```

- [ ] **Step 4: Add sync/async request paths and exports**

Set `Accept` to both supported exposition formats, add `If-None-Match` only
when supplied, and use the configured `/metrics` path without claiming that the
server publishes it automatically.

- [ ] **Step 5: Run metrics tests and commit**

Run: `python3 -m unittest bindings.python.tests.unit.client.test_metrics -v`

Expected: PASS.

```bash
git add bindings/python/hpactor/client bindings/python/tests/unit/client/test_metrics.py
git commit -m "feat(python): add metrics clients"
```

### Task 5: Implement the bounded general HTTP gateway clients

**Files:**
- Create: `bindings/python/hpactor/client/gateway.py`
- Modify: `bindings/python/hpactor/client/__init__.py`
- Create: `bindings/python/tests/unit/client/test_gateway.py`

**Interfaces:**
- Consumes: `GatewayClientConfig`, bounded HTTP transports, HTTPX request options, and `HttpResponseError`.
- Produces: `GatewayClient`, `AsyncGatewayClient`, `request`, `request_checked`, and `get/post/put/patch/delete` methods returning detached bounded `httpx.Response`.

- [ ] **Step 1: Write failing method, URL-policy, response, and retry tests**

```python
class GatewayClientTest(unittest.TestCase):
    def test_non_success_is_returned_unchanged(self) -> None:
        client, seen = make_gateway_client(status=429, body=b"busy")
        response = client.post("/orders", json={"id": 7})
        self.assertEqual(response.status_code, 429)
        self.assertEqual(response.content, b"busy")
        self.assertEqual(seen[0].method, "POST")

    def test_absolute_url_is_rejected_by_default(self) -> None:
        client, _ = make_gateway_client(status=200)
        with self.assertRaises(ConfigurationError):
            client.get("https://other.example/escape")

    def test_checked_request_maps_non_success(self) -> None:
        client, _ = make_gateway_client(status=404, body=b"missing")
        with self.assertRaises(HttpResponseError) as caught:
            client.request_checked("GET", "/unknown")
        self.assertEqual(caught.exception.status_code, 404)
```

Add query, headers, content, JSON, idempotent GET retry, non-idempotent POST no
retry, explicit POST idempotency, redirect-disabled, response-limit, and async
equivalence tests.

- [ ] **Step 2: Run gateway tests to verify RED**

Run: `python3 -m unittest bindings.python.tests.unit.client.test_gateway -v`

Expected: FAIL because gateway clients do not exist.

- [ ] **Step 3: Implement safe path resolution and public methods**

Use `httpx.URL` components rather than string concatenation. Reject userinfo,
fragments, and a changed origin unless `allow_absolute_url=True`. Pass only
documented HTTPX request arguments: params, headers, cookies, content, data,
files, json, auth, and extensions.

```python
def request(self, method: str, path: str, *, idempotent: bool | None = None,
            allow_absolute_url: bool = False, **options: object) -> httpx.Response:
    url = resolve_gateway_url(self._config.endpoint.base_url, path,
                              allow_absolute_url=allow_absolute_url)
    safe = method.upper() in {"GET", "HEAD"} if idempotent is None else idempotent
    return self._transport.request(method, url, idempotent=safe, **options)
```

`request_checked` calls `request`, returns 2xx responses, and otherwise raises
`HttpResponseError.from_response` with at most 4 KiB diagnostic content.

- [ ] **Step 4: Run gateway tests and full HTTP client suite**

Run: `python3 -m unittest discover -s bindings/python/tests/unit/client -p 'test_*http*.py' -v`

Expected: bounded transport and gateway tests PASS.

- [ ] **Step 5: Commit gateway clients**

```bash
git add bindings/python/hpactor/client/gateway.py \
  bindings/python/hpactor/client/__init__.py \
  bindings/python/tests/unit/client/test_gateway.py
git commit -m "feat(python): add HTTP gateway clients"
```

### Task 6: Generate CLI protobuf modules and implement the pure HPAC codec

**Files:**
- Create: `bindings/python/tools/generate_client_protos.py`
- Create: `bindings/python/hpactor/client/_proto/__init__.py`
- Create: `bindings/python/hpactor/client/_proto/cli_pb2.py`
- Create: `bindings/python/hpactor/client/_proto/cli_messages_pb2.py`
- Create: `bindings/python/hpactor/client/_hpac.py`
- Create: `bindings/python/tests/unit/client/test_proto_generation.py`
- Create: `bindings/python/tests/unit/client/test_hpac.py`

**Interfaces:**
- Consumes: `protos/hpactor/cli.proto`, `protos/hpactor/cli_messages.proto`, the Phase 1 pinned protoc toolchain, `ResponseLimitError`, and `ProtocolError`.
- Produces: reproducible checked-in Python protobuf modules, public re-exports of CLI protobuf classes, `encode_frame(payload, max_payload_bytes) -> bytes`, sync `read_frame`, and async `read_frame_async` over exact-reader callables.

- [ ] **Step 1: Write failing regeneration and byte-compatibility tests**

```python
class HpacCodecTest(unittest.TestCase):
    def test_encode_matches_cpp_command_utils(self) -> None:
        command = CliCommand(path="system/stats", format="json")
        payload = command.SerializeToString(deterministic=True)
        encoded = encode_frame(payload, max_payload_bytes=1024)
        self.assertEqual(encoded[:4], b"HPAC")
        self.assertEqual(encoded[4:8], struct.pack("!I", len(payload)))
        self.assertEqual(encoded[8:], payload)

    def test_declared_oversize_fails_before_payload_read(self) -> None:
        reader = ExactReader(b"HPAC" + struct.pack("!I", 17))
        with self.assertRaises(ResponseLimitError):
            read_frame(reader.read_exact, max_payload_bytes=16)
        self.assertEqual(reader.bytes_requested, 8)
```

Add invalid magic, EOF at each header byte, EOF at each payload boundary,
zero-length valid protobuf, exactly-at-limit, one-over-limit, two concatenated
frames, malformed response protobuf, and async exact-reader cases. The
generation test runs `generate_client_protos.py --check` and expects no diff.

- [ ] **Step 2: Run codec and generation tests to verify RED**

Run:

```bash
python3 -m unittest bindings.python.tests.unit.client.test_proto_generation -v
python3 -m unittest bindings.python.tests.unit.client.test_hpac -v
```

Expected: FAIL because generated modules, generator, and codec are absent.

- [ ] **Step 3: Add deterministic protobuf generation**

The script accepts `--protoc`, `--output`, and `--check`. It generates into a
temporary directory, rewrites only generated absolute sibling imports to
package-relative imports, normalizes line endings, and either atomically
copies both files or byte-compares them with the checked-in copies. It exits 1
and prints mismatched paths in check mode.

Run the pinned Phase 1 protoc:

```bash
python3 bindings/python/tools/generate_client_protos.py \
  --protoc build/_deps/protobuf-build/protoc \
  --output bindings/python/hpactor/client/_proto
```

Expected: both generated files contain descriptors for package `hpactor.cli`.

- [ ] **Step 4: Implement bounded framing with exact readers**

```python
MAGIC = b"HPAC"
HEADER = struct.Struct("!4sI")

def encode_frame(payload: bytes, *, max_payload_bytes: int) -> bytes:
    size = len(payload)
    if size > max_payload_bytes:
        raise ResponseLimitError(limit=max_payload_bytes, observed=size,
                                 resource="cli outbound frame")
    return HEADER.pack(MAGIC, size) + payload

def read_frame(read_exact: Callable[[int], bytes], *, max_payload_bytes: int) -> bytes:
    magic, size = HEADER.unpack(read_exact(HEADER.size))
    if magic != MAGIC:
        raise ProtocolError("invalid HPAC magic")
    if size > max_payload_bytes:
        raise ResponseLimitError(limit=max_payload_bytes, observed=size,
                                 resource="cli inbound frame")
    return read_exact(size)
```

Map short exact reads to a typed truncated-frame `ProtocolError`. The async
function follows the same ordering and awaits `read_exact` once for header and
once for payload.

- [ ] **Step 5: Run regeneration, descriptor, and codec tests**

Run:

```bash
python3 bindings/python/tools/generate_client_protos.py \
  --protoc build/_deps/protobuf-build/protoc --check
python3 -m unittest bindings.python.tests.unit.client.test_proto_generation -v
python3 -m unittest bindings.python.tests.unit.client.test_hpac -v
```

Expected: generation check and all codec tests PASS.

- [ ] **Step 6: Commit generated protocol support**

```bash
git add bindings/python/tools/generate_client_protos.py \
  bindings/python/hpactor/client/_proto bindings/python/hpactor/client/_hpac.py \
  bindings/python/tests/unit/client/test_proto_generation.py \
  bindings/python/tests/unit/client/test_hpac.py
git commit -m "feat(python): add HPAC CLI protocol codec"
```

### Task 7: Implement secure CLI streams and sync/async CLI clients

**Files:**
- Create: `bindings/python/hpactor/client/_sync_stream.py`
- Create: `bindings/python/hpactor/client/_async_stream.py`
- Create: `bindings/python/hpactor/client/cli.py`
- Modify: `bindings/python/hpactor/client/models.py`
- Modify: `bindings/python/hpactor/client/__init__.py`
- Create: `bindings/python/tests/unit/client/test_cli_streams.py`
- Create: `bindings/python/tests/unit/client/test_cli.py`

**Interfaces:**
- Consumes: `CliClientConfig`, `Deadline`, generated request/reply classes, HPAC codec, and CLI typed errors.
- Produces: `CliResult`, `CliClient`, `AsyncCliClient`, lazy `connect`, idempotent close, command-tree `execute`, low-level `call`, and typed inspect/kill/quarantine/unquarantine/list/system/memory methods.

- [ ] **Step 1: Write failing endpoint-policy, exact-I/O, concurrency, cancellation, and RPC tests**

```python
class CliClientTest(unittest.TestCase):
    def test_execute_serializes_command_and_preserves_error(self) -> None:
        server = ScriptedCliServer(response=CliResponse(
            content_type="text/plain", payload=b"denied", is_error=True,
            error_code=9,
        ))
        client = CliClient(loopback_config(server.port))
        with self.assertRaises(CliCommandError) as caught:
            client.execute("actor/7/kill", params={"force": "true"})
        self.assertEqual(caught.exception.error_code, 9)
        self.assertEqual(caught.exception.payload, b"denied")

    def test_structured_inspect_parses_exact_reply(self) -> None:
        expected = InspectStateReply(actor_id=7)
        client = scripted_client(structured_response(expected))
        self.assertEqual(client.inspect(7), expected)
```

Async tests launch a scripted `asyncio.start_server`, start two concurrent
calls and assert the second command arrives only after the first response,
cancel after the server receives a full command, assert the connection closes,
and assert the next explicit request opens a new connection without replaying
the cancelled command. Mirror timeout-after-write for sync sockets.

- [ ] **Step 2: Run CLI tests to verify RED**

Run:

```bash
python3 -m unittest bindings.python.tests.unit.client.test_cli_streams -v
python3 -m unittest bindings.python.tests.unit.client.test_cli -v
```

Expected: FAIL because stream adapters and public clients do not exist.

- [ ] **Step 3: Implement exact blocking and asyncio streams**

Blocking streams set socket timeouts to `Deadline.remaining()` before each
connect/read/write. `read_exact` loops until `n` bytes or raises truncated-frame
`ProtocolError`; `write_all` loops over partial sends. Async streams use
`asyncio.wait_for` with the remaining deadline around `open_unix_connection`,
`open_connection`, `readexactly`, `drain`, and `wait_closed`.

Both adapters implement `connect`, `read_frame`, `write_frame`, `connected`,
and idempotent `close/aclose`. They never reconnect or replay internally.

- [ ] **Step 4: Implement one-in-flight command exchange**

```python
def _exchange(self, command: CliCommand, deadline: Deadline) -> CliResponse:
    if not self._lock.acquire(timeout=deadline.remaining()):
        raise OperationTimeout(phase="cli request lock")
    try:
        stream = self._ensure_connected(deadline)
        try:
            payload = command.SerializeToString(deterministic=True)
            stream.write_frame(payload, deadline)
            raw = stream.read_frame(deadline)
            response = CliResponse()
            try:
                response.ParseFromString(raw)
            except DecodeError as exc:
                raise ProtocolError("invalid CliResponse protobuf") from exc
            return response
        except BaseException:
            stream.close()
            self._stream = None
            raise
    finally:
        self._lock.release()
```

The async version awaits `asyncio.wait_for(self._lock.acquire(),
deadline.remaining())`, releases the lock in `finally`, re-raises
`CancelledError`, and invalidates the stream for cancellation and all
transport/protocol failures. A remote `CliCommandError` does not invalidate an
otherwise valid stream.

- [ ] **Step 5: Implement command-tree and exact structured methods**

Build `CliCommand(path=..., params=..., args=..., format=...)` without shell
concatenation. Implement RPC mapping exactly:

```python
_RPC_REPLIES = {
    "inspect": InspectStateReply,
    "kill": KillReply,
    "quarantine": QuarantineReply,
    "enumerate": ListActorsReply,
    "system_stats": SystemStatsReply,
    "memory_stats": MemoryStatsReply,
}
```

`unquarantine` sends `QuarantineRequest(unquarantine=True)`. `call` requires a
protobuf `Message` request and reply class, verifies `is_structured`, parses the
payload, and maps server `is_error` to `CliCommandError` with a 4 KiB diagnostic
cap.

- [ ] **Step 6: Run CLI tests plus a descriptor compatibility check**

Run:

```bash
python3 -m unittest bindings.python.tests.unit.client.test_cli_streams -v
python3 -m unittest bindings.python.tests.unit.client.test_cli -v
python3 -m unittest bindings.python.tests.unit.client.test_hpac -v
```

Expected: all stream, cancellation, serialization, RPC, and codec tests PASS.

- [ ] **Step 7: Commit CLI clients**

```bash
git add bindings/python/hpactor/client bindings/python/tests/unit/client/test_cli*.py
git commit -m "feat(python): add protobuf CLI clients"
```

### Task 8: Add capability bundles, deterministic close, and redacted request hooks

**Files:**
- Create: `bindings/python/hpactor/client/_events.py`
- Create: `bindings/python/hpactor/client/bundle.py`
- Modify: `bindings/python/hpactor/client/models.py`
- Modify: `bindings/python/hpactor/client/__init__.py`
- Modify: `bindings/python/hpactor/client/_http.py`
- Modify: `bindings/python/hpactor/client/health.py`
- Modify: `bindings/python/hpactor/client/metrics.py`
- Modify: `bindings/python/hpactor/client/gateway.py`
- Modify: `bindings/python/hpactor/client/cli.py`
- Create: `bindings/python/tests/unit/client/test_events.py`
- Create: `bindings/python/tests/unit/client/test_bundle.py`
- Create: `bindings/python/tests/support/client_contract.py`
- Create: `bindings/python/tests/unit/client/test_sync_async_contract.py`

**Interfaces:**
- Consumes: all four sync/async clients, `HPActorClientConfig`, `Capability`, transport ownership, `UnsupportedCapability`, `ClientClosedError`, and `EventLoopMismatchError`.
- Produces: `RequestEvent`, isolated event hooks, `HPActorClient`, `AsyncHPActorClient`, `capabilities`, typed missing-property behavior, shared HTTP transport pooling, deterministic close, and a parity contract used by later integration tests.

- [ ] **Step 1: Write failing capability, ownership, close, hook, and parity tests**

```python
class BundleTest(unittest.TestCase):
    def test_missing_capability_is_typed(self) -> None:
        config = HPActorClientConfig(health=health_config())
        client = HPActorClient(config)
        self.assertEqual(client.capabilities, frozenset({Capability.HEALTH}))
        with self.assertRaises(UnsupportedCapability) as caught:
            _ = client.cli
        self.assertEqual(caught.exception.capability, Capability.CLI)

    def test_shared_owned_http_transport_closes_once(self) -> None:
        client, raw = bundle_with_shared_health_metrics_transport()
        client.close()
        client.close()
        self.assertEqual(raw.close_calls, 1)

    def test_hook_error_does_not_replace_result(self) -> None:
        client = health_client_with_hook(lambda event: (_ for _ in ()).throw(ValueError("hook")))
        self.assertEqual(client.liveness().state, HealthState.HEALTHY)
```

Add async close order, injected transport survival, use after close, first-I/O
event-loop binding, cross-loop rejection, redaction of authorization/cookies,
4 KiB diagnostic cap, and matching sync/async result/error field tests.

- [ ] **Step 2: Run bundle and parity tests to verify RED**

Run:

```bash
python3 -m unittest bindings.python.tests.unit.client.test_events -v
python3 -m unittest bindings.python.tests.unit.client.test_bundle -v
python3 -m unittest bindings.python.tests.unit.client.test_sync_async_contract -v
```

Expected: FAIL because event and bundle implementations are absent.

- [ ] **Step 3: Implement bounded redacted events**

`RequestEvent` contains capability, operation, redacted origin, attempt,
duration, request bytes, response bytes, and result category. It contains no
headers, body, CLI params, or CLI args. Invoke hooks after transport cleanup;
catch ordinary hook exceptions, log them through `logging.getLogger("hpactor.client")`,
and never catch cancellation or process-exit exceptions.

Add `event_hook: Callable[[RequestEvent], None] | None = None` as a keyword-only
constructor argument on each sync/async capability client and both bundles.
The bundle passes the same hook to clients it owns; hooks are runtime behavior,
not frozen endpoint configuration and not part of the HTTP pooling key.

- [ ] **Step 4: Implement bundle construction and lifecycle**

Build only configured capabilities. Pool HTTP transports only when endpoint
TLS, headers, certificate, timeouts, limits, redirects, retry, and ownership
keys are equal. Missing properties always raise
`UnsupportedCapability(capability)`.

Close in this order: block new operations, CLI, capability-owned unshared HTTP
adapters, then each shared owned HTTPX client once. Continue after close errors,
raise the first, and attach later messages using `Exception.add_note`.

- [ ] **Step 5: Complete the sync/async contract harness**

Define adapter callables for construct, invoke, close, and exception capture so
one case table runs against both API families. Cover every public operation,
model field, typed failure, ownership state, and close state; do not compare
implementation-private attributes.

- [ ] **Step 6: Run the complete pure-Python client unit suite**

Run: `python3 -m unittest discover -s bindings/python/tests/unit/client -p 'test_*.py' -v`

Expected: all client unit tests PASS with no resource warnings.

- [ ] **Step 7: Commit bundles and parity contracts**

```bash
git add bindings/python/hpactor/client bindings/python/tests/unit/client \
  bindings/python/tests/support/client_contract.py
git commit -m "feat(python): add external client bundles"
```

### Task 9: Prove behavior against real HPActor health, metrics, gateway, and CLI servers

**Files:**
- Create: `tests/integration/python/python_external_sdk_fixture.cpp`
- Modify: `tests/integration/python/CMakeLists.txt`
- Create: `bindings/python/tests/integration/client/test_runtime_interop.py`
- Modify: `bindings/python/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: real `HealthHttpServer`, `HealthState`, `MetricsActor`, `HTTPGatewayActor`, `CliProtoServerActor`, temporary UDS paths, loopback TCP, and every public client from Tasks 3 through 8.
- Produces: one fixture process that prints a single JSON endpoint manifest, accepts deterministic state-control commands over stdin, and shuts down on `stop`; plus end-to-end sync/async interop evidence.

- [ ] **Step 1: Write the failing Python interop suite against the named fixture binary**

```python
class RuntimeInteropTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = ExternalSdkFixture.start(
            binary=os.environ["HPACTOR_EXTERNAL_SDK_FIXTURE"])

    def test_real_health_metrics_gateway_and_uds_cli(self) -> None:
        endpoints = self.fixture.endpoints
        with HealthClient(health_config(endpoints.health)) as health:
            self.assertEqual(health.liveness().state, HealthState.HEALTHY)
        with MetricsClient(metrics_config(endpoints.metrics)) as metrics:
            snapshot = metrics.scrape()
            self.assertIn("hpactor_metrics_events_lost_total", snapshot.text)
        with GatewayClient(gateway_config(endpoints.gateway)) as gateway:
            self.assertEqual(gateway.post("/echo", content=b"phase2").content,
                             b"phase2")
        with CliClient(CliClientConfig(uds_path=endpoints.cli_uds)) as cli:
            self.assertGreaterEqual(cli.system_stats().worker_count, 1)
```

Add async equivalents, degraded/unhealthy control, gateway 404/429, explicit
metrics route, every structured CLI RPC, loopback TCP parity, malformed fixture
endpoint rejection, and disconnect-after-mutating-command no-replay evidence.

- [ ] **Step 2: Configure and run the test to verify RED**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build python_external_sdk_fixture _hpactor
HPACTOR_EXTERNAL_SDK_FIXTURE="$PWD/build/tests/integration/python/python_external_sdk_fixture" \
  python3 -m unittest discover -s bindings/python/tests/integration/client -p 'test_*.py' -v
```

Expected: configure or build FAIL because the fixture target and Python suite do not exist.

- [ ] **Step 3: Implement the bounded real-runtime fixture**

Bind all TCP listeners to loopback and choose ephemeral ports. Place the UDS in
the test-provided temporary directory. Register an `/echo` application route
and an explicit `/metrics` route that returns the real `MetricsActor` snapshot.
Attach a mutable test `HealthState` to `HealthHttpServer`. Start both UDS and
loopback TCP `CliProtoServerActor` listeners.

After all listeners succeed, print and flush exactly one manifest:

```json
{"health":"http://127.0.0.1:41001","metrics":"http://127.0.0.1:41002","gateway":"http://127.0.0.1:41003","cli_uds":"/tmp/.../cli.sock","cli_host":"127.0.0.1","cli_port":41004}
```

Accept newline commands `health healthy`, `health degraded`, `health unhealthy`,
`gateway reject-next`, `cli disconnect-after-command`, and `stop`. Reject every
other control command and cap line length at 256 bytes.

- [ ] **Step 4: Register fixture and Python integration tests**

Add a CTest entry that sets the fixture path and `PYTHONPATH` to the built
package. Give the suite a 60-second timeout, serialize it to avoid UDS collisions,
and ensure the fixture is terminated and its UDS removed in every Python cleanup
path.

- [ ] **Step 5: Run focused real-runtime interoperability**

Run:

```bash
ninja -C build python_external_sdk_fixture _hpactor
ctest --test-dir build -R python_external_sdk_client_interop --output-on-failure
```

Expected: one interop test target PASS, covering sync/async HTTP, UDS/TCP CLI,
real protobuf replies, explicit metrics exposure, and no replay.

- [ ] **Step 6: Commit runtime interoperability**

```bash
git add tests/integration/python bindings/python/tests/integration/client \
  bindings/python/tests/CMakeLists.txt
git commit -m "test(python): prove external SDK interoperability"
```

### Task 10: Build and accept the universal client wheel beside native wheels

**Files:**
- Modify: `pyproject.toml`
- Create: `bindings/python/packaging/build_client_wheel.py`
- Modify: `bindings/python/packaging/verify_wheel.py`
- Create: `bindings/python/tests/packaging/test_client_wheel.py`
- Create: `bindings/python/tests/packaging/test_wheel_selection.py`
- Create: `bindings/python/tests/wheel/test_client_smoke.py`
- Modify: `bindings/python/tests/wheel/run_clean_smoke.py`
- Modify: `.github/workflows/python-wheels.yml`
- Modify: `.github/workflows/python-publish.yml`

**Interfaces:**
- Consumes: complete pure-Python package, checked-in generated protobufs, Phase 1D scikit-build-core metadata, four repaired native wheels, existing wheel verifier, and publish gates.
- Produces: reproducible `hpactor-<version>-py3-none-any.whl`, five-wheel acceptance, native-preference/universal-fallback resolver evidence, and one immutable five-wheel plus sdist release set.

- [ ] **Step 1: Write failing universal content, metadata, resolver, and release-set tests**

```python
class ClientWheelTest(unittest.TestCase):
    def test_universal_wheel_is_pure_and_complete(self) -> None:
        wheel = only_wheel(self.wheelhouse, "*-py3-none-any.whl")
        names = wheel_names(wheel)
        self.assertIn("hpactor/client/health.py", names)
        self.assertIn("hpactor/client/_proto/cli_pb2.py", names)
        self.assertFalse(any(name.endswith((".so", ".dylib", ".dll")) for name in names))
        self.assertEqual(wheel_tags(wheel), {"py3-none-any"})
        self.assertEqual(requires_dist(wheel), {
            "protobuf>=7.35.0,<8", "httpx>=0.28.1,<0.29",
        })

    def test_release_set_has_four_native_one_universal_one_sdist(self) -> None:
        self.assertReleaseSet(native_wheels=4, universal_wheels=1, sdists=1)
```

Resolver tests use `pip download --only-binary=:all: --no-index --find-links`
with target platform/implementation/ABI arguments. Assert Linux/macOS CPython
selects `cp311-abi3` and Windows/PyPy-compatible targets select `py3-none-any`.

- [ ] **Step 2: Run packaging tests to verify RED**

Run: `python3 -m unittest discover -s bindings/python/tests/packaging -p 'test_*wheel*.py' -v`

Expected: FAIL because the client wheel builder and five-wheel policy are absent.

- [ ] **Step 3: Implement a reproducible CMake-free client wheel build**

The wrapper removes only its own output directory, invokes the current Python,
and validates the filename before returning it:

```python
command = [
    sys.executable, "-m", "build", "--wheel",
    "-Cwheel.cmake=false",
    "-Cwheel.platlib=false",
    "-Cwheel.py-api=py3",
    "--outdir", str(output_dir),
]
subprocess.run(command, cwd=repo_root, check=True)
wheel = only_wheel(output_dir, "hpactor-*-py3-none-any.whl")
```

Set scikit-build-core's package inclusion to `bindings/python/hpactor`. Verify
generated protobufs before build. The native build keeps `wheel.cmake=true`,
`wheel.platlib=true`, and `wheel.py-api=cp311` through its existing CI settings.
Update project classifiers to retain CPython and add PyPy and OS-independent
client support without adding a Windows native-runtime classifier.

- [ ] **Step 4: Extend wheel verification and clean smoke**

Universal mode requires all public Python files, generated protobufs, `py.typed`,
license, purelib metadata, no native binary, and no bundled native library.
Native mode requires the same Python files plus `_hpactor` and the existing
binary closure. Both modes run import-quiescence, config, health MockTransport,
async health, HPAC codec, typing, and metadata smoke; native mode additionally
runs all Phase 1 actor smoke.

- [ ] **Step 5: Extend build and publish workflows**

Add a client-wheel job on Linux that builds once, verifies it, installs it in
clean CPython 3.11 and 3.14 environments, and uploads one artifact. Add a
non-native resolver/smoke job using Windows CPython and a PyPy 3.11 lane when
available. The acceptance job downloads all artifacts, rejects duplicate tags,
and requires exactly five wheels and one sdist.

The publish workflow consumes only the accepted artifact, checks all six
hashes, publishes with trusted publishing, verifies index installation on one
native lane and one universal-only lane, then creates the GitHub release.

- [ ] **Step 6: Run local package and resolver verification**

Run:

```bash
python3 bindings/python/packaging/build_client_wheel.py --output wheelhouse/client
python3 bindings/python/packaging/verify_wheel.py \
  --mode client --wheel wheelhouse/client/hpactor-*-py3-none-any.whl
python3 -m unittest bindings.python.tests.packaging.test_client_wheel -v
python3 -m unittest bindings.python.tests.packaging.test_wheel_selection -v
python3 bindings/python/tests/wheel/run_clean_smoke.py \
  --mode client --wheel wheelhouse/client/hpactor-*-py3-none-any.whl
```

Expected: universal tag/content, metadata, selection, clean install, sync/async
client smoke, and no-native lazy failure all PASS.

- [ ] **Step 7: Commit five-wheel packaging**

```bash
git add pyproject.toml bindings/python/packaging bindings/python/tests/packaging \
  bindings/python/tests/wheel .github/workflows/python-wheels.yml \
  .github/workflows/python-publish.yml
git commit -m "ci(python): publish universal client wheel"
```

### Task 11: Publish honest manual guidance and final Phase 2 acceptance evidence

**Files:**
- Create: `bindings/python/examples/external_client.py`
- Create: `docs/manual/python/external-client.rst`
- Modify: `docs/manual/python/index.rst`
- Modify: `docs/manual/python/installation.rst`
- Modify: `docs/manual/python/api.rst`
- Modify: `docs/manual/monitoring/health.rst`
- Modify: `docs/manual/monitoring/metrics.rst`
- Modify: `docs/manual/operations/cli-server.rst`
- Modify: `docs/manual/operations/http-gateway.rst`
- Modify: `docs/manual/limitations.rst`
- Create: `bindings/python/tests/docs/test_external_client_docs.py`
- Modify: `CLAUDE_MEMORY.md`
- Modify: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`
- Modify: `docs/superpowers/specs/2026-07-06-python-binding-phase2-external-sdk-design.md`

**Interfaces:**
- Consumes: all accepted clients, real-runtime fixture, five-wheel CI evidence, existing Sphinx manual, issue #426, and the Phase 2 design acceptance criteria.
- Produces: executable sync/async guidance, corrected endpoint claims, security/runbook guidance, API reference, and evidence-backed Phase 2 completion status.

- [ ] **Step 1: Write failing documentation contract and example tests**

```python
class ExternalClientDocsTest(unittest.TestCase):
    def test_manual_contains_required_boundaries(self) -> None:
        text = Path("docs/manual/python/external-client.rst").read_text()
        for phrase in (
            "explicitly exposed metrics URL",
            "does not authenticate or encrypt CLI TCP",
            "allow_insecure_remote_tcp=True",
            "16 MiB",
            "commands are never retried",
        ):
            self.assertIn(phrase, text)

    def test_example_compiles_without_native_import(self) -> None:
        source = Path("bindings/python/examples/external_client.py").read_text()
        compile(source, "external_client.py", "exec")
        self.assertNotIn("_hpactor", source)
```

Add checks that health docs do not promise distinct path policies, metrics docs
do not promise automatic exposure, gateway docs distinguish application/admin
listeners, and limitations distinguish native from portable client support.

- [ ] **Step 2: Run documentation tests to verify RED**

Run:

```bash
python3 -m unittest bindings.python.tests.docs.test_external_client_docs -v
sphinx-build -W -b html docs/manual docs/manual/_build/html
```

Expected: test FAIL because the page/example are absent or claims remain; the
existing docs build may still pass before content is added.

- [ ] **Step 3: Write executable sync and async examples**

The example accepts `--base-url`, optional `--cli-socket`, and
`--async`. It constructs only explicitly supplied capabilities, uses context
managers, prints health state and the metrics byte count, and never prints
metrics bodies, credentials, or CLI arguments. A remote CLI host option requires
`--allow-insecure-remote-cli` and passes the exact config opt-in.

- [ ] **Step 4: Write the external client guide and correct adjacent pages**

Document installation, universal/native wheel selection, explicit server
prerequisites, sync/async bundle and individual use, health interpretation,
raw metrics text, gateway status handling, CLI UDS/TCP, structured RPCs,
timeouts, limits, retries, cancellation, close, errors, redaction, and
troubleshooting. State that current `HealthHttpServer` uses one handler and
that metrics require a proven route/listener. Recommend UDS or an authenticated
encrypted proxy for production operations.

- [ ] **Step 5: Run focused source, docs, type, package, and interop verification**

Run:

```bash
python3 -m unittest discover -s bindings/python/tests/unit/client -p 'test_*.py' -v
python3 -m unittest bindings.python.tests.docs.test_external_client_docs -v
python3 -m compileall -q bindings/python/hpactor bindings/python/examples
ninja -C build python_external_sdk_fixture _hpactor
sphinx-build -W -b html docs/manual docs/manual/_build/html
ctest --test-dir build -R python_external_sdk_client_interop --output-on-failure
python3 bindings/python/packaging/build_client_wheel.py --output wheelhouse/client
python3 bindings/python/tests/wheel/run_clean_smoke.py \
  --mode client --wheel wheelhouse/client/hpactor-*-py3-none-any.whl
git diff --check
```

Expected: all client unit/docs tests, bytecode compilation, targeted build,
real-runtime interop, docs, universal clean smoke, and whitespace checks PASS.

- [ ] **Step 6: Require remote five-wheel and native-regression evidence**

Do not mark Phase 2 implemented until `.github/workflows/python-wheels.yml`
records four repaired native wheel jobs, one universal wheel job, native Phase
1 smoke on all four wheels, client smoke on all five wheels, resolver selection,
CPython 3.11 through 3.14 compatibility, the non-native fallback lane, protobuf
regeneration, dependency audit, and one accepted six-artifact release set.

- [ ] **Step 7: Record evidence-backed status only after every gate is green**

Update memory and design status with exact workflow run URL, commit, wheel
filenames, hashes, test counts, supported interpreters/platforms, HTTPX and
protobuf versions, native audit versions, known server prerequisites, and
remaining remote-node/authentication limitations. Until the remote evidence
exists, write `Implementation in progress` rather than `Implemented`.

- [ ] **Step 8: Commit documentation and acceptance evidence**

```bash
git add bindings/python/examples/external_client.py bindings/python/tests/docs \
  docs/manual CLAUDE_MEMORY.md \
  docs/superpowers/specs/2026-07-03-python-language-binding-design.md \
  docs/superpowers/specs/2026-07-06-python-binding-phase2-external-sdk-design.md
git commit -m "docs(python): publish external SDK guidance"
```

## Plan Completion Checklist

- [ ] Phase 1 package and wheel prerequisites exist before Phase 2 execution.
- [ ] `hpactor.client` imports without `_hpactor` and with no runtime side effects.
- [ ] Python metadata pins protobuf and HTTPX to the verified ranges.
- [ ] All configuration is frozen, validated, bounded, and secret-redacted.
- [ ] Sync and async HTTP transports enforce one monotonic deadline and response limit.
- [ ] Retries are opt-in, bounded, and limited to explicitly idempotent HTTP requests.
- [ ] Health clients preserve `OK` and JSON runtime responses without inventing path policy.
- [ ] Metrics clients preserve exposition text and represent ETag 304 distinctly.
- [ ] Gateway clients preserve non-2xx responses and reject origin escape by default.
- [ ] Generated CLI protobufs reproduce from the canonical `.proto` files.
- [ ] HPAC framing matches the C++ encoder byte for byte and rejects adversarial frames.
- [ ] CLI clients serialize callers, honor deadlines, invalidate failed streams, and never replay.
- [ ] Every advertised structured CLI RPC parses a real C++ reply.
- [ ] Bundles expose only configured capabilities and close owned transports once.
- [ ] Sync and async clients pass one shared behavioral contract suite.
- [ ] Real HPActor health, explicit metrics, gateway, UDS CLI, and loopback TCP tests pass.
- [ ] Non-loopback unauthenticated CLI TCP requires the explicit insecure opt-in.
- [ ] Four native wheels, one universal wheel, and one sdist pass acceptance.
- [ ] Universal fallback and native preference are proven by resolver tests.
- [ ] Native Phase 1 actor smoke remains green after lazy imports and packaging changes.
- [ ] Manual pages state actual endpoint prerequisites and security limitations.
- [ ] No implemented status is recorded before local and remote acceptance evidence exists.

## Execution Handoff

This plan is complete only as a planning artifact. Implementation has not begun.
Execute tasks in order because later tasks consume exact public contracts and
test seams established earlier.

Two supported execution modes are:

1. **Subagent-Driven (recommended):** use `superpowers:subagent-driven-development`, dispatch one fresh worker per task, and perform specification then code-quality review between tasks.
2. **Inline Execution:** use `superpowers:executing-plans`, run tasks in small batches, and stop at review checkpoints.
