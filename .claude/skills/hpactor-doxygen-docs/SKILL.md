---
name: hpactor-doxygen-docs
description: Use when implementing HPActor C++ features, fixing bugs, changing exported headers, or documenting/reviewing public APIs that need strict Doxygen comments for classes, structs, enums, methods, and free functions.
---

# HPActor Doxygen Docs

Keep HPActor public API documentation synchronized with feature and bug-fix work.

## Workflow

1. Read `AGENTS.md`, `CLAUDE.md`, and `CLAUDE_MEMORY.md` before editing.
2. Work in the required `.worktrees/` checkout if the current checkout is not already a linked worktree.
3. For every feature-development or bug-fix task, identify new or changed public declarations before the task is complete.
4. Start with exported headers under `include/hpactor/`; include `src/`, tests, and architecture docs only to confirm behavior.
5. Add or update strict Doxygen for every touched key public class, struct, enum, public method, constructor, destructor, free function, public field, template, alias, and callback type.
6. If no public API documentation changes are needed, state why in the final response.
7. Preserve declarations, formatting intent, ABI, generated files, and TypeTag/protobuf compatibility. Do not refactor while documenting.
8. Verify by building when practical; for comment-only changes, at minimum run a focused syntax/format check or inspect the touched declarations for malformed comments.

## Strict Style

- Use C++ Doxygen comments immediately before declarations: `///` for multi-line declaration docs and `///<` for enum values or simple public fields.
- Prefer backslash commands consistently: `\brief`, `\param`, `\tparam`, `\return`, `\retval`, `\pre`, `\post`, `\note`, `\warning`, `\code{.cpp}`, `\endcode`.
- Every documented function with parameters must have one `\param[in]`, `\param[out]`, or `\param[in,out]` per parameter, using the exact parameter names.
- Every non-`void` function must document `\return`, or use `\retval` when the return space is a small named set.
- Every template parameter must have `\tparam`.
- Destructors and ownership-transfer APIs must document lifetime and ownership effects.
- Blocking, scheduler, actor-context, thread-affinity, lock-free, and event-loop APIs must include a `\note` that states the concurrency contract.
- Error-returning APIs must document error behavior with `\return`, `\retval`, `\post`, or `\note`. HPActor is generally built without exceptions, so do not add `\throws` unless the specific API is compiled in an exception-enabled translation unit and actually throws.
- Avoid undocumented Doxygen commands unless a Doxygen configuration in the repo defines them.

```cpp
/// \brief Short imperative summary of the API contract.
///
/// Longer details only when they add caller-visible behavior.
///
/// \tparam T Exact role of the template parameter.
/// \param[in] input Exact caller obligation for this argument.
/// \param[out] output Exact mutation or value written by the API.
/// \return Caller-visible result, ownership, or error state.
/// \note Thread safety: State whether the API is actor-confined,
///       externally synchronized, lock-free, blocking, or event-loop-only.
```

## Content Rules

- Document observed behavior only. If behavior is unclear, inspect implementation, tests, or architecture docs before writing the comment.
- Say what the API guarantees, what the caller owns, and what preconditions matter. Avoid restating the signature.
- Keep comments concise and operational. Prefer one clear sentence over broad background.
- Mark backlog or design-only behavior as planned only when the existing docs explicitly say it is not implemented.
- Do not add placeholders, marketing language, speculative guarantees, or examples that have not been checked against the current API.

## HPActor Focus Areas

- Actor lifecycle and message flow: sender/reply semantics, child ownership, monitors, links, shutdown, and scheduled delivery.
- Mailbox and scheduler APIs: bounded capacity, admission results, wakeup semantics, blocking behavior, and thread-safety.
- Network and RPC APIs: endpoint ownership, async completion, timeout behavior, serialization, and remote/local differences.
- Config and topology APIs: ownership of parsed models, validation failures, registration timing, and TOML/parser isolation.
- Metrics, logging, tracing, and memory APIs: allocation ownership, ring-buffer capacity, drain behavior, labels, and export format guarantees.

## Review Checklist

- No declaration or behavior changed.
- Feature and bug-fix changes include strict Doxygen for new or modified public API declarations, or explicitly record why no Doxygen update was needed.
- Every parameter, return value, template parameter, ownership rule, and relevant concurrency rule is documented.
- Comments match implementation and tests, not only the architecture backlog.
- The style is Doxygen-compatible and consistent across touched headers.
- Verification command output is recorded in the final response.
