// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once
#include <hpactor/msg/frame.hpp>
// Compatibility shim: WireFrame moved to msg/ subsystem.
// to_proto/from_proto address conversion functions remain in the msg/ header
// for now (they are defined in src/net/frame.cpp).
// TODO: update consumers to include <hpactor/msg/frame.hpp> directly.
