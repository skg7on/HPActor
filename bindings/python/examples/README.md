# HPActor Python Examples

These examples demonstrate the HPActor Python binding API using generated
protobuf messages with explicit `TypeTag` values.

Each example uses `asyncio.run(main())`, registers messages with explicit
TypeTags, and exits with no runtime threads remaining.

## Running

```bash
# Install hpactor and protobuf first
pip install hpactor "protobuf>=7.35.0,<8"

# Run any example
python3 echo.py
python3 request_response.py
python3 supervision.py
python3 operations.py
```

## Examples

| File | Description |
|------|-------------|
| `echo.py` | Five-minute first actor: spawn, ask, reply |
| `request_response.py` | Typed request-response with ActorError handling |
| `supervision.py` | Handler failure with automatic restart |
| `operations.py` | Delivery options and clean shutdown |
