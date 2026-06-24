# DeathPact & PipeTo — Design Document

## 1. DeathPact

### 1.1 Overview
The CAF-style death pact pattern causes a watching actor to terminate when a linked actor dies. In HPActor, this is implemented via a boolean flag on `ActorContext`.

### 1.2 API
```cpp
void set_death_pact(bool enabled);
bool has_death_pact() const;
```

### 1.3 Behavior
When `death_pact_` is true and a `DownMsg` arrives for a linked actor, the watching actor calls `passivate()` (triggers its own shutdown). Monitoring (one-way) is not affected — only linked (bidirectional) actors trigger the death pact.

### 1.4 Default
`false` — existing behavior is preserved.

## 2. PipeTo

### 2.1 Overview
The Akka PipeTo pattern forwards a resolved result to a target actor via success/error callbacks. HPActor implements this as a free function template.

### 2.2 API
```cpp
template <typename T>
void pipe_to(const result<T>& r, const ActorAddress& target,
             std::function<void(const ActorAddress&, T)> on_success,
             std::function<void(const ActorAddress&, error)> on_error);
```

### 2.3 Usage
```cpp
auto r = handle.get();
pipe_to(r, receiver_addr,
        [](const ActorAddress& t, MyResponse v) { t.send(...); },
        [](const ActorAddress& t, error e) { t.send_error(e); });
```

### 2.4 Design Decision
Header-only utility. No ActorSystem or actor context required. The `result<T>` type is defined in `types.hpp`.

## 3. References
- [ActorContext header](../../include/hpactor/actor/actor_context.hpp) — death_pact
- [PipeTo header](../../include/hpactor/actor/pipe_to.hpp) — pipe_to
- [Akka Gap Analysis (Issue #329)](https://github.com/skg7on/HPActor/issues/329)
