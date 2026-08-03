# 1. Worktree Isolation

**Every design or implementation write MUST happen in an isolated git worktree.**
The main checkout at the repository root is for read-only inspection only.

## Starting Work

When a task requires writing files (code, docs, config, tests, build artifacts):

1. If not already in a linked worktree, create one.
2. Do all edits inside `.claude/worktrees/<short-task-name>/`.
3. Use the worktree-local `build/` directory for configure, build, and test output.
4. `.claude/worktrees/` is already gitignored — no additional ignore rule needed.

## Branch Naming — Hard Constraints

Worktree branches MUST follow a strict `<prefix>/<short-description>` pattern.

**Permitted prefixes (use EXACTLY as shown with the trailing `/`):**

| Prefix | Use for |
|--------|---------|
| `worktree/` | General task work (edits, cleanups, reorgs) |
| `feature/` | New features; include ticket ID when available (e.g., `feature/sys-284-daemon-service-design`) |
| `fix/` | Bug fixes (e.g., `fix/rate-limiter-lost-wakeup-spin`) |
| `docs/` | Documentation-only changes (e.g., `docs/dedup-claude-md-rules`) |
| `refactor/` | Pure refactors with no behavior change |

**Description rules:**

- Lowercase kebab-case ONLY: `[a-z][a-z0-9]*(-[a-z][a-z0-9]*)*`.
- Short and descriptive (3–6 words).
- NO underscores (`_`), NO plus signs (`+`), NO uppercase.
- The prefix and description are separated by `/`, NEVER by `-`, `+`, or `_`.

**The worktree directory MUST match:**
`.claude/worktrees/<short-description>/` — i.e., the branch name with the
prefix and trailing `/` stripped. For branch `worktree/foo-bar`, the directory
is `.claude/worktrees/foo-bar/`.

**Anti-patterns — these branch names are REJECTED:**

| Wrong | Problem | Correct |
|-------|---------|---------|
| `worktree-fix+msg-006-foo` | `-` instead of `/` after prefix; `+` in description | `worktree/msg-006-foo` or `fix/msg-006-foo` |
| `worktree-msg-005-bar` | `-` instead of `/` after prefix | `worktree/msg-005-bar` |
| `worktree-pybind-baz` | `-` instead of `/` after prefix | `worktree/pybind-baz` |
| `feature/my_feature` | Underscore in description | `feature/my-feature` |
| `fix/CamelCaseBug` | Uppercase in description | `fix/camel-case-bug` |
| `misc/some-task` | `misc/` is not a permitted prefix | Use `worktree/some-task` |

**Correct examples:**

```
git worktree add -b worktree/dedup-claude-md-rules .claude/worktrees/dedup-claude-md-rules main
git worktree add -b feature/act-009-durable-outbox .claude/worktrees/act-009-durable-outbox main
git worktree add -b fix/lost-wakeup-mailbox .claude/worktrees/lost-wakeup-mailbox main
```

**Branch name validation — MANDATORY before `git worktree add`:**

Before creating a worktree, Claude MUST mentally validate the branch name
against this checklist. If ANY check fails, STOP and pick a compliant name:

1. Does the branch start with exactly one of `worktree/`, `feature/`, `fix/`, `docs/`, `refactor/` (with the `/`)?
2. After the prefix, does the description contain ONLY `[a-z0-9-]`?
3. Does the description start and end with a letter or digit (not `-`)?
4. Are there NO consecutive hyphens (`--`)?
5. Does `.claude/worktrees/<description>/` match the worktree path?

**Equivalent regex the branch name must match:**
`^(worktree|feature|fix|docs|refactor)/[a-z][a-z0-9]*(-[a-z][a-z0-9]*)*$`

## Writing Files — Hard Rules

- **Verify your directory** before every write: `pwd` must print
  `.../HPActor/.claude/worktrees/<name>/`, never `.../HPActor/` (the main checkout).
- **Prefer relative paths** — they resolve against the worktree root automatically.
- **NEVER target the main checkout path.** The absolute path
  `/Users/skg7on/Workspace/Projects/HPActor/` is the main checkout — files
  written there land on `main`, not your worktree branch.
- **Subagents inherit CWD.** If a subagent uses an absolute path, derive it from
  `pwd` at runtime, never from a hardcoded string.

## CWD Verification — Hard Gate

Before ANY file write (Edit, Write, or a shell command that creates/modifies
files), Claude MUST satisfy this gating condition:

- `pwd` resolves to a path under `.claude/worktrees/<name>/`.
- If `pwd` shows the main checkout path (`.../HPActor/` without
  `.claude/worktrees/`), the write is BLOCKED. Create or enter a worktree first.

This is not advisory. A write to the main checkout is a rule violation.

## Before Committing

- Confirm `git branch --show-current` shows the worktree branch, not `main`.
- Run `git status` to verify all changes are in the worktree and no files
  leaked to the main checkout.
