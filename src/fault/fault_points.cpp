// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/fault/fault_point.hpp>

namespace hpactor::fault {
namespace {

const FaultPointRegistrar kMailboxEnqueueFail{
    "hpactor.mailbox.enqueue.fail", FaultDomain::kMailbox,
    "Mailbox enqueue fails with capacity error"};

const FaultPointRegistrar kMailboxDequeueDrop{
    "hpactor.mailbox.dequeue.drop", FaultDomain::kMailbox,
    "Silent message discard on dequeue"};

const FaultPointRegistrar kAllocatorOOM{
    "hpactor.allocator.oom", FaultDomain::kAllocator,
    "Allocator out-of-memory failure"};

const FaultPointRegistrar kActorHandlerDelay{
    "hpactor.actor.handler.delay", FaultDomain::kActor,
    "Actor message handler delay"};

const FaultPointRegistrar kSchedulerWorkerPause{
    "hpactor.scheduler.worker.pause", FaultDomain::kScheduler,
    "Scheduler worker pause"};

const FaultPointRegistrar kSchedulerWorkerPanic{
    "hpactor.scheduler.worker.panic", FaultDomain::kScheduler,
    "Scheduler worker crash"};

const FaultPointRegistrar kTransportSendDrop{
    "hpactor.transport.send.drop", FaultDomain::kTransport,
    "Transport send silently dropped"};

const FaultPointRegistrar kTransportSendDelay{
    "hpactor.transport.send.delay", FaultDomain::kTransport,
    "Transport send delayed"};

const FaultPointRegistrar kTransportRecvDrop{
    "hpactor.transport.recv.drop", FaultDomain::kTransport,
    "Transport receive silently dropped"};

const FaultPointRegistrar kTransportRecvCorrupt{
    "hpactor.transport.recv.corrupt", FaultDomain::kTransport,
    "Transport receive data corruption"};

const FaultPointRegistrar kTransportConnectionReset{
    "hpactor.transport.connection.reset", FaultDomain::kTransport,
    "Transport connection reset"};

const FaultPointRegistrar kGossipPacketLoss{
    "hpactor.gossip.packet.loss", FaultDomain::kGossip,
    "Gossip packet loss"};

} // anonymous namespace
} // namespace hpactor::fault
