.. HPActor Developer Manual documentation master file

HPActor Developer Manual
========================

**HPActor** is a C++20 event-based actor framework for building scalable,
high-performance distributed applications. Inspired by CAF (C++ Actor
Framework), it provides cooperative scheduling, hierarchical supervision,
pluggable service discovery, and a production reliability plane with
delivery semantics, bounded mailboxes, dead-letter queues, distributed
tracing, and deterministic fault injection.

This manual is the authoritative guide for developing, deploying,
monitoring, and operating HPActor-based systems.

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   getting-started/overview
   getting-started/installation
   getting-started/your-first-actor
   getting-started/project-structure

.. toctree::
   :maxdepth: 2
   :caption: Building Applications

   building-applications/actor-types
   building-applications/message-passing
   building-applications/lifecycle
   building-applications/topology-config
   building-applications/remote-actors
   building-applications/distributed-patterns

.. toctree::
   :maxdepth: 2
   :caption: Monitoring

   monitoring/metrics
   monitoring/logging
   monitoring/tracing
   monitoring/health

.. toctree::
   :maxdepth: 2
   :caption: Operations

   operations/cli
   operations/cli-server
   operations/http-gateway
   operations/daemon-mode

.. toctree::
   :maxdepth: 2
   :caption: SRE Integration

   sre-integration/prometheus-grafana
   sre-integration/logging-loki
   sre-integration/tracing-jaeger
   sre-integration/alerting
   sre-integration/chaos-engineering

.. toctree::
   :maxdepth: 2
   :caption: Best Practices

   best-practices/actor-design
   best-practices/error-handling
   best-practices/performance
   best-practices/testing
   best-practices/deployment

.. toctree::
   :maxdepth: 2
   :caption: Python Binding

   python/index
   python/installation
   python/your-first-actor
   python/actor-api
   python/topology
   python/external-sdk

.. toctree::
   :maxdepth: 2
   :caption: Appendix

   limitations
