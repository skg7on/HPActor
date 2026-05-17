# Low-Coverage Test Design

**Date:** 2026-05-17
**Status:** approved
**Approach:** A — by subsystem, simplest-first within each

## Summary

Design test cases for all 24 source files currently below 50% line coverage
across 9 subsystems. Target ≥80% line and function coverage per file. Follow
existing test conventions: standalone `.cpp` executables linked against
`hpactor`, using `assert()` + `printf("PASSED ...\n")` in `main()`, registered
in `tests/CMakeLists.txt`.

## Test Conventions

- File: `tests/<subsystem>/test_<component>.cpp`
- No test framework dependency (no GTest, no Catch2)
- Deterministic: no timing assumptions; use `scheduler_threads = 0` when
  inspecting mailbox/lifecycle state; generous timeouts (5s+) when scheduler is
  needed
- Build: `add_executable` + `target_link_libraries(test_<name> hpactor)` +
  `add_test(NAME test_<name> COMMAND test_<name>)`

## Subsystem 1: `sched/`

### `test_coroutine_frame_pool.cpp`

| Test | Description |
|------|-------------|
| `test_acquire_release` | Acquire returns valid frame; release returns it; `empty()/available()/total()` reflect state |
| `test_exhaustion` | For N-frame pool, N acquires succeed, N+1th returns nullptr |
| `test_release_nullptr` | `release(nullptr)` is no-op |
| `test_double_release` | Frame with `in_use=false` is no-op on release |
| `test_reacquire` | acquire → release → acquire returns same frame |
| `test_concurrent_stress` | N threads acquire/release M times; final available == total |
| `test_empty_true` | Empty pool after acquiring all frames |
| `test_constructor_params` | `available()` and `total()` match constructor args |

### `test_worker_thread.cpp`

| Test | Description |
|------|-------------|
| `test_start_stop` | `start()` sets running, `stop()` joins thread |
| `test_double_start` | Second `start()` returns immediately |
| `test_push_pop` | Push work item, pop returns it |
| `test_steal` | Can steal from worker with queued items |
| `test_depth` | `depth()` reflects queue size |
| `test_donation_count` | `increment_donations()` increases count |
| `test_acquire_release_frame` | Delegates to frame pool when set |
| `test_try_steal` | With scheduler owner, performs A2WS victim selection |
| `test_process_noop` | `process()` is a no-op placeholder |
| `test_index` | `index()` matches config `worker_index` |

## Subsystem 2: `log/`

### `test_log_sinks.cpp`

| Test | Description |
|------|-------------|
| `test_stderr_write` | `write("hello")` succeeds, content goes to stderr |
| `test_stderr_flush` | `flush()` succeeds |
| `test_file_write` | Write to temp file; verify content on disk |
| `test_file_flush` | `flush()` on open file succeeds |
| `test_file_write_closed` | Write to closed file returns error |
| `test_file_factory` | `make_file_sink(path)` returns non-null |
| `test_rotating_write_below_threshold` | Write below `max_bytes`; verify content |
| `test_rotating_triggers_rotation` | Write exceeding `max_bytes` triggers rotation; old file → `.1` |
| `test_rotating_multi_file_chain` | `max_files=3`; multiple rotations produce `.1`/`.2`/`.3` |
| `test_rotating_flush` | `flush()` on open file succeeds |
| `test_rotating_write_closed` | Write to closed file returns error |
| `test_rotating_factory` | `make_rotating_file_sink(cfg)` returns non-null |

## Subsystem 3: `metrics/`

### `test_metrics_aggregator.cpp`

| Test | Description |
|------|-------------|
| `test_families_registered_once` | `ensure_families_registered()` registers 13 families; second call is no-op |
| `test_mailbox_enqueue_event` | Increments gauge + counter with actor labels |
| `test_mailbox_dequeue_event` | Decrements gauge |
| `test_message_processed_event` | Observes histogram with latency value |
| `test_actor_spawned_event` | Increments active_actors + lifecycle counter |
| `test_actor_terminated_event` | Decrements active_actors + lifecycle counter |
| `test_scheduler_dispatch_event` | Per-worker counter with worker_id label |
| `test_scheduler_steal_event` | Per-source counter with source_worker label |
| `test_supervisor_restart_event` | Counter with actor labels |
| `test_memory_alloc_event` | Gauge increments by value_hi |
| `test_memory_free_event` | Gauge decrements by value_hi |
| `test_rejection_drop_dead_letter_events` | Three counters: mailbox_rejected, mailbox_dropped, mailbox_dead_letter |
| `test_backpressure_dead_letter_lost` | Two counters: backpressure_signals, dead_letter_lost |
| `test_stub_events_noop` | kLifecycleTransition, kMessageRejected, kActorDrain* are no-ops |
| `test_begin_end_drain` | `end_drain()` records active actor gauge |
| `test_make_labels_cached` | First call resolves type name; second call uses cache |
| `test_make_labels_unknown_actor` | Uses "unknown" type when actor not found |
| `test_make_labels_id_format` | Label includes actor_id as decimal string |

## Subsystem 4: `actor/`

### `test_scoped_actor.cpp`

| Test | Description |
|------|-------------|
| `test_construct_destruct` | Construction with ActorSystem, destruction calls `on_deactivate()` |

### `test_local_actor.cpp`

| Test | Description |
|------|-------------|
| `test_two_arg_constructor` | Zero id, provided context and system |
| `test_three_arg_constructor` | Provided id, context, and system |

### `test_spawn_receiver.cpp`

| Test | Description |
|------|-------------|
| `test_make_behavior_returns_valid` | `make_behavior()` returns a Behavior |
| `test_handle_spawn_request_success` | Valid request calls registry.spawn, sends response via transport |
| `test_handle_spawn_request_failure` | Failed spawn sends error response |
| `test_handle_spawn_request_null_transport` | Null transport skips response send (no crash) |

## Subsystem 5: `ref/`

### `test_actor_proxy.cpp` (extend existing)

| Test | Description |
|------|-------------|
| `test_construct_with_transport` | Address + transport constructor |
| `test_construct_with_system` | Address + system resolves transport |
| `test_try_send_no_transport` | Dead letter (RemoteNodeUnreachable), returns ActorNotFound |
| `test_try_send_transport_failure` | Dead letter (TransportSendFailed), returns Rejected |
| `test_try_send_success` | Valid transport → Accepted |
| `test_try_send_cache_hit` | Location cache hit → uses cached endpoint |
| `test_try_send_discovery_miss` | Discovery miss → dead letter (MissingRoute) |
| `test_try_send_with_trace_context` | Frame includes trace context proto |
| `test_send_fire_and_forget` | `send()` wraps `try_send()`, discards result |

## Subsystem 6: `mem/`

### `test_guard_page.cpp` (extend existing)

| Test | Description |
|------|-------------|
| `test_page_size_positive` | `page_size()` returns positive value matching `sysconf` |
| `test_guarded_alloc_nonnull` | `guarded_alloc(N)` returns non-null, memory is readable/writable |
| `test_guarded_alloc_zero` | `guarded_alloc(0)` still allocates guard pages |
| `test_guarded_alloc_oob_sigsegv` | Write before/after usable region → SIGSEGV (fork+waitpid) |
| `test_guarded_free_valid` | Frees successfully |
| `test_guarded_free_null` | `guarded_free(nullptr, N)` is no-op |
| `test_handler_install_idempotent` | Second `install_corruption_handler()` is no-op |
| `test_handler_remove_restore` | `remove_corruption_handler()` restores previous; second call no-op |
| `test_set_log_fd` | `set_guard_page_log_fd(fd)` sets fd |

## Subsystem 7: `supervision/`

### `test_supervision.cpp` (extend existing)

| Test | Description |
|------|-------------|
| `test_one_for_one_passes_directive` | `on_child_failure()` returns failure.directive |
| `test_all_for_one_always_restart` | `on_child_failure()` always returns Restart |
| `test_supervisor_actor_construct` | Construction initializes children vector |
| `test_supervisor_handle_down_restart` | DownMsg with Restart → `restart_child()` |
| `test_supervisor_handle_down_stop` | DownMsg with Stop → removes child |
| `test_supervisor_handle_down_escalate` | DownMsg with Escalate → no-op |
| `test_supervisor_restart_child_lifecycle` | Drives Failed→Starting transitions, bumps incarnation |
| `test_supervisor_restart_rate_limit` | 10 restarts in 5s window; 11th removes child |
| `test_supervisor_restart_metrics` | Emits kSupervisorRestart event when ring buffer set |
| `test_supervisor_restart_all` | Restarts each child |
| `test_self_supervising_add_remove_child` | Local child CRUD |
| `test_self_supervising_remote_child_crud` | `add_remote_child/remove_remote_child/has_remote_child/get_remote_child` |
| `test_self_supervising_decide_restart_within_limit` | Within rate limit → calls `on_failure()` |
| `test_self_supervising_decide_restart_exceeded` | Exceeded max_restarts → returns Stop |
| `test_self_supervising_decide_restart_reset` | Counter resets after `restart_interval` |

## Subsystem 8: `cli/`

### `test_line_editor.cpp` (extend existing)

| Test | Description |
|------|-------------|
| `test_construct_sets_singleton` | Constructor sets `current_` |
| `test_destruct_clears_singleton` | Destructor clears `current_` |
| `test_readline_returns_input` | `readline()` returns user input string |
| `test_readline_null_line` | Null from linenoise → empty string |
| `test_add_history` | `add_history()` calls linenoiseHistoryAdd |
| `test_load_save_history` | `load_history()`/`save_history()` with path |
| `test_tokenize_partial_basic` | Splits "foo bar baz" into ["foo","bar","baz"] |
| `test_tokenize_partial_skips_eof` | Eof tokens are excluded |
| `test_completion_exact_match` | Exact keyword match advances node |
| `test_completion_prefix_match` | Prefix "act" matches "actor" keyword |
| `test_completion_parameter_token` | Parameter tokens use user input in prefix |
| `test_hints_gray_for_partial` | Gray hint shown for partial keyword match |
| `test_hints_null_when_no_match` | No match → nullptr |
| `test_free_hints` | Frees strdup result |

### `test_cli_actor.cpp`

| Test | Description |
|------|-------------|
| `test_history_path_config` | Config path takes precedence |
| `test_history_path_home_fallback` | Falls back to `$HOME/.hpactor_history` |
| `test_parse_actor_id_decimal` | "42" → ActorId{42} |
| `test_parse_actor_id_hex` | "0xFF" → ActorId{255} |
| `test_parse_actor_id_invalid` | "abc" → ActorId{0} |
| `test_build_command_tree` | All commands registered |
| `test_execute_tokens_help` | "/help" shows available commands |
| `test_execute_tokens_quit` | "/quit" sets running_=false |
| `test_execute_tokens_unknown_command` | Unknown command shows error with suggestion |
| `test_execute_tokens_flag_handling` | Flags stored as params |
| `test_execute_tokens_leaf_no_execute` | Shows help for intermediate node |
| `test_run_once_eof` | Returns false on null/empty input |
| `test_enumerate_actors_no_filter` | Returns all actors |
| `test_enumerate_actors_with_filter` | Filters by type name substring |
| `test_system_stats` | "/system stats" shows actor count, scheduler threads |
| `test_actor_list` | "/actor list" shows table |

## Subsystem 9: `net/`

### `test_hybrid_discovery.cpp`

| Test | Description |
|------|-------------|
| `test_start_stop_order` | Starts registrar then gossip; stops gossip then registrar |
| `test_discover_local_first` | `discover()` checks registrar first, falls back to gossip |
| `test_discover_all_merge` | Merges local + remote; same-host takes precedence |
| `test_announce_local` | Local endpoint → goes to registrar AND gossip |
| `test_announce_remote` | Remote endpoint → only gossip |
| `test_member_change_callback` | Callback wired through both layers |

### `test_connection_pool.cpp` (extend existing)

| Test | Description |
|------|-------------|
| `test_get_connection_round_robin` | Round-robin selection from active connections |
| `test_get_connection_empty` | Returns nullptr when no active connections |
| `test_send_via_connection` | Routes via active connection |
| `test_send_queues_when_no_connection` | Queues pending when no connection available |
| `test_try_send_returns_false_on_full` | Returns false when pending queue is full |
| `test_drain_clears_and_returns_unsent` | Shuts down, clears, returns unsent count |
| `test_abort_clears` | Shuts down without returning unsent |
| `test_stats` | Reflects active/pending/reconnect counts |
| `test_reconnect_backoff` | Exponential backoff with max attempts |
| `test_reconnect_max_attempts_guard` | No reconnect after max attempts |
| `test_flush_pending` | Drains pending queue to available connections |
| `test_add_pending_max_limit` | Returns false when pending queue full |
| `test_on_frame_rpc_response` | Routes RPC responses to handler |
| `test_on_frame_spawn_response` | Routes spawn responses to handler |
| `test_on_frame_actor_message` | Routes actor messages to handler |

### Networking files requiring integration tests

The remaining networking files (`tls_context`, `tcp_transport`, `tls_connection`,
`registrar_client`, `registrar_server`, `registrar`, `gossip_membership`) are large,
I/O-heavy modules that need real event loops and socket pairs. Tests follow patterns
from existing net tests (`test_tcp_transport_comprehensive.cpp`,
`test_tls_integration.cpp`, `test_registrar_connection.cpp`). Design specifics to be
detailed during implementation based on available test infrastructure.

## Implementation Order

1. `sched/` — `test_coroutine_frame_pool`, `test_worker_thread`
2. `log/` — `test_log_sinks`
3. `metrics/` — `test_metrics_aggregator`
4. `actor/` — `test_scoped_actor`, `test_local_actor`, `test_spawn_receiver`
5. `ref/` — extend `test_actor_proxy`
6. `mem/` — extend `test_guard_page`
7. `supervision/` — extend `test_supervision`
8. `cli/` — extend `test_line_editor`, `test_cli_actor`
9. `net/` — `test_hybrid_discovery`, extend `test_connection_pool`, then remaining files
