# Review the implementation, continue the unfinished task ane technical debet
  - ❌ Server handle_tcp_message() — doesn't process Register messages
  - ❌ Client fd storage — clients_ map never populated
  - ❌ Null pointer issue — RegistrarClient gets nullptr for shared_registry
  - ❌ UDP response — handle_udp_packet() doesn't send ResolveResponse
  - ❌ Hardcoded "127.0.0.1" — won't work across machines
  - ❌ Registration missing AcceptorInfo in payload
  - ❌ Failover reconnect logic — stubbed