# core/ subsystem — DECOMMISSIONED

The `core/` subsystem has been dissolved. Its contents have been relocated:

| Original file              | New location                                    |
|----------------------------|-------------------------------------------------|
| `actor_system.hpp`         | `include/hpactor/actor/actor_system.hpp`        |
| `actor_registry.hpp`       | Folded into `ActorSystem` internals             |
| `actor_system_ids.hpp`     | Merged into `include/hpactor/types/types.hpp`   |
| `actor_ref_cache.hpp`      | `include/hpactor/actor/actor_ref_cache.hpp`     |
| `proto_type_registry.hpp`  | `include/hpactor/msg/proto_type_registry.hpp`   |
| `mailbox.hpp`              | Removed (superseded by `mailbox/mpsc_actor_mailbox.hpp`) |
| `mutex_mailbox.hpp`        | Removed (superseded by `mailbox/mpsc_actor_mailbox.hpp`) |
| `proto_type_registry.cpp`  | `src/msg/proto_type_registry.cpp`               |

Include paths should use the new locations listed above.
