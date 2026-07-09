# HPActor Python Binding

Asyncio-first Python language binding for the HPActor actor runtime.

## Supported Platforms

| Platform | Architecture | Wheel Tag |
|----------|-------------|-----------|
| Linux (manylinux_2_28) | x86_64 | `manylinux_2_28_x86_64` |
| Linux (manylinux_2_28) | ARM64 | `manylinux_2_28_aarch64` |
| macOS 12.0+ | x86_64 | `macosx_12_0_x86_64` |
| macOS 12.0+ | ARM64 | `macosx_12_0_arm64` |

## Requirements

- **CPython >= 3.11** (CPython 3.11, 3.12, 3.13, 3.14 supported via ABI3)
- **protobuf >= 7.35.0, < 8**
- Linux: glibc 2.28 or newer
- macOS: 12.0 or newer

Windows, musllinux, PyPy, and free-threaded CPython are not supported in this release.

## Installation

```bash
pip install hpactor
```

## Execution Model

HPActor Python actors run on a dedicated asyncio event-loop thread. The C++ scheduler
and network threads never call Python or acquire the GIL. Cross-thread communication
uses bounded value-only queues — no `PyObject*` crosses the native boundary.

## Usage

```python
import asyncio
from hpactor import Actor, ActorSystem, Behavior, MessageRegistry
from google.protobuf.wrappers_pb2 import StringValue

class Echo(Actor):
    def behavior(self) -> Behavior:
        return Behavior().on_request(
            StringValue, StringValue, self.echo
        )

    async def echo(self, msg: StringValue, ctx) -> StringValue:
        return StringValue(value=msg.value)

async def main():
    messages = MessageRegistry()
    messages.register(StringValue, type_tag=0x1000)
    async with ActorSystem(messages=messages) as system:
        ref = await system.spawn(Echo, name="echo")
        reply = await system.ask(ref, StringValue(value="hello"),
                                 response_type=StringValue)
        print(reply.value)

asyncio.run(main())
```

## Stability

This is an **alpha** release (Phase 1D). The API may change before 1.0. Native
remote-node participation is deferred until identity, authorization, and protocol
negotiation exist.

## License

Apache 2.0
