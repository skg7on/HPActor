# HPActor Python Examples

These examples demonstrate the Phase 1D in-process binding. They use
generated protobuf messages with explicit, stable TypeTags. No example
uses pickle, JSON, or any non-protobuf message path.

## Running

Install the hpactor wheel first, then run any example directly:

```bash
pip install hpactor
python echo.py
```

## Example list

| File | Description |
|------|-------------|
| `echo.py` | Five-minute first actor — spawn, ask/reply, shutdown |
| `operations.py` | Runtime lifecycle — send fire-and-forget, clean shutdown |

Each example exits cleanly with no runtime threads left behind.
