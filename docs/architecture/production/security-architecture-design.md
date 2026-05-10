# Security Architecture Design

## 1. Executive Summary

An actor runtime that supports remote spawn, RPC, CLI, metrics, HTTP ingress,
and service discovery needs a clear security model. Production HPActor should
assume untrusted networks, compromised clients, accidental exposure of admin
interfaces, and the need for auditability.

This design defines node identity, transport security, authentication,
authorization, secret handling, audit logging, and secure defaults.

## 2. Goals

1. Use authenticated encryption for cluster traffic.
2. Authenticate nodes, operators, and service clients.
3. Authorize sensitive actions such as remote spawn and admin commands.
4. Protect metrics, CLI, and HTTP management endpoints.
5. Support certificate and secret rotation without full cluster downtime.
6. Produce audit logs for security-relevant actions.

## 3. Non-Goals

- Building a full identity provider.
- Replacing application-level authorization.
- Supporting plaintext remote cluster traffic as a production default.

## 4. Identity Model

Identities:

- `NodeIdentity`: runtime process participating in a cluster.
- `OperatorIdentity`: human or automation using CLI/Admin API.
- `ServiceIdentity`: ingress client or peer service.
- `ActorIdentity`: optional logical identity for actor-level authorization.

Node identity fields:

- cluster id
- node id
- certificate fingerprint
- incarnation
- allowed roles
- deployment zone

## 5. Transport Security

Production default:

- mTLS for inter-node TCP transport.
- Certificate verification against configured trust roots.
- Node id bound to certificate subject or SAN.
- Protocol version negotiation inside authenticated channel.
- Plain TCP disabled unless explicitly configured for dev/test.

Certificate rotation:

- Trust bundle reload.
- Dual-cert overlap window.
- New connections use new certificate.
- Existing connections close when old certificate expires or policy requires.

## 6. Authentication And Authorization

Authorization resources:

- remote spawn
- remote send to protected system actors
- RPC
- CLI commands
- Admin API endpoints
- metrics endpoint
- DLQ replay
- config reload
- shard movement
- shutdown commands

Policy model:

```toml
[system.security]
mode = "enforce"

[[system.security.role]]
name = "operator"
allow = ["cluster.read", "actor.inspect", "dlq.read"]

[[system.security.role]]
name = "admin"
allow = ["*"]
```

First implementation can use static TOML roles. Later versions can integrate
with SPIFFE, Kubernetes service accounts, or external policy engines.

## 7. Secret Handling

Rules:

- Do not log secrets.
- Redact sensitive TOML keys in errors and CLI output.
- Load secrets from files, environment, or future secret provider abstraction.
- Keep private keys out of serialized topology.
- Support live reload for trust bundle and credentials where possible.

## 8. Audit Logging

Audit events:

- authentication success/failure
- authorization denial
- remote spawn
- admin command
- DLQ replay/export
- config reload
- shutdown/drain command
- cluster quarantine/fencing decision
- certificate reload

Audit records include:

- timestamp
- actor/operator identity
- action
- resource
- decision
- reason
- trace id if present
- source endpoint

## 9. Security Modes

- `off`: dev only, no production support.
- `permissive`: logs violations but allows action.
- `enforce`: denies unauthorized action.

Production default should become `enforce` once migration is complete.

## 10. Observability

Metrics:

- `hpactor_security_auth_total`
- `hpactor_security_authz_denied_total`
- `hpactor_security_tls_handshake_total`
- `hpactor_security_cert_reload_total`

CLI:

- `/security status`
- `/security roles`
- `/security audit --since 10m`

## 11. Acceptance Criteria

- Cluster traffic can run with mTLS and node identity validation.
- Admin and CLI commands require authorization in enforce mode.
- Metrics and DLQ replay can be protected.
- Security-relevant actions produce audit records.
- Secrets are redacted from logs and CLI output.
- Cert reload does not require full cluster downtime.

