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

const FaultPointRegistrar kAllocatorSegmentMmapFail{
    "hpactor.allocator.segment.mmap_fail", FaultDomain::kAllocator,
    "SegmentProvider mmap returns MAP_FAILED"};

const FaultPointRegistrar kAllocatorFreelistPopCorrupt{
    "hpactor.allocator.freelist.pop.corrupt", FaultDomain::kAllocator,
    "Freelist pop returns corrupted node"};

const FaultPointRegistrar kAllocatorSlabRefillFail{
    "hpactor.allocator.slab_cache.refill_fail", FaultDomain::kAllocator,
    "SlabCache refill returns nullptr"};

const FaultPointRegistrar kAllocatorRegionTryReserveFail{
    "hpactor.allocator.region.try_reserve.fail", FaultDomain::kAllocator,
    "Memory region hard-limit rejection"};

const FaultPointRegistrar kAllocatorRegionRecordFreeSkip{
    "hpactor.allocator.region.record_free.skip", FaultDomain::kAllocator,
    "Memory region record_free silently skipped"};

const FaultPointRegistrar kActorHandlerDelay{
    "hpactor.actor.handler.delay", FaultDomain::kActor,
    "Actor message handler delay"};

// --- Actor lifecycle fault points (Phase 3 Task 3.1) ---

const FaultPointRegistrar kActorLifecycleTransitionFail{
    "hpactor.actor.lifecycle.transition.fail", FaultDomain::kActor,
    "Lifecycle state transition CAS always fails"};
const FaultPointRegistrar kActorLifecycleTransitionCorrupt{
    "hpactor.actor.lifecycle.transition.corrupt", FaultDomain::kActor,
    "Lifecycle transitions to wrong state"};
const FaultPointRegistrar kActorReceiveDrop{
    "hpactor.actor.receive.drop", FaultDomain::kActor,
    "Actor receive silently skips message"};
const FaultPointRegistrar kActorBecomeDrop{
    "hpactor.actor.become.drop", FaultDomain::kActor,
    "Behavior swap refused"};
const FaultPointRegistrar kActorOnExitDrop{
    "hpactor.actor.on_exit.drop", FaultDomain::kActor,
    "DownMsg never sent to linked actors"};
const FaultPointRegistrar kActorSpawnFail{
    "hpactor.actor.spawn.fail", FaultDomain::kActor,
    "Actor spawn pipeline failure"};
const FaultPointRegistrar kActorDrainOneCorrupt{
    "hpactor.actor.drain_one.corrupt", FaultDomain::kActor,
    "Wrong drain policy decision"};
const FaultPointRegistrar kActorCircuitBreakerFail{
    "hpactor.actor.circuit_breaker.record.fail", FaultDomain::kActor,
    "Circuit breaker always trips"};

const FaultPointRegistrar kSchedulerWorkerPause{
    "hpactor.scheduler.worker.pause", FaultDomain::kScheduler,
    "Scheduler worker pause"};

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

const FaultPointRegistrar kGossipPingDrop{
    "hpactor.gossip.ping.drop", FaultDomain::kGossip,
    "Gossip ping loss -> false suspicion"};
const FaultPointRegistrar kGossipAckDrop{
    "hpactor.gossip.ack.drop", FaultDomain::kGossip,
    "Gossip ack loss -> false suspicion cascade"};
const FaultPointRegistrar kGossipJoinDrop{
    "hpactor.gossip.join.drop", FaultDomain::kGossip,
    "Gossip join loss -> cluster formation failure"};
const FaultPointRegistrar kGossipSyncCorrupt{
    "hpactor.gossip.sync_rsp.corrupt", FaultDomain::kGossip,
    "Gossip sync response corruption"};
const FaultPointRegistrar kGossipLeaveDrop{
    "hpactor.gossip.leave.drop", FaultDomain::kGossip,
    "Gossip leave message lost"};
const FaultPointRegistrar kGossipProtocolRoundDelay{
    "hpactor.gossip.protocol_round.delay", FaultDomain::kGossip,
    "Gossip protocol round delayed"};
const FaultPointRegistrar kGossipMarkSuspiciousDrop{
    "hpactor.gossip.mark_suspicious.drop", FaultDomain::kGossip,
    "Gossip mark suspicious skipped"};
const FaultPointRegistrar kGossipMarkDeadDrop{
    "hpactor.gossip.mark_dead.drop", FaultDomain::kGossip,
    "Gossip mark dead skipped"};
const FaultPointRegistrar kGossipMergeMemberCorrupt{
    "hpactor.gossip.merge_member.corrupt", FaultDomain::kGossip,
    "Gossip merge member incarnation corruption"};
const FaultPointRegistrar kGossipPickPeersFail{
    "hpactor.gossip.pick_random_peers.fail", FaultDomain::kGossip,
    "Gossip pick random peers returns empty"};

// --- New domain registrations (Phase 1 -- infrastructure) ---

// kRpc
const FaultPointRegistrar kRpcSendDelay{
    "hpactor.rpc.send.delay", FaultDomain::kRpc,
    "RPC send delayed"};
const FaultPointRegistrar kRpcResponseDelay{
    "hpactor.rpc.response.delay", FaultDomain::kRpc,
    "RPC response delayed"};
const FaultPointRegistrar kRpcTimeoutDrop{
    "hpactor.rpc.timeout.drop", FaultDomain::kRpc,
    "RPC timeout not processed"};
const FaultPointRegistrar kRpcRetryDrop{
    "hpactor.rpc.retry.drop", FaultDomain::kRpc,
    "RPC retry never scheduled"};

// kSupervision
const FaultPointRegistrar kSupervisionRestartDrop{
    "hpactor.supervision.restart_child.drop", FaultDomain::kSupervision,
    "Supervision child restart silently skipped"};
const FaultPointRegistrar kSupervisionRestartFail{
    "hpactor.supervision.restart_child.fail", FaultDomain::kSupervision,
    "Restart count never resets, child permanently killed"};
const FaultPointRegistrar kSupervisionHandleDownDrop{
    "hpactor.supervision.handle_child_down.drop", FaultDomain::kSupervision,
    "Supervision child death notification dropped"};
const FaultPointRegistrar kSupervisionHandleDownCorrupt{
    "hpactor.supervision.handle_child_down.corrupt", FaultDomain::kSupervision,
    "Wrong supervision directive dispatched"};
const FaultPointRegistrar kSupervisionDecideRestartFail{
    "hpactor.supervision.decide_restart.fail", FaultDomain::kSupervision,
    "Decide restart always returns Stop"};
const FaultPointRegistrar kSupervisionAddChildDrop{
    "hpactor.supervision.add_child.drop", FaultDomain::kSupervision,
    "Child registration silently refused"};
const FaultPointRegistrar kSupervisionRemoveChildDrop{
    "hpactor.supervision.remove_child.drop", FaultDomain::kSupervision,
    "Stale child reference persists"};

// kDiscovery
const FaultPointRegistrar kDiscoveryHeartbeatDrop{
    "hpactor.discovery.heartbeat.drop", FaultDomain::kDiscovery,
    "Discovery heartbeat silently dropped"};
const FaultPointRegistrar kDiscoveryRegisterDrop{
    "hpactor.discovery.register.drop", FaultDomain::kDiscovery,
    "Discovery registration silently dropped"};
const FaultPointRegistrar kDiscoveryConnectFail{
    "hpactor.discovery.connect.fail", FaultDomain::kDiscovery,
    "Registrar connection failure"};
const FaultPointRegistrar kLocationCacheGetFail{
    "hpactor.location_cache.get.fail", FaultDomain::kDiscovery,
    "Location cache always misses"};
const FaultPointRegistrar kLocationCachePutDrop{
    "hpactor.location_cache.put.drop", FaultDomain::kDiscovery,
    "Location cache put silently dropped"};
const FaultPointRegistrar kLocationCacheEvictDrop{
    "hpactor.location_cache.evict.drop", FaultDomain::kDiscovery,
    "Location cache stale entry persists"};

// kTracing
const FaultPointRegistrar kTracingStartSpanDrop{
    "hpactor.tracing.start_span.drop", FaultDomain::kTracing,
    "Tracing span start silently dropped"};

// kMetrics

const FaultPointRegistrar kTransportSendCorrupt{
    "hpactor.transport.send.corrupt", FaultDomain::kTransport,
    "Transport send data corruption"};
const FaultPointRegistrar kPoolSendDrop{
    "hpactor.connection_pool.send.drop", FaultDomain::kTransport,
    "Connection pool send silently dropped"};
const FaultPointRegistrar kPoolTrySendFail{
    "hpactor.connection_pool.try_send.fail", FaultDomain::kTransport,
    "Connection pool admission denied"};
const FaultPointRegistrar kPoolReconnectDrop{
    "hpactor.connection_pool.reconnect.drop", FaultDomain::kTransport,
    "Connection pool reconnect prevented"};
const FaultPointRegistrar kPoolFlushDrop{
    "hpactor.connection_pool.flush.drop", FaultDomain::kTransport,
    "Connection pool flush silently drops"};
const FaultPointRegistrar kPoolFrameDrop{
    "hpactor.connection_pool.frame.drop", FaultDomain::kTransport,
    "Connection pool received frame dropped"};
const FaultPointRegistrar kWireframeReadDrop{
    "hpactor.wireframe.handle_read.drop", FaultDomain::kTransport,
    "Wireframe read silently dropped"};
const FaultPointRegistrar kWireframeWriteDrop{
    "hpactor.wireframe.flush_write_buffer.drop", FaultDomain::kTransport,
    "Wireframe write buffer never flushed"};
const FaultPointRegistrar kAcceptorListenFail{
    "hpactor.acceptor.listen.fail", FaultDomain::kTransport,
    "Acceptor bind/listen failure"};
const FaultPointRegistrar kAcceptorAcceptDrop{
    "hpactor.acceptor.accept.drop", FaultDomain::kTransport,
    "Accepted connection silently dropped"};

// --- Mailbox expansion (Phase 2 Task 2.1) ---

const FaultPointRegistrar kMailboxDlqPushDrop{
    "hpactor.mailbox.dlq.push.drop", FaultDomain::kMailbox,
    "Dead-letter record silently dropped"};

const FaultPointRegistrar kMailboxDedupCorrupt{
    "hpactor.mailbox.dedup.is_duplicate.corrupt", FaultDomain::kMailbox,
    "Duplicate detection returns wrong answer"};
// --- Scheduler & Timer fault points (Phase 2 Task 2.3) ---

const FaultPointRegistrar kSchedulerNotifyReadyDrop{
    "hpactor.scheduler.notify_ready.drop", FaultDomain::kScheduler,
    "Scheduler notify_ready silently dropped"};
const FaultPointRegistrar kSchedulerTryStealFail{
    "hpactor.scheduler.try_steal.fail", FaultDomain::kScheduler,
    "Scheduler try_steal returns false when work exists"};
const FaultPointRegistrar kSchedulerPopLocalFail{
    "hpactor.scheduler.pop_local.fail", FaultDomain::kScheduler,
    "Scheduler pop_local returns false for non-empty queue"};
const FaultPointRegistrar kSchedulerExecuteDispatchSkip{
    "hpactor.scheduler.execute_actor.dispatch_skip", FaultDomain::kScheduler,
    "Scheduler dequeues actor but skips dispatch"};
const FaultPointRegistrar kTimerScheduleFail{
    "hpactor.timing_wheel.schedule.fail", FaultDomain::kTimer,
    "Timing wheel schedule silently dropped"};
const FaultPointRegistrar kTimerAdvanceSkip{
    "hpactor.timing_wheel.advance.skip", FaultDomain::kTimer,
    "Timing wheel advance skips expired timers"};
const FaultPointRegistrar kTimerCancelFail{
    "hpactor.timing_wheel.cancel.fail", FaultDomain::kTimer,
    "Timing wheel cancel silently ignored"};

// --- Phase 4: Tier 3 observability path ---

const FaultPointRegistrar kTracingFinishSpanDrop{
    "hpactor.tracing.finish_span.drop", FaultDomain::kTracing,
    "Tracing span finish silently dropped"};
const FaultPointRegistrar kTracingInjectContextCorrupt{
    "hpactor.tracing.inject_context.corrupt", FaultDomain::kTracing,
    "Tracing context injection corrupted"};
const FaultPointRegistrar kTracingDrainOnceFail{
    "hpactor.tracing.drain_once.fail", FaultDomain::kTracing,
    "Tracing drain once failed"};
const FaultPointRegistrar kTracingForceFlushFail{
    "hpactor.tracing.force_flush.fail", FaultDomain::kTracing,
    "Tracing force flush failed"};
const FaultPointRegistrar kTracingStartFail{
    "hpactor.tracing.start.fail", FaultDomain::kTracing,
    "Tracing start failed (drain thread not spawned)"};
const FaultPointRegistrar kMetricsAggregatorCorrupt{
    "hpactor.metrics.aggregator.on_event.corrupt", FaultDomain::kMetrics,
    "Metrics aggregator event processing corrupted"};
const FaultPointRegistrar kCliRunOnceFail{
    "hpactor.cli.actor.run_once.fail", FaultDomain::kActor,
    "CLI daemon run_once exits prematurely"};
const FaultPointRegistrar kCliExecuteTokensCorrupt{
    "hpactor.cli.execute_tokens.corrupt", FaultDomain::kActor,
    "CLI command execution silently skipped"};
const FaultPointRegistrar kCliLexerCorrupt{
    "hpactor.cli.lexer.tokenize.corrupt", FaultDomain::kActor,
    "CLI lexer returns empty tokens"};
const FaultPointRegistrar kConfigParseFail{
    "hpactor.config.parse.fail", FaultDomain::kConfig,
    "TOML parser returns failure"};

} // anonymous namespace
} // namespace hpactor::fault
