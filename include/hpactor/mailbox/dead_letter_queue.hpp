// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once
#include <hpactor/msg/dead_letter_record.hpp>
// Compatibility shim: DeadLetterReason, DeadLetterSource, DeadLetterRecord
// moved to msg/ subsystem. DeadLetterQueue class still available via this
// include.
// TODO: update consumers to include <hpactor/msg/dead_letter_record.hpp>
// directly for the data types, or this header for the DeadLetterQueue class.
