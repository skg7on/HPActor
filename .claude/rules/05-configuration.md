# 5. Configuration

**Prefer subsystem-owned extension points over central switches.**

- New TOML subsystem config MUST use a self-registering parser in
  `src/config/parsers/`. Create a single source file with a static
  `TomlSystemParserRegistration<T>` or `TomlDocumentParserRegistration<T>`
  — no edits to the monolithic parser are needed.
- Parser interfaces MUST use opaque `TomlTableView`. Never expose `toml++`
  headers in a public interface.
- Never grow centralized parser logic when a subsystem parser can own the
  behavior.
