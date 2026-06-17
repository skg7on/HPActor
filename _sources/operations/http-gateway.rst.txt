.. _operations-http-gateway:

HTTP Gateway
============

The :cpp:class:`HTTPGatewayActor` provides HTTP ingress for external
tooling, enabling RESTful interaction with an HPActor system.

Overview
--------

.. code-block:: text

   External Tool ──► HTTP :8080 ──► HTTPGatewayActor ──► Actors
   (curl, browser,                    │
    Prometheus, etc.)                 ├── /metrics  → MetricsActor
                                      ├── /healthz  → HealthHttpServer
                                      ├── /api/*    → Application actors
                                      └── /admin/*  → System commands

The gateway translates HTTP requests into actor messages and HTTP
responses back to the client.

Configuration
-------------

.. code-block:: toml

   [system.http]
   enabled = true
   listen_address = "0.0.0.0"
   listen_port = 8080
   max_connections = 1000
   request_timeout_ms = 30000

   # Optional TLS
   # [system.http.tls]
   # cert_file = "/etc/ssl/certs/hpactor.crt"
   # key_file = "/etc/ssl/private/hpactor.key"

Built-in Endpoints
------------------

.. list-table::
   :header-rows: 1

   * - Endpoint
     - Method
     - Description
   * - ``/metrics``
     - GET
     - Prometheus OpenMetrics text
   * - ``/healthz``
     - GET
     - Liveness check (returns 200)
   * - ``/readyz``
     - GET
     - Readiness check (200 or 503)
   * - ``/api/v1/actors``
     - GET
     - List actors (query: ``?type=worker&state=running``)
   * - ``/api/v1/actors/:id``
     - GET
     - Actor detail
   * - ``/api/v1/actors/:id``
     - POST
     - Send a message to an actor
   * - ``/api/v1/dlq``
     - GET
     - DLQ records (query: ``?actor_id=3&limit=100``)
   * - ``/api/v1/dlq/:index/replay``
     - POST
     - Replay a DLQ record

Custom HTTP Routes
------------------

Register application-specific routes:

.. code-block:: cpp

   class MyHttpGateway : public net::HTTPGatewayActor {
   protected:
       void configure_routes(RouteTable& routes) override {
           HTTPGatewayActor::configure_routes(routes);  // keep built-ins

           // GET /api/v1/orders/:id
           routes.get("/api/v1/orders/:id",
               [this](const HttpRequest& req) -> HttpResponse {
                   auto order_id = req.path_param("id");
                   auto result = query_order(order_id);
                   return HttpResponse::json(result);
               });

           // POST /api/v1/orders
           routes.post("/api/v1/orders",
               [this](const HttpRequest& req) -> HttpResponse {
                   auto order = parse_order(req.body());
                   auto ref = context()->spawn<OrderActor>();
                   context()->send(ref, order);
                   return HttpResponse::created({.id = ref.id()});
               });
       }
   };

   HPACTOR_REGISTER_ACTOR(MyHttpGateway, "my_http_gateway");

Message Protocol
----------------

POST to ``/api/v1/actors/:id`` sends a message:

.. code-block:: bash

   curl -X POST http://localhost:8080/api/v1/actors/3 \
       -H "Content-Type: application/json" \
       -d '{"type": "ProcessOrder", "order_id": "ORD-1234", "quantity": 5}'

The gateway:
1. Parses the JSON body.
2. Looks up the ``TypeTag`` from the ``type`` field.
3. Deserializes the protobuf payload.
4. Sends the ``TypedMessage`` to the target actor.
5. Returns the response or a 202 Accepted for fire-and-forget.

Security
--------

- **Rate limiting**: Configure per-endpoint rate limits (future).
- **Authentication**: Use a reverse proxy (Nginx, Envoy) with mTLS for
  production HTTP access.
- **Input validation**: The gateway validates message structure but
  delegates business-logic validation to actors.

Integration Example: Prometheus
-------------------------------

.. code-block:: yaml

   # prometheus.yml
   scrape_configs:
     - job_name: 'hpactor'
       static_configs:
         - targets: ['my-app:8080']
       metrics_path: '/metrics'

Integration Example: Health Check for Load Balancer
---------------------------------------------------

.. code-block:: nginx

   upstream hpactor_backend {
       server 10.0.1.1:8080;
       server 10.0.1.2:8080;

       health_check uri=/healthz interval=5s;
   }
