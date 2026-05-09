# Registrar Star Topology Example - Design Spec

## Overview

Add a complex example that demonstrates how HPActor's embedded registrar works in
a single-server, multiple-process deployment.

The example shows one process winning the registrar port bind and becoming the
authoritative registrar server. Later processes fail that bind, become registrar
clients, connect to the server over TCP, register their actor-system endpoint,
receive membership updates, and keep their membership alive with heartbeats. A
separate query mode sends a UDP `ResolveQuery` to the registrar and prints the
`ResolveResponse`.

This is a registrar-control-plane example. It focuses on membership and endpoint
resolution, not application-level actor message exchange after resolution.

## Current Code Reality

`ActorSystem` creates a `UdpRegistrar` when network is enabled and
`config.registrar.udp_port > 0`.

Default registrar ports come from `RegistrarConfig`:

```cpp
struct RegistrarConfig {
    uint16_t udp_port = 5353;
    uint16_t tcp_port = 5353;
    ...
};
```

These registrar ports are distinct from `Config::tcp_port`, which is the actor
transport listening port:

```cpp
struct Config {
    EndPoint endpoint = LocalEndpoint;
    bool enable_network = false;
    uint16_t tcp_port = 0; // actor transport port
    net::RegistrarConfig registrar = {};
    ...
};
```

The example must keep those roles separate:

| Field | Meaning | Default in code | Example value |
| --- | --- | --- | --- |
| `Config::endpoint` | actor-system identity | `LocalEndpoint` | `127.0.0.1:17001` |
| `Config::tcp_port` | actor transport listen port | `0` | `17001` |
| `RegistrarConfig::tcp_port` | registrar TCP control plane | `5353` | `5353` or override |
| `RegistrarConfig::udp_port` | registrar UDP query plane | `5353` | `5353` or override |

The runnable example should default to HPActor's registrar defaults (`5353`) but
accept `--registrar-port <port>` because port 5353 may already be owned by mDNS
or other local services.

## Example

Create `examples/12_registrar_star_topology.cpp`.

The binary has three modes:

```bash
./examples/12_registrar_star_topology --server \
  --actor-port 17000 \
  --registrar-port 5353

./examples/12_registrar_star_topology --worker worker-a \
  --actor-port 17001 \
  --registrar-host 127.0.0.1 \
  --registrar-port 5353

./examples/12_registrar_star_topology --query \
  --registrar-host 127.0.0.1 \
  --registrar-port 5353 \
  --target 127.0.0.1:17001
```

For local machines where `5353` is unavailable, all commands can pass the same
override, for example `--registrar-port 19053`.

## Topology

```text
Single host: 127.0.0.1

Process 1: registrar server
  ActorSystem endpoint: 127.0.0.1:17000
  Actor transport TCP: 17000
  Registrar TCP:       5353
  Registrar UDP:       5353

Process 2: worker-a
  ActorSystem endpoint: 127.0.0.1:17001
  Actor transport TCP: 17001
  Registrar TCP:       5353
  Registrar UDP:       5353

Process 3: worker-b
  ActorSystem endpoint: 127.0.0.1:17002
  Actor transport TCP: 17002
  Registrar TCP:       5353
  Registrar UDP:       5353

Process 4: query probe
  Sends UDP ResolveQuery to 127.0.0.1:5353.
```

The first process can bind registrar TCP port `5353`, so it enters server mode.
Workers use the same registrar port. Their registrar bind fails with
`EADDRINUSE`, so `UdpRegistrar::start()` enters client mode.

## Server Mode Flow

Server mode starts with:

```cpp
hpactor::Config config;
config.enable_network = true;
config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:17000");
config.tcp_port = 17000;
config.registrar.tcp_port = registrar_port;
config.registrar.udp_port = registrar_port;

hpactor::ActorSystem system(config);
```

During `ActorSystem` construction:

1. `ActorSystem` creates `net::EventLoop`.
2. `ActorSystem` creates `net::UdpRegistrar`.
3. `UdpRegistrar::start()` attempts to bind registrar TCP port.
4. Bind succeeds for the first process.
5. `UdpRegistrar` creates `RegistrarServer`.
6. `RegistrarServer` listens on registrar TCP port.
7. `UdpRegistrar` opens the UDP resolver socket on registrar UDP port.
8. `RegistrarServer` owns the authoritative `NodeRegistry`.

The example should print:

```text
SERVER mode
  actor endpoint: 127.0.0.1:17000
  actor tcp:      17000
  registrar tcp:  5353
  registrar udp:  5353
```

## Client Mode Flow

Worker mode starts with:

```cpp
hpactor::Config config;
config.enable_network = true;
config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:17001");
config.tcp_port = 17001;
config.registrar.tcp_port = registrar_port;
config.registrar.udp_port = registrar_port;
config.registrar.static_routes.push_back(
    hpactor::net::StaticRouteConfig{
        hpactor::endpoint_ops::parse_endpoint(
            "127.0.0.1:" + std::to_string(registrar_port)),
        registrar_host,
        registrar_port});

hpactor::ActorSystem system(config);
```

During `ActorSystem` construction:

1. `UdpRegistrar::start()` attempts to bind registrar TCP port.
2. Bind fails because the server process already owns the port.
3. `UdpRegistrar` creates a client-side `NodeRegistry`.
4. Static routes seed that registry with the registrar server endpoint.
5. `UdpRegistrar` creates `RegistrarClient`.
6. `RegistrarClient` connects to the registrar server over TCP.
7. `RegistrarClient` sends `TcpMessageType::Register`.
8. `RegistrarServer` stores the worker in its authoritative registry.
9. `RegistrarServer` sends `TcpMessageType::Accept`.
10. `RegistrarClient` sends periodic `TcpMessageType::Heartbeat`.
11. The server broadcasts `NodeJoin` and `NodeLeave` messages to other clients.

The worker should print:

```text
CLIENT mode
  worker name:    worker-a
  actor endpoint: 127.0.0.1:17001
  actor tcp:      17001
  registrar host: 127.0.0.1
  registrar tcp:  5353
  registrar udp:  5353
```

## Information Exchange

```text
worker-a -> registrar TCP:
  Register {
    endpoint_info.endpoint = "127.0.0.1:17001"
    endpoint_info.host = "<local-ip>"
    endpoint_info.tcp_port = <registered tcp port from current implementation>
    acceptors = [...]
  }

registrar -> worker-a TCP:
  Accept {
    error_code = 0
  }

worker-b -> registrar TCP:
  Register {
    endpoint_info.endpoint = "127.0.0.1:17002"
    endpoint_info.host = "<local-ip>"
    endpoint_info.tcp_port = <registered tcp port from current implementation>
    acceptors = [...]
  }

registrar -> worker-a TCP:
  NodeJoin {
    endpoint_info.endpoint = "127.0.0.1:17002"
    endpoint_info.host = "<local-ip>"
    endpoint_info.tcp_port = <registered tcp port from current implementation>
  }

worker-a -> registrar TCP:
  Heartbeat {}
```

The example should describe the registered endpoint string as the primary node
identity. It should print the response `tcp_port` field as part of the wire
payload, but should not present it as the registrar port unless that is exactly
what the current implementation sent.

## UDP Query And Response

Query mode sends one UDP request:

```text
ResolveQuery target: 127.0.0.1:17001
Destination:         127.0.0.1:5353
```

The UDP packet format is:

```text
[Magic HPAC:4][Version:1][Type:1][Length:4][Reserved:2][Payload...]
```

The query payload is `PbResolveQueryPayload`:

```text
target_endpoint = "127.0.0.1:17001"
```

The server path is:

1. UDP socket receives packet.
2. `UdpRegistrar::handle_udp_packet()` validates header and version.
3. `ResolveQuery` payload is parsed.
4. `RegistrarServer::registry()` is queried for the target endpoint.
5. If found, the server returns `ResolveResponse`.

The response payload is `PbResolveResponsePayload`:

```text
endpoint_info.endpoint = "127.0.0.1:17001"
endpoint_info.host     = "<registered host>"
endpoint_info.tcp_port = <registered tcp_port field>
```

Query mode should print either:

```text
RESOLVED
  endpoint: 127.0.0.1:17001
  host:     127.0.0.1
  tcp_port: 5353
```

or:

```text
NOT FOUND
  endpoint: 127.0.0.1:17999
```

## Design Decisions

1. Keep this as one example binary with multiple modes. This matches existing
   cross-process examples and makes manual demos simple.
2. Use the real `ActorSystem` startup path rather than constructing
   `UdpRegistrar` directly. The example should teach how users encounter the
   registrar through normal HPActor configuration.
3. Use a manual UDP query probe in `--query` mode. There is no public
   `UdpRegistrar::resolve_remote()` API today, and building the packet in the
   example makes the wire flow explicit.
4. Default registrar port is `5353` to match `RegistrarConfig`. The CLI exposes
   `--registrar-port` so demos can avoid local port conflicts.
5. Keep the example exception-free. HPActor builds with `-fno-exceptions`, so
   CLI parsing and UDP query errors should return status strings instead of
   throwing.
6. Do not add new framework APIs for this example. The goal is educational
   coverage of existing behavior.

## Out Of Scope

- Gossip or hybrid discovery.
- Multi-server discovery.
- TLS.
- Remote actor messaging after UDP resolution.
- Fixing or redefining `NodeEndpoint::tcp_port` semantics.
- Production CLI parsing library.

## Success Criteria

The example is successful when:

1. The server process prints that it is the registrar server.
2. Each worker process prints that it is a registrar client.
3. The server logs registrations for worker endpoints.
4. Workers stay registered through heartbeat messages.
5. A query for a known worker endpoint prints a decoded `ResolveResponse`.
6. A query for an unknown endpoint prints `NOT FOUND` or times out with a clear
   message.
7. `ninja -C build` builds the example.
8. Existing registrar tests still pass.
