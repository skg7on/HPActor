# HPActor Developer Manual

This directory contains the HPActor Developer Manual in reStructuredText (RST)
format, built with [Sphinx](https://www.sphinx-doc.org/).

## Prerequisites

Install the required Python packages:

```bash
pip install sphinx sphinx-rtd-theme
```

## Build

From this directory (`docs/manual/`):

```bash
make html
```

The built HTML will be in `_build/html/`. Open `_build/html/index.html` in your
browser.

For other output formats:

```bash
make pdf       # requires LaTeX
make epub      # requires epubcheck
make singlehtml
```

To check for broken links:

```bash
make linkcheck
```

## Directory Structure

```
docs/manual/
├── conf.py                          # Sphinx configuration
├── index.rst                        # Master TOC
├── README.md                        # This file
├── getting-started/
│   ├── overview.rst                 # Framework concepts
│   ├── installation.rst             # Build and link
│   ├── your-first-actor.rst         # Hello World actor
│   └── project-structure.rst        # Recommended project layout
├── building-applications/
│   ├── actor-types.rst              # Choosing the right actor
│   ├── message-passing.rst          # TypedMessage, handlers
│   ├── lifecycle.rst                # Spawn, supervise, shutdown
│   ├── topology-config.rst          # TOML bootstrap
│   ├── remote-actors.rst            # Discovery, remote spawn
│   └── distributed-patterns.rst     # Ask, pub-sub, routing
├── monitoring/
│   ├── metrics.rst                  # Prometheus integration
│   ├── logging.rst                  # Structured logging
│   ├── tracing.rst                  # Distributed tracing
│   └── health.rst                   # Health/readiness checks
├── operations/
│   ├── cli.rst                      # CLI commands and usage
│   ├── cli-server.rst               # Remote CLI access
│   ├── http-gateway.rst             # HTTP API
│   └── daemon-mode.rst              # systemd deployment
├── sre-integration/
│   ├── prometheus-grafana.rst       # Metrics + dashboards
│   ├── logging-loki.rst             # Log aggregation
│   ├── tracing-jaeger.rst           # Trace visualization
│   ├── alerting.rst                 # Alert rules
│   └── chaos-engineering.rst        # Fault injection
├── best-practices/
│   ├── actor-design.rst             # Patterns and anti-patterns
│   ├── error-handling.rst           # Failure envelopes
│   ├── performance.rst              # Tuning guide
│   ├── testing.rst                  # Test strategies
│   └── deployment.rst               # Production deployment
└── limitations.rst                  # Known gaps and constraints
```

## Contributing

When adding or editing documentation:

1. Use RST format and follow the existing style.
2. Code examples should reference actual files in `examples/` when possible.
3. Build with `make html` to verify no warnings before committing.
4. Cross-reference other sections using ``:ref:`` and ``:doc:`` roles.
