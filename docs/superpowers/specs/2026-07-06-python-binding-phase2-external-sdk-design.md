<!--
Copyright 2026 HPActor Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Python Binding Phase 2 External SDK Design

**Status:** Approved design

**Date:** 2026-07-06 (updated 2026-07-10 for pybind11 backend and current status)

**Target:** Python 3.11+ portable clients, with native HPActor runtime support
remaining on the Phase 1 CPython/Linux/macOS matrix. The native `_hpactor`
module now uses the pybind11 backend (see [pybind11 backend design]
(2026-07-10-pybind11-backend-design.md)). The lazy native import via
`__getattr__` on `hpactor` resolves to the pybind11-built module.

## 1. Summary

Phase 2 adds a pure-Python external SDK under `hpactor.client`. It lets Python
programs consume HPActor health, metrics, application HTTP gateway, and CLI
surfaces without embedding the C++ runtime or importing `hpactor._hpactor`.

The SDK has equivalent synchronous and asynchronous clients. Four independent
typed clients sit behind an optional capability bundle:

- `HealthClient` and `AsyncHealthClient`;
- `MetricsClient` and `AsyncMetricsClient`;
- `GatewayClient` and `AsyncGatewayClient`;
- `CliClient` and `AsyncCliClient`;
- `HPActorClient` and `AsyncHPActorClient` as thin configured bundles.

HTTP clients use `httpx`. CLI clients use the implemented protobuf protocol
over Unix-domain sockets or TCP: the literal `HPAC`, a big-endian 32-bit
payload length, and one serialized `CliCommand` or `CliResponse`.

The package release gains one `py3-none-any` client-only wheel alongside the
four native ABI3 wheels. Importing `hpactor.client` never imports the native
extension. The universal wheel therefore provides the external SDK on systems
where the native runtime is unavailable.

Phase 2 does not make Python a native remote actor node. It also does not
pretend that an endpoint exists merely because a manual or configuration field
mentions it. Capabilities are explicit, and documentation states the server
configuration required for each client.

## 2. Current Runtime Evidence

The design follows implemented contracts rather than treating every manual
statement as proof of runtime behavior.

### 2.1 Health HTTP server

`HealthHttpServer` currently installs one request handler for its listener. The
handler does not inspect the request path. It produces:

- `200 OK` with body `OK` for a healthy state or when no `HealthState` is
  attached;
- `200 OK` with JSON for a degraded state;
- `503 Service Unavailable` with JSON for an unhealthy state.

The JSON contains an overall `status` and an array of checks with `name`,
`status`, and `reason`. The SDK may call configurable liveness and readiness
paths, defaulting to `/healthz` and `/readyz`, but it must not infer that the
current server evaluates those paths differently.

### 2.2 Metrics

`MetricsActor` implements a `MetricsRequest` handler that drains its event
ring, snapshots the registry, and emits OpenMetrics text. A `metrics_path`
configuration field exists. The reviewed implementation does not prove that
every process automatically publishes that text on an HTTP listener.

`MetricsClient` therefore consumes an explicitly configured HTTP URL. The
manual must show how the application or operations listener exposes the
metrics actor output. The SDK must not advertise automatic `/metrics`
availability without end-to-end runtime evidence.

### 2.3 Application HTTP gateway

`HTTPGatewayActor` exposes application-registered routes. It separates the
query from the path, matches the route registry, converts the request into a
typed actor message, tracks a pending reply, returns `404` for unknown routes,
and can return `429` when delivery is rejected. The SDK treats this as general
HTTP and does not invent actor-specific semantics above it.

### 2.4 CLI protobuf server

`CliProtoServerActor` listens on UDS and optionally TCP. A request body is a
serialized `hpactor.cli.CliCommand`; a response body is a serialized
`hpactor.cli.CliResponse`. Bodies are carried in an eight-byte HPAC frame:

```text
bytes 0..3   ASCII "HPAC"
bytes 4..7   unsigned payload length in network byte order
bytes 8..N   protobuf payload
```

`CliCommand.rpc_method` takes priority over command-tree `path`. Implemented
structured methods include inspection, kill, quarantine, enumeration, system
statistics, and memory statistics. Command-tree execution produces text or
JSON according to the requested format.

The current CLI TCP protocol has neither authentication nor TLS. Phase 2 must
keep that boundary visible in construction, documentation, and tests.

## 3. Goals

1. Provide an idiomatic external SDK that does not require the native module.
2. Offer matching sync and async APIs with shared values and failures.
3. Preserve HTTP response semantics for application gateway routes.
4. Interoperate exactly with the implemented HPAC CLI framing and protobufs.
5. Bound response bodies, frames, timeouts, connection pools, and retries.
6. Make configured and unsupported capabilities explicit.
7. Default to secure HTTP behavior and make insecure CLI TCP difficult to use
   accidentally.
8. Keep the SDK useful as individual clients without requiring the aggregate
   bundle.
9. Package one universal client wheel without weakening the four native wheel
   guarantees.
10. Document gaps between configured surfaces, manual claims, and proven
    runtime behavior.

## 4. Non-Goals

Phase 2 does not:

- participate as an HPActor transport node;
- send actor wire frames, resolve actor identities, or allocate `TypeTag`
  values over the network;
- add node authentication, authorization, protocol negotiation, or feature
  negotiation;
- add TLS or authentication to the CLI server;
- expose an unauthenticated remote CLI by default;
- implement a partial Prometheus/OpenMetrics query or exposition parser;
- convert arbitrary gateway payloads into actor messages on the client;
- synthesize health/readiness distinctions absent from the server response;
- probe mutating endpoints to discover capabilities;
- retry mutating operations implicitly;
- replace `httpx` with a project-owned HTTP implementation;
- promise that a configured metrics path is listening without integration
  evidence;
- extend native runtime support to Windows or non-CPython interpreters.

## 5. Architectural Decisions

### 5.1 Composable clients and a thin bundle

Each capability is a standalone client with one responsibility and one
transport family. The bundle only validates configuration, constructs the
selected clients, exposes them as properties, and coordinates close.

This avoids a monolith in which HTTP pooling, CLI serialization, security, and
lifecycle are coupled. It also preserves the umbrella design's capability
discovery requirement: the bundle knows which capabilities were configured,
while an individual client is an explicit capability by construction.

### 5.2 Async-first parity, not async emulation

The public API is designed async-first, but synchronous clients are first-class
operational tools. Sync clients use `httpx.Client` and blocking sockets. Async
clients use `httpx.AsyncClient` and asyncio streams. Async methods do not call
sync methods through a worker thread.

Both forms share immutable configuration, response models, generated
protobufs, frame validation rules, error taxonomy, and semantic tests.

### 5.3 Explicit capability configuration

`HPActorClientConfig` contains optional configuration for `health`, `metrics`,
`gateway`, and `cli`. A missing section means the capability is unsupported.
The bundle exposes `capabilities` as a frozen set of `Capability` enum values.

Accessing a missing property raises `UnsupportedCapability`. Construction does
not contact the network, and ordinary construction performs no speculative
endpoint probing.

### 5.4 Transport isolation

The package has three internal seams:

- an HTTP adapter around injected or owned `httpx` clients;
- a CLI stream adapter for blocking and asyncio UDS/TCP streams;
- a pure frame codec that has no socket or event-loop dependency.

The typed clients depend on those narrow seams. Tests can replace a seam
without replacing public clients, and HTTPX or socket details do not leak into
health, metrics, or CLI result models.

`GatewayClient` is the intentional exception: it accepts HTTPX request options
and returns `httpx.Response` because its job is to preserve general HTTP rather
than reduce it to an HPActor-specific subset.

## 6. Package Layout and Imports

The public package is organized conceptually as:

```text
hpactor/
  __init__.py
  client/
    __init__.py
    config.py
    errors.py
    models.py
    health.py
    metrics.py
    gateway.py
    cli.py
    bundle.py
    _http.py
    _hpac.py
    _sync_stream.py
    _async_stream.py
    _proto/
      cli_pb2.py
      cli_messages_pb2.py
```

Names beginning with `_` are not public compatibility surfaces. Generated
protobuf classes used by typed CLI methods are re-exported from
`hpactor.client.cli`, so users do not import package-private generated modules.

`hpactor.__init__` must not eagerly import `hpactor._hpactor`. Pure-Python
values and `hpactor.client` remain importable in the universal wheel. Accessing
a native-only export when `_hpactor` is absent raises
`NativeBindingUnavailable` with the current platform, interpreter, and the
supported native matrix. Import itself has no network, thread, socket, or
event-loop side effect.

## 7. Public Configuration

Configuration types are frozen dataclasses. They validate at construction and
never contain live transports.

### 7.1 HTTP endpoint configuration

`HttpEndpointConfig` contains:

- absolute `base_url`;
- optional default headers;
- TLS verification setting, default `True`;
- optional client certificate supported by HTTPX;
- `follow_redirects`, default `False`;
- `HttpTimeouts`;
- `HttpLimits`;
- optional `RetryPolicy`, default disabled.

`HttpTimeouts` defaults to 5 seconds for connect and pool acquisition and 30
seconds for read and write. All values must be positive and finite. A caller
may override them per request within the configured maximum total deadline.

`HttpLimits` defaults to 20 keep-alive connections, 100 total connections, and
a 16 MiB response body. A response is consumed with streaming reads so the
body limit is enforced before unbounded accumulation.

### 7.2 Health and metrics configuration

`HealthClientConfig` contains an `endpoint: HttpEndpointConfig` value and adds:

- `liveness_path`, default `/healthz`;
- `readiness_path`, default `/readyz`.

`MetricsClientConfig` contains an `endpoint: HttpEndpointConfig` value and adds:

- `metrics_path`, default `/metrics`;
- accepted content types for Prometheus/OpenMetrics text.

Paths must begin with `/`, must not contain credentials or fragments, and are
resolved beneath the configured base URL without permitting a network-location
override.

### 7.3 CLI configuration

`CliClientConfig` contains exactly one endpoint after defaults are applied:

- `uds_path`, default `/tmp/hpactor/hpactor.sock`; or
- `host` plus `port`.

It also contains:

- connect timeout, default 5 seconds;
- request timeout, default 10 seconds;
- maximum outbound payload, default 16 MiB;
- maximum inbound payload, default 16 MiB;
- default format, one of `pretty`, `json`, or `tabular`;
- `allow_insecure_remote_tcp`, default `False`.

UDS is the preferred default on POSIX. Selecting TCP requires setting
`uds_path=None`. The IP literals in `127.0.0.0/8`, the IPv6 literal `::1`, and
the exact hostname `localhost` are permitted without the remote opt-in. Every
other hostname or address is treated as remote and rejected unless
`allow_insecure_remote_tcp=True`; DNS is not used to weaken that decision.

On platforms without Unix-domain socket support, UDS construction raises a
typed configuration error and TCP remains available under the same security
rules.

### 7.4 Bundle configuration

`HPActorClientConfig` holds optional instances of the four capability configs.
At least one must be present. The same endpoint config may be reused by value,
but the bundle decides whether HTTP transports can be pooled together only
when their TLS, headers, timeout, limit, redirect, and ownership settings are
identical.

## 8. Public API

### 8.1 Bundle use

```python
from hpactor.client import (
    AsyncHPActorClient,
    HealthClientConfig,
    HPActorClientConfig,
    HttpEndpointConfig,
    MetricsClientConfig,
)

endpoint = HttpEndpointConfig(base_url="https://node.example")
config = HPActorClientConfig(
    health=HealthClientConfig(endpoint=endpoint),
    metrics=MetricsClientConfig(endpoint=endpoint),
)

async with AsyncHPActorClient(config) as client:
    readiness = await client.health.readiness()
    metrics = await client.metrics.scrape()
```

`client.cli` in this example raises `UnsupportedCapability`. The object never
returns `None` for a missing configured capability, so a configuration mistake
cannot become a later attribute error.

### 8.2 Individual use

Each client accepts its capability config directly and supports explicit close
and context-manager use. It may also accept a compatible injected HTTPX client
or CLI stream factory for advanced applications and tests.

An injected transport remains owned by the injector. A client-created
transport is owned and closed by that client. Ownership is fixed at
construction and exposed as a read-only diagnostic value.

### 8.3 Sync and async naming

Synchronous classes use the unprefixed name. Asynchronous classes use the
`Async` prefix. Method names and non-await return models otherwise match. The
SDK does not create hidden event loops, call `asyncio.run()`, or use thread
offloading to implement an async method.

## 9. Health Client

`liveness()` and `readiness()` issue bounded GET requests to their configured
paths. Both return `HealthResult` with:

- requested `HealthProbe` (`LIVENESS` or `READINESS`);
- `HealthState` (`HEALTHY`, `DEGRADED`, `UNHEALTHY`, or `UNKNOWN`);
- HTTP status;
- tuple of `HealthCheck` values;
- response content type;
- raw body bytes;
- elapsed duration.

Interpretation is conservative:

1. Body `OK` with a 2xx status is `HEALTHY`.
2. Valid health JSON uses its overall status and checks.
3. `503` with valid unhealthy JSON is `UNHEALTHY`.
4. A syntactically valid but unknown status is `UNKNOWN`.
5. Other non-2xx responses raise `HttpResponseError` rather than being called
   a health state.
6. Malformed JSON advertised as JSON raises `ProtocolError`.

An unhealthy or degraded result is data, not a transport exception.
`require_live()` and `require_ready()` return the result on success and raise
`HealthCheckFailed` otherwise.

Because the current server uses one handler, the `probe` field records which
path the caller requested; it is not evidence that the server evaluated a
distinct liveness or readiness policy.

## 10. Metrics Client

`scrape()` issues a bounded GET and returns `MetricsSnapshot` with raw text,
content type, HTTP status, collection time, elapsed duration, and an optional
ETag. It accepts the Prometheus text and OpenMetrics text media types, including
parameters.

The SDK validates HTTP success, media type, response size, and UTF-8 decoding.
It does not parse metric families or offer local query semantics. Preserving
the exact exposition text avoids an incomplete parser silently dropping
labels, exemplars, timestamps, or future syntax.

Conditional requests may use a caller-supplied ETag. A `304` result is returned
as `MetricsNotModified`; it is not converted into an empty snapshot.

## 11. Gateway Client

`request(method, path, **httpx_options)` sends a request beneath the configured
base URL and returns an `httpx.Response`. The response body is fully consumed
and bounded before return, so the response does not retain an open network
stream. Convenience methods `get`, `post`, `put`, `patch`, and `delete`
delegate to `request` without changing semantics.

The path resolver rejects absolute URLs unless `allow_absolute_url=True` is an
explicit per-call option. The default prevents a value intended as an
application path from bypassing the configured host and TLS policy.

Non-2xx responses, including the gateway's `404` and `429`, are returned to the
caller. `request_checked()` is available for callers who want a typed
`HttpResponseError` on non-2xx responses. Response limits apply in either form.

Arbitrary gateway requests are never automatically retried. A caller may opt
into the configured idempotent retry policy for GET or HEAD only. Retrying any
other method requires an explicit per-request idempotency declaration.

## 12. CLI Client

### 12.1 Command-tree execution

`execute(path, *, params=None, args=(), format=None)` builds `CliCommand` and
returns `CliResult` with content type, payload bytes, decoded text when
applicable, structured flag, and remote error metadata.

The path is slash-separated and must not begin with `/`. Parameters are copied
into the protobuf map and arguments preserve order. Client input is never
assembled into a shell command.

When `CliResponse.is_error` is true, `execute` raises `CliCommandError` carrying
the error code, content type, and bounded payload. It never discards the server
diagnostic body.

### 12.2 Structured methods

Typed methods serialize the implemented request protobuf and parse the exact
reply protobuf:

- `inspect` -> `InspectStateReply`;
- `kill` -> `KillReply`;
- `quarantine` and `unquarantine` -> `QuarantineReply`;
- `list_actors` -> `ListActorsReply`;
- `system_stats` -> `SystemStatsReply`;
- `memory_stats` -> `MemoryStatsReply`.

The lower-level `call(rpc_method, request, response_type)` is public for
forward-compatible implemented RPCs. It requires a protobuf request instance
and response class, validates `is_structured`, and raises `ProtocolError` when
the payload does not parse as the requested reply.

The SDK does not claim an arbitrary RPC name is supported. Server method-not-
found responses remain `CliCommandError` with their remote error code.

### 12.3 Framing

The frame encoder rejects payloads above the outbound bound before allocation.
The decoder reads exactly eight header bytes, compares all four magic bytes,
decodes an unsigned network-order length, validates it against the inbound
bound, and reads exactly that payload. EOF before either exact read is a
`ProtocolError` and invalidates the connection.

Only one request may be in flight on a CLI connection. Concurrent callers are
serialized by a sync or async lock because the current protocol has no request
correlation field. The lock wait is included in the request deadline.

### 12.4 Connection behavior

CLI clients connect lazily on the first request or explicitly through
`connect()`. A healthy connection is reused. A connection is discarded after:

- timeout after any request byte was written;
- cancellation while a request is in flight;
- invalid magic, invalid length, truncated payload, or invalid protobuf;
- remote EOF or socket error;
- close.

The next explicit request may establish a new connection. The SDK does not
replay the failed command, because the server may have executed it before the
connection failed.

## 13. Lifecycle and Concurrency

All clients support idempotent `close()`. Async clients support
`await aclose()`. Sync and async context managers close only owned transports.
Use after close raises `ClientClosedError`.

Async client instances bind to the running event loop on first I/O. Use from a
different loop raises `EventLoopMismatchError`. Construction remains loop-free.

Closing while work is active causes new operations to fail immediately and
allows active HTTP operations to finish until their deadlines. An in-flight
CLI operation is cancelled by closing the stream; it completes with
`ClientClosedError` unless caller cancellation was the initiating event.

Bundle close is deterministic: prevent new bundle access, close CLI, close
capability-owned HTTP clients, then close a shared owned HTTP transport once.
Failure to close one capability does not skip the remaining closes; close
reports the first failure and chains the rest as diagnostic notes.

## 14. Bounds, Deadlines, and Retries

Every operation uses a monotonic deadline. DNS, connect, pool acquisition,
lock wait, write, response headers, and body/frame read are charged to that
operation. Phase timeouts may be smaller, but none may extend the total
deadline.

All response paths enforce the configured 16 MiB default before accumulating
the whole response. Tests cover declared and streaming bodies that cross the
limit. Diagnostic payloads stored in exceptions are truncated to 4 KiB with an
explicit truncation flag.

`RetryPolicy` is disabled by default. When enabled it has a maximum of three
attempts, exponential backoff starting at 100 ms, a 2-second cap, and bounded
jitter. It may retry connection failures and configured transient HTTP status
codes only for an idempotent request. `Retry-After` may extend neither the
total deadline nor the configured attempt cap.

CLI operations are never retried. Gateway POST, PUT, PATCH, and DELETE are not
idempotent unless the caller explicitly declares the operation idempotent.

## 15. Error Model

All SDK-defined failures derive from `HPActorClientError`:

```text
HPActorClientError
  ConfigurationError
    InsecureTransportError
  UnsupportedCapability
  NativeBindingUnavailable
  ClientClosedError
  EventLoopMismatchError
  TransportError
    ConnectionError
    OperationTimeout
  ResponseLimitError
  ProtocolError
  HttpResponseError
  HealthCheckFailed
  CliCommandError
```

Exceptions contain stable machine-readable fields and bounded diagnostics.
They never include authorization headers, cookies, client keys, full URLs with
userinfo, or unbounded response bodies. HTTPX and socket exceptions are
retained as causes but are not the public error contract.

Cancellation remains the platform cancellation exception rather than being
wrapped as `HPActorClientError`. Cleanup still runs, and CLI cancellation
invalidates the connection.

## 16. Security Model

HTTP TLS verification is on by default. Redirect following is off by default,
which prevents credentials or trust assumptions from moving to another origin
silently. Explicit redirects remain subject to the response limit and total
deadline.

Configuration representations redact sensitive headers and certificate key
paths. Structured logs expose capability, operation, origin without userinfo,
duration, attempt, result category, and byte counts. They do not expose bodies
or CLI arguments by default.

UDS access relies on filesystem ownership and permissions configured by the
server. CLI TCP is plaintext and unauthenticated. Loopback TCP is intended for
local development and controlled hosts. Non-loopback TCP requires the explicit
`allow_insecure_remote_tcp` opt-in and emits a once-per-client warning. The
manual recommends UDS or an authenticated, encrypted proxy and does not call
raw TCP production-safe.

The SDK does not add bearer-token fields to CLI configuration because the
current server has no contract that could validate them.

## 17. Runtime Compatibility and Honest Capability Claims

Capabilities are supported at two different levels:

1. The Python client implementation supports the protocol.
2. A particular HPActor deployment has explicitly exposed the corresponding
   server surface.

The SDK proves the first and configuration declares the second. It does not
probe all possible ports, infer a metrics listener from `metrics_path`, or
equate an application gateway with the CLI admin HTTP server.

Documentation contains a server-side checklist for each client:

- health listener and attached health state;
- exact metrics exposure URL;
- application gateway listener and registered routes;
- CLI protobuf server UDS or trusted TCP listener.

If a future runtime adds a negotiated capability endpoint, the bundle may gain
an explicit discovery operation. That is outside this phase.

## 18. Packaging and Release

Phase 2 changes the release set from four wheels plus one sdist to five wheels
plus one sdist:

- four existing CPython 3.11 ABI3 native wheels for Linux x86_64, Linux ARM64,
  macOS x86_64, and macOS ARM64;
- one `py3-none-any` wheel containing the pure-Python package, generated CLI
  protobuf modules, and external SDK without `_hpactor`;
- one source distribution.

Native and universal wheels share the same project name, version, public
Python files, generated protobufs, dependency bounds, and metadata. Native
wheels additionally contain `_hpactor` (pybind11-based) and native runtime
assets. The pybind11 headers are vendored at build time under
`third_party/pybind11/include/`. `Py_LIMITED_API` is deferred; ABI3 compliance
to be verified by `abi3audit` as a Phase 1D follow-up. Required Python
dependencies include the verified protobuf range and a bounded HTTPX range.

Resolver tests prove:

- supported CPython/Linux/macOS environments prefer the matching native wheel;
- environments without a compatible native wheel select the universal wheel;
- the universal wheel imports `hpactor` and `hpactor.client` without attempting
  to load `_hpactor`;
- accessing a native-only API from the universal wheel raises the typed native
  availability error;
- client behavior and public typing are identical between wheel forms.

The portable SDK supports Python 3.11+ wherever its pure-Python dependencies
and selected transports are supported. Native runtime support remains limited
to the Phase 1 matrix. Windows can use HTTP and explicitly configured CLI TCP;
it receives no native runtime support, and UDS is available only when the host
Python/socket platform supports it.

Release evidence records all five wheel hashes, the sdist hash, dependency
audit, resolver selection results, clean-install tests, and native wheel audit.
The Phase 1D documentation and CI assertions that require exactly four wheels
must be updated atomically with the Phase 2 release change.

## 19. Testing Strategy

### 19.1 Shared contract tests

One parameterized contract suite runs against sync and async clients. It covers
configuration, result equality, errors, ownership, close, timeouts, bounds,
retry eligibility, and redaction. This prevents the two APIs from drifting.

### 19.2 HTTP unit tests

HTTPX mock transports cover:

- healthy `OK`, degraded JSON, unhealthy JSON, unknown statuses, and malformed
  health bodies;
- OpenMetrics and Prometheus content types, ETag/304, invalid UTF-8, and
  oversized streaming responses;
- gateway methods, query strings, request bodies, headers, non-2xx responses,
  absolute-URL rejection, redirect policy, and idempotency declarations;
- connect, pool, write, read, and total deadlines;
- retry attempt caps and cancellation;
- injected versus owned transport close.

### 19.3 CLI codec and adversarial tests

Pure codec tests compare exact bytes with C++ `encode_as_frame`. They cover
zero-length protobuf payloads, boundary lengths, invalid magic, truncated
headers, oversized declared lengths, truncated payloads, trailing next-frame
bytes, malformed protobuf, and diagnostic truncation.

Scripted sync and asyncio servers cover delayed reads, delayed writes, EOF at
every byte boundary, disconnect after command receipt, cancellation, lock
deadline, concurrent callers, reconnect after failure, and close during I/O.

Property and fuzz-oriented tests assert that arbitrary input either produces
one bounded frame or a typed error without unbounded allocation or hang.

### 19.4 C++ interoperability tests

Integration fixtures start real HPActor components and prove:

- health `OK`, degraded, and unhealthy responses are interpreted correctly;
- a deliberately exposed metrics route preserves the real `MetricsActor`
  OpenMetrics output;
- registered `HTTPGatewayActor` routes preserve method, query, body, status,
  and delivery rejection behavior;
- sync and async CLI clients execute command-tree requests over UDS;
- loopback TCP CLI matches UDS framing;
- every supported structured CLI method parses the C++ reply protobuf;
- mutating command disconnects are not replayed.

The metrics fixture is described as an explicitly exposed route, not proof of
an automatic listener.

### 19.5 Packaging tests

Clean environments test:

- the universal wheel without any checkout or native libraries;
- all four repaired native wheels;
- CPython 3.11 through 3.14 compatibility;
- at least one non-native platform resolver lane for universal-wheel
  selection;
- import side effects, lazy native failure, public typing, sync HTTP, async
  HTTP, and CLI codec smoke;
- source distribution contents and reproducible protobuf generation.

Native wheel tests continue to run Phase 1 actor smoke in addition to Phase 2
client smoke. A universal wheel passing does not substitute for native wheel
acceptance.

## 20. Observability and Documentation

Clients expose opt-in standard-library logging and request event hooks. Metrics
integration is application-owned; the SDK does not start a metrics server.
Hooks receive bounded value objects and must not receive secrets or response
bodies. Hook failure is isolated from transport cleanup and is reported through
logging rather than replacing the operation result.

The manual gains:

- installation and native-versus-client wheel selection;
- sync and async quick starts;
- server configuration prerequisites;
- health response interpretation;
- raw OpenMetrics retrieval;
- gateway response handling;
- CLI UDS and explicit insecure TCP examples;
- timeouts, limits, retries, cancellation, and close;
- capability and error reference;
- security and troubleshooting guidance.

Existing health, metrics, gateway, and CLI pages must be corrected where they
state behavior not proven by current code. Backlog-only authentication,
negotiation, and automatic endpoint claims remain clearly labeled.

## 21. Acceptance Criteria

Phase 2 is complete only when all of the following are proven:

1. `hpactor.client` imports and works without `_hpactor`.
2. Sync and async public clients pass the same behavioral contract suite.
3. Missing bundle capabilities raise `UnsupportedCapability` without network
   probing.
4. Health parsing covers current `OK` and JSON responses without inventing a
   liveness/readiness distinction.
5. Metrics retrieval preserves real OpenMetrics text from an explicitly
   exposed runtime route.
6. Gateway requests preserve general HTTP semantics and enforce response
   bounds.
7. CLI framing is byte-for-byte compatible with the C++ HPAC implementation
   over UDS and loopback TCP.
8. Every advertised structured CLI method interoperates with the C++ server.
9. CLI cancellation, timeout, protocol failure, and disconnect invalidate the
   stream and never replay a command.
10. No response, frame, retry sequence, pool, or stored diagnostic exceeds its
    configured bound.
11. TLS verification, redirect policy, secret redaction, and non-loopback CLI
    TCP opt-in pass security tests.
12. Client and bundle close are idempotent and respect injected transport
    ownership.
13. Four native wheels, one universal wheel, and one sdist pass clean install,
    resolver, metadata, dependency, and behavior checks.
14. Native wheel Phase 1 actor smoke remains green after lazy import changes.
15. The manual distinguishes client protocol support from deployment endpoint
    availability and repeats the unauthenticated CLI TCP warning.

## 22. Alternatives Rejected

### 22.1 Bundle only

A single mandatory client object would make simple health scripts configure
unrelated capabilities and would couple independent transports. The bundle is
kept as a convenience above independently usable clients.

### 22.2 Independent clients without a bundle

This is smaller but loses the umbrella design's explicit capability set and a
single lifecycle boundary for applications that use several surfaces.

### 22.3 One monolithic transport client

HTTP and CLI have different framing, security, concurrency, and retry
contracts. Combining them would leak conditionals through every operation and
make safe close difficult to reason about.

### 22.4 Standard-library HTTP only

Blocking `http.client` cannot implement an async-first SDK without threads, and
a project-owned asynchronous HTTP parser would add substantial protocol and
security risk. HTTPX provides maintained sync and async transports while the
SDK retains its own semantic bounds.

### 22.5 Async-only API

Health checks, metrics collection, and operational CLI scripts are frequently
synchronous. A first-class sync API is justified, but it must share contracts
instead of wrapping async calls in hidden event loops.

### 22.6 Clients only in native wheels

This would make a pure-Python SDK unavailable on precisely the systems where a
remote client is most useful. The universal client wheel preserves one project
name and one API.

### 22.7 Separate `hpactor-client` distribution

A second distribution would split release versions and either change imports
or require two distributions to own one top-level package. A universal wheel
under the existing project avoids that ambiguity.

### 22.8 Automatic capability probing

Probe results can be false behind proxies, may touch sensitive admin routes,
and cannot prove that similarly named endpoints implement HPActor semantics.
Explicit configuration is deterministic and auditable.

### 22.9 Automatic CLI retry

The server may execute a command before the response connection fails. Without
request IDs and deduplication, replay can duplicate destructive operations.

## 23. Delivery Boundary

This specification is the design gate for Phase 2. It includes the pure-Python
SDK, generated CLI protobuf packaging, lazy native imports, universal wheel,
interop tests, documentation corrections, and release evidence.

Implementation requires a separate detailed plan. Native remote-node
participation, authenticated CLI, protocol negotiation, automatic capability
discovery, and an automatic metrics listener remain later designs.
