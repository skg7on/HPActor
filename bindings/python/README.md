# hpactor

Asyncio-first Python language binding for the **HPActor** C++20 actor runtime.

## Supported platforms

| Platform | Architectures | Wheel tag |
|----------|--------------|-----------|
| Linux (manylinux_2_28) | x86_64, aarch64 | `manylinux_2_28_*` |
| macOS 12.0+ | x86_64, arm64 | `macosx_12_0_*` |

**Requirements:** CPython ≥ 3.11, `protobuf>=7.35.0,<8`.

No threads are created at import time. An `ActorSystem` starts the runtime
thread and the dedicated asyncio event loop on context-manager entry.

## Quick start

```python
import asyncio
import hpactor
from myapp.messages_pb2 import Ping, Pong


@hpactor.actor("echo")
class Echo(hpactor.Actor):
    def behavior(self) -> hpactor.Behavior:
        return hpactor.Behavior().on_request(Ping, Pong, self.on_ping)

    async def on_ping(self, msg: Ping, ctx: hpactor.ActorContext) -> Pong:
        return Pong(text=msg.text)


async def main() -> None:
    messages = hpactor.MessageRegistry()
    messages.register(Ping, type_tag=0x1000)
    messages.register(Pong, type_tag=0x1001)

    async with hpactor.ActorSystem(messages=messages) as system:
        echo = await system.spawn(Echo, name="echo")
        pong = await system.ask(echo, Ping(text="hello"), response_type=Pong)
        assert pong.text == "hello"


asyncio.run(main())
```

## Stability

**Alpha.** This is the Phase 1D in-process binding. Windows, musllinux, PyPy,
and free-threaded CPython are not yet supported. Native remote-node
participation requires future cluster-identity and protocol-negotiation work.

## Source builds

Pass `HPACTOR_WHEEL_DEPS_PREFIX` to the CMake build pointing at a prefix that
contains PIC static libraries for OpenSSL 3.5.5, Abseil 20260107.1, and
protobuf 35.0. See `bindings/python/packaging/` for the hermetic build scripts.
