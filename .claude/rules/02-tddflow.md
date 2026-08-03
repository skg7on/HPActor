# 2. TDDFlow

**All production code changes MUST follow RED → GREEN → REFACTOR.**

## When This Applies

TDDFlow is mandatory for: features, bug fixes, refactors, and behavior changes.

Exceptions (must be explicitly approved by the user): generated code, throwaway
exploration, docs-only work, configuration-only work.

## Workflow

1. **RED** — Write one focused failing test that describes the next required
   behavior. Run the narrowest relevant test command. Confirm it fails for the
   expected reason.
2. **GREEN** — Write the minimum implementation needed to pass that test. Run
   the same focused command. Confirm it passes.
3. **REFACTOR** — Clean up while keeping the same tests green. Do not add new
   behavior during this step.
4. Repeat for each behavior or edge case until the design is fully implemented.

Never write production implementation before observing the failing test.
Record the RED and GREEN verification commands in the final response for
feature and bug-fix tasks.

Invoke the `tddflow-development` skill (`.claude/skills/tddflow-development/`)
for detailed guidance after design approval.
