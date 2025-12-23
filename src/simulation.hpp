#pragma once

#include "determinism.hpp"
#include "event.hpp"
#include "event_wire.hpp"
#include "log.hpp"
#include "lp.hpp"
#include "migration.hpp"
#include "optimistic_migration.hpp"
#include "transport.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace warpsim
{
    inline constexpr TimeStamp prev_stamp(TimeStamp ts) noexcept
    {
        if (ts.sequence > 0)
        {
            return TimeStamp{ts.time, ts.sequence - 1};
        }
        if (ts.time == 0)
        {
            return TimeStamp{0, 0};
        }
        return stamp_at_max_sequence(ts.time - 1);
    }
    struct SimulationConfig
    {
        enum class GvtMode : std::uint8_t
        {
            // Conservative inflight tracking; for MPI this uses per-message Ack to drain inflight.
            AckInflight = 0,

            // Algorithmic GVT using colored messages + counters (no transport-level Ack required).
            ColoredCounters = 1,
        };

        RankId rank = 0;
        std::size_t maxSnapshotsPerLP = 2048;

        GvtMode gvtMode = GvtMode::AckInflight;

        // Optional mapping from destination LPId to destination RankId.
        // If not set, all traffic is assumed local to `rank`.
        std::function<RankId(LPId)> lpToRank;

        // Optional global reduction for conservative GVT.
        // Given this rank's local minimum, returns the global minimum.
        // (e.g., MPI_Allreduce with a lexicographic min on {time, sequence}).
        std::function<TimeStamp(TimeStamp)> gvtReduceMin;

        // Optional global reduction for signed sums (used by algorithmic GVT).
        // (e.g., MPI_Allreduce with MPI_SUM)
        std::function<std::int64_t(std::int64_t)> gvtReduceSum;

        // Optional global reduction to determine whether ANY rank still has work.
        // Used to avoid premature local termination in multi-rank runs.
        // Given `localHasWork`, returns `true` if any rank has work.
        std::function<bool(bool)> anyRankHasWork;

        // Optional directory routing for optimistic entity migration.
        // If set and an event has `target != 0`, Simulation::send routes it to the
        // directory LP for that entity (unless the sender is already that directory).
        // The directory is responsible for forwarding to the correct owner at ev.ts.
        std::function<LPId(EntityId)> directoryForEntity;

        // Optional: if set (non-zero) and directory routing is enabled, the kernel
        // will periodically send a control event to each observed directory LP to
        // indicate the latest GVT so the directory can fossil-collect its ownership
        // history safely.
        std::uint32_t directoryFossilCollectKind = 0;

        // How often to invoke global collectives (GVT reduction + any-rank-work).
        // 1 means every main-loop iteration (maximally responsive, slowest).
        // Larger values reduce collective overhead but delay GVT advancement and termination.
        std::uint64_t collectivePeriodIters = 1;

        // Global deterministic seed for model RNG.
        // Models should derive random values from this seed + LPId + (Event.src, Event.uid) so results
        // are deterministic across rollback and MPI rank remaps.
        std::uint64_t seed = 1;

        // Logging (disabled by default).
        LogLevel logLevel = LogLevel::Off;

        // Optional sink for committed (GVT-safe) side effects.
        //
        // If set, the kernel will deliver committed payloads in deterministic order once they are
        // safe (event timestamp < global virtual time). This prevents duplicate side effects when
        // optimistic execution rolls back.
        std::function<void(RankId, LPId, TimeStamp, const Payload &)> committedSink;

        // Optional sink for *events* once they become irrevocable (ts < GVT).
        //
        // This is intended for determinism/correctness testing: if two runs are equivalent,
        // the sequence of committed events should match exactly.
        //
        // Notes:
        // - This fires only when an event record is fossil-collected (ts < global GVT).
        // - Anti-messages are also events; callers may choose to ignore `ev.isAnti`.
        std::function<void(RankId, LPId, const Event &)> committedEventSink;

        // Optional: determinism oracle.
        //
        // If enabled, the kernel computes a commutative digest over (committed events, committed output)
        // using DeterminismAccumulator. This is intended for cross-rank-count determinism checks.
        //
        // For MPI runs, callers can provide u64 reductions (sum/xor) to obtain a global digest.
        bool enableDeterminismOracle = false;
        std::function<std::uint64_t(std::uint64_t)> reduceU64Sum;
        std::function<std::uint64_t(std::uint64_t)> reduceU64Xor;
        std::optional<DeterminismDigest> expectedDeterminismDigest;

        // Optional: deterministic priority between event classes at the same timestamp.
        //
        // If set, auto-assigned sequence numbers are constructed so that events for which
        // `isControlEvent(ev)` is true will always order before other events at the same time.
        // This is useful for migration control events like update-owner/install.
        std::function<bool(const Event &)> isControlEvent;

        // Optional: install-before-use buffering for optimistic migration.
        //
        // If enabled, and a model throws "World: unknown EntityId=..." while processing a
        // targeted event, the kernel will defer that event until an install event for the
        // same entity is successfully processed on the same LP.
        //
        // This is a pragmatic guardrail for optimistic migration setups where an entity can
        // arrive (install) after other targeted events due to speculative forwarding.
        bool enableInstallBeforeUseBuffering = false;
        std::uint32_t installEventKind = optimistic_migration::DefaultInstallKind;

        // Enable extra runtime invariant checks (throws on violation).
#if defined(WARPSIM_ENABLE_INVARIANT_CHECKS_DEFAULT)
        bool enableInvariantChecks = (WARPSIM_ENABLE_INVARIANT_CHECKS_DEFAULT != 0);
#else
        bool enableInvariantChecks = false;
#endif
    };

    // Minimal optimistic Time Warp (single-rank core) with explicit transport hooks.
    // Multi-rank is supported architecturally via ITransport + WireMessage, but the
    // example transport is in-process.
    class Simulation final : public IEventSink
    {
    public:
        struct Stats
        {
            TimeStamp gvt{0, 0};
            std::size_t pending = 0;
            std::size_t inflight = 0;
            std::size_t cancelled = 0;
            std::size_t processed = 0;
            std::size_t sentLog = 0;

            // Monotonic counters (not affected by fossil collection).
            std::uint64_t totalProcessed = 0;
            std::uint64_t rollbacksTotal = 0;
            std::uint64_t rollbacksStraggler = 0;
            std::uint64_t rollbacksAnti = 0;
            std::uint64_t rollbackUndoneEvents = 0;

            std::uint64_t sentEvents = 0;
            std::uint64_t sentAnti = 0;
            std::uint64_t receivedEvents = 0;
            std::uint64_t receivedAnti = 0;

            std::uint64_t antiCancelledPending = 0;
            std::uint64_t antiCancelledProcessed = 0;
        };

        explicit Simulation(SimulationConfig cfg, std::shared_ptr<ITransport> transport)
            : m_cfg(cfg), m_transport(std::move(transport))
        {
            if (!m_transport)
            {
                throw std::runtime_error("Simulation requires a transport");
            }

            Logger::instance().set_level(m_cfg.logLevel);
        }

        void add_lp(std::unique_ptr<ILogicalProcess> lp)
        {
            if (!lp)
            {
                throw std::runtime_error("add_lp: null");
            }
            const auto id = lp->id();
            if (m_lps.count(id) != 0)
            {
                throw std::runtime_error("add_lp: duplicate LPId");
            }

            LPContext ctx;
            ctx.lp = std::move(lp);
            ctx.vt = TimeStamp{0, 0};
            ctx.nextSeqNormal = 1;
            ctx.nextSeqControl = 1;

            m_lps.emplace(id, std::move(ctx));

            if (!m_systemSrcSet)
            {
                m_systemSrc = id;
                m_systemSrcSet = true;
            }
        }

        // Schedule an initial event locally.
        void schedule(Event ev)
        {
            enqueue_(std::move(ev));
        }

        // Drain transport -> enqueue messages.
        void poll_transport() { (void)poll_transport_count(); }

        // Drain transport -> enqueue messages and return the number of wire messages consumed.
        std::size_t poll_transport_count()
        {
            std::size_t polled = 0;
            while (true)
            {
                auto msg = m_transport->poll();
                if (!msg)
                {
                    return polled;
                }

                ++polled;

                if (msg->kind == MessageKind::Event)
                {
                    auto ev = decode_event(std::span<const std::byte>(msg->bytes.data(), msg->bytes.size()));
                    if (ev.isAnti)
                    {
                        ++m_receivedAnti;
                    }
                    else
                    {
                        ++m_receivedEvents;
                    }

                    if (m_cfg.gvtMode == SimulationConfig::GvtMode::ColoredCounters)
                    {
                        ++m_gvtRecvByColor[static_cast<std::size_t>(ev.gvtColor & 1u)];
                    }

                    // Acknowledge delivery to the sender rank so the sender can
                    // conservatively account for in-flight messages (for GVT and termination).
                    if (m_cfg.gvtMode == SimulationConfig::GvtMode::AckInflight)
                    {
                        WireWriter w;
                        w.write_u32(ev.src);
                        w.write_u64(ev.uid);
                        w.write_u8(static_cast<std::uint8_t>(ev.isAnti ? 1 : 0));
                        WireMessage ack;
                        ack.kind = MessageKind::Ack;
                        ack.srcRank = m_cfg.rank;
                        ack.dstRank = msg->srcRank;
                        ack.bytes = w.take();
                        m_transport->send(std::move(ack));
                    }

                    // For single-rank/in-proc use, we may see our own messages. Keep
                    // the legacy inflight-clear path for that case.
                    if (m_cfg.gvtMode == SimulationConfig::GvtMode::AckInflight)
                    {
                        track_inflight_recv_(ev);
                    }
                    enqueue_(std::move(ev));
                }
                else if (msg->kind == MessageKind::Migration)
                {
                    auto mig = decode_migration(std::span<const std::byte>(msg->bytes.data(), msg->bytes.size()));
                    apply_migration_(mig);
                }
                else if (msg->kind == MessageKind::Ack)
                {
                    if (m_cfg.gvtMode != SimulationConfig::GvtMode::AckInflight)
                    {
                        continue;
                    }
                    WireReader r(std::span<const std::byte>(msg->bytes.data(), msg->bytes.size()));
                    const auto src = static_cast<LPId>(r.read_u32());
                    const auto uid = static_cast<EventUid>(r.read_u64());
                    const bool isAnti = (r.read_u8() != 0);
                    clear_inflight_key_(InflightKey{src, uid, isAnti});
                }
            }
        }

        // Runs until no pending events and no incoming messages.
        void run()
        {
            start_if_needed_();

            const auto finalize_at_end = [&]()
            {
                // Once the simulation has globally terminated, no further events can arrive.
                // Flush any remaining committed output and release history.
                prune_to_gvt_(max_stamp_());
                apply_ready_migrations_();

                if (m_cfg.enableDeterminismOracle)
                {
                    DeterminismDigest d = m_determinism.digest();
                    if (m_cfg.reduceU64Sum)
                    {
                        d.committedEventSum = m_cfg.reduceU64Sum(d.committedEventSum);
                        d.committedOutputSum = m_cfg.reduceU64Sum(d.committedOutputSum);
                    }
                    if (m_cfg.reduceU64Xor)
                    {
                        d.committedEventXor = m_cfg.reduceU64Xor(d.committedEventXor);
                        d.committedOutputXor = m_cfg.reduceU64Xor(d.committedOutputXor);
                    }

                    if (m_cfg.expectedDeterminismDigest.has_value() && !(d == *m_cfg.expectedDeterminismDigest))
                    {
                        throw std::runtime_error("Determinism oracle failed: digest mismatch");
                    }
                }

                if (m_cfg.enableInstallBeforeUseBuffering && !m_deferredTargeted.empty())
                {
                    throw std::runtime_error("Install-before-use buffering: simulation ended with deferred targeted events still pending (missing install?)");
                }
            };

            const std::uint64_t period = (m_cfg.collectivePeriodIters == 0) ? 1 : m_cfg.collectivePeriodIters;
            std::uint64_t iters = 0;

            while (true)
            {
                poll_transport();

                if (!m_pending.empty())
                {
                    auto ev = pop_next_();
                    if (ev)
                    {
                        dispatch_(*ev);
                    }
                }

                ++iters;
                const bool doCollectives = (iters % period) == 0;

                // Fossil collection runs every loop, but only advances GVT via global reduction
                // during collective epochs.
                fossil_collect_(doCollectives);

                // For multi-rank termination, only perform the global reduction on epochs.
                // Between epochs we keep looping; this bounds collective overhead.
                bool localHasWork = (!m_pending.empty()) || m_transport->has_pending();
                if (m_cfg.gvtMode == SimulationConfig::GvtMode::AckInflight)
                {
                    localHasWork = localHasWork || (!m_inflightByUid.empty());
                }
                else
                {
                    // In colored-counters mode we must conservatively account for in-flight
                    // messages without transport-level ACKs.
                    if (doCollectives && m_cfg.gvtReduceSum)
                    {
                        const std::int64_t sent = static_cast<std::int64_t>(m_gvtSentByColor[0] + m_gvtSentByColor[1]);
                        const std::int64_t recv = static_cast<std::int64_t>(m_gvtRecvByColor[0] + m_gvtRecvByColor[1]);
                        const std::int64_t globalInFlight = m_cfg.gvtReduceSum(sent - recv);
                        localHasWork = localHasWork || (globalInFlight != 0) || m_gvtRedPhase;
                    }
                    else if (m_cfg.anyRankHasWork)
                    {
                        // Between epochs we keep looping.
                        localHasWork = true;
                    }
                }
                if (m_cfg.anyRankHasWork)
                {
                    if (!doCollectives)
                    {
                        continue;
                    }
                    if (!m_cfg.anyRankHasWork(localHasWork))
                    {
                        finalize_at_end();
                        return;
                    }
                    continue;
                }

                if (!localHasWork)
                {
                    finalize_at_end();
                    return;
                }
            }
        }

        // Step the simulation by at most one dispatched event.
        //
        // Useful for explicit rollback/determinism demonstrations and unit tests.
        // Returns true if any progress was made (transport message consumed or event dispatched).
        bool run_one(bool doCollectives = false)
        {
            start_if_needed_();

            bool progressed = (poll_transport_count() != 0);

            if (!m_pending.empty())
            {
                auto ev = pop_next_();
                if (ev)
                {
                    dispatch_(*ev);
                    progressed = true;
                }
            }

            fossil_collect_(doCollectives);
            return progressed;
        }

        Stats stats() const
        {
            Stats out;
            out.gvt = m_lastGvt;
            out.pending = m_pending.size();
            out.inflight = m_inflightByUid.size();
            out.cancelled = m_cancelledByUid.size();

            std::size_t processed = 0;
            std::size_t sent = 0;
            for (const auto &[id, ctx] : m_lps)
            {
                (void)id;
                processed += ctx.processed.size();
                sent += ctx.sentLog.size();
            }
            out.processed = processed;
            out.sentLog = sent;

            out.totalProcessed = m_totalProcessed;
            out.rollbacksTotal = m_rollbacksTotal;
            out.rollbacksStraggler = m_rollbacksStraggler;
            out.rollbacksAnti = m_rollbacksAnti;
            out.rollbackUndoneEvents = m_rollbackUndoneEvents;
            out.sentEvents = m_sentEvents;
            out.sentAnti = m_sentAnti;
            out.receivedEvents = m_receivedEvents;
            out.receivedAnti = m_receivedAnti;

            out.antiCancelledPending = m_antiCancelledPending;
            out.antiCancelledProcessed = m_antiCancelledProcessed;
            return out;
        }

        // IEventSink
        void send(Event ev) override
        {
            validate_invariants_("send:entry");

            // Assign uid if caller didn't.
            if (ev.uid == 0)
            {
                // Anti-messages cancel by (src, uid), so uid must be unique per-src.
                // Keep it deterministic regardless of MPI rank mapping.
                auto &ctx = ctx_(ev.src);
                const std::uint64_t local = ctx.nextUid++;
                if (local == 0)
                {
                    throw std::runtime_error("Event UID counter overflow for LPId (exhausted 2^64-1 UIDs)");
                }
                ev.uid = static_cast<EventUid>(local);
            }

            // Auto-assign sequence if caller didn't set it.
            if (ev.ts.sequence == 0)
            {
                auto &ctx = ctx_(ev.src);

                // Default behavior: legacy monotonic per-LP sequence (preserves existing tests).
                if (!m_cfg.isControlEvent)
                {
                    ev.ts.sequence = ctx.nextSeqNormal++;
                }
                else
                {
                    const bool isControl = m_cfg.isControlEvent(ev);

                    // Encode priority in the top bit of the sequence: control=0, normal=1.
                    // This makes control events order before normal events at the same timestamp,
                    // regardless of send order.
                    if (isControl)
                    {
                        const std::uint64_t local = ctx.nextSeqControl++;
                        if (local == 0)
                        {
                            throw std::runtime_error("Event sequence counter overflow for control events (exhausted 2^64-1 sequences)");
                        }
                        ev.ts.sequence = local;
                    }
                    else
                    {
                        const std::uint64_t local = ctx.nextSeqNormal++;
                        if (local == 0)
                        {
                            throw std::runtime_error("Event sequence counter overflow for normal events (exhausted 2^64-1 sequences)");
                        }
                        ev.ts.sequence = (1ULL << 63) | local;
                    }
                }
            }

            // Entity routing indirection: if a migration has established an owner for this
            // entity, route to that owner regardless of the caller-provided dst.
            if (ev.target != 0)
            {
                constexpr LPId UnsetDst = std::numeric_limits<LPId>::max();

                if (m_cfg.directoryForEntity)
                {
                    const LPId dir = m_cfg.directoryForEntity(ev.target);
                    if (dir == 0)
                    {
                        throw std::runtime_error("send: directoryForEntity returned LPId=0 for target entity");
                    }
                    m_directoryLps.insert(dir);
                    // Avoid re-routing forwarded events emitted by the directory itself.
                    if (ev.src != dir)
                    {
                        const LPId prevDst = ev.dst;
                        ev.dst = dir;

                        Logger::instance().logf(LogLevel::Trace, m_cfg.rank, ev.src, ev.ts,
                                                "route targeted event via directory: entity=%llu kind=%u prev_dst=%u dir=%u uid=%llu",
                                                static_cast<unsigned long long>(ev.target),
                                                static_cast<unsigned>(ev.payload.kind),
                                                static_cast<unsigned>(prevDst),
                                                static_cast<unsigned>(dir),
                                                static_cast<unsigned long long>(ev.uid));
                    }
                }
                else
                {
                    auto it = m_entityOwner.find(ev.target);
                    if (it != m_entityOwner.end())
                    {
                        ev.dst = it->second;

                        Logger::instance().logf(LogLevel::Trace, m_cfg.rank, ev.src, ev.ts,
                                                "route targeted event via owner map: entity=%llu kind=%u owner=%u uid=%llu",
                                                static_cast<unsigned long long>(ev.target),
                                                static_cast<unsigned>(ev.payload.kind),
                                                static_cast<unsigned>(ev.dst),
                                                static_cast<unsigned long long>(ev.uid));
                    }
                }

                // Safety: targeted events must be routable.
                // Model helpers like modeling::send_targeted intentionally use an "unset" dst sentinel;
                // if the kernel cannot resolve an owner/directory, letting the event proceed would
                // silently drop/misroute work.
                if (ev.dst == UnsetDst)
                {
                    throw std::runtime_error("send: event has target!=0 but dst is unset after routing (enable directoryForEntity or ensure an owner is established)");
                }
            }

            const RankId dstRank = m_cfg.lpToRank ? m_cfg.lpToRank(ev.dst) : m_cfg.rank;

            // Short-circuit local deliveries: enqueue directly instead of sending through
            // the transport. This avoids MPI self-send/local-send quirks and keeps local
            // causality within the rank.
            if (dstRank == m_cfg.rank)
            {
                if (m_cfg.gvtMode != SimulationConfig::GvtMode::AckInflight)
                {
                    ev.gvtColor = m_gvtColor;
                }

                if (ev.isAnti)
                {
                    ++m_sentAnti;
                }
                else
                {
                    ++m_sentEvents;
                }
                enqueue_(std::move(ev));

                validate_invariants_("send:local_enqueue");

                // Log for potential rollback -> anti-messages.
                if (!ev.isAnti)
                {
                    auto &ctx = ctx_(ev.src);
                    const TimeStamp sendTime = ctx.inEvent ? ctx.currentExecTs : TimeStamp{0, 0};
                    ctx.sentLog.push_back(SentRecord{sendTime, ev.ts, ev.dst, ev.uid});
                }
                return;
            }

            WireMessage msg;
            msg.kind = MessageKind::Event;
            msg.srcRank = m_cfg.rank;
            msg.dstRank = dstRank;
            msg.bytes = encode_event(ev);

            if (m_cfg.gvtMode == SimulationConfig::GvtMode::AckInflight)
            {
                track_inflight_send_(ev);
            }
            else
            {
                // Colored-counters GVT: stamp events with current send color.
                ev.gvtColor = m_gvtColor;
                msg.bytes = encode_event(ev);
                ++m_gvtSentByColor[static_cast<std::size_t>(ev.gvtColor & 1u)];
                if (ev.gvtColor != m_gvtWhiteColor)
                {
                    m_gvtMinRedSentTs = min_stamp_(m_gvtMinRedSentTs, ev.ts);
                }
            }

            if (ev.isAnti)
            {
                ++m_sentAnti;
            }
            else
            {
                ++m_sentEvents;
            }
            m_transport->send(std::move(msg));

            // Log for potential rollback -> anti-messages.
            if (!ev.isAnti)
            {
                auto &ctx = ctx_(ev.src);
                const TimeStamp sendTime = ctx.inEvent ? ctx.currentExecTs : TimeStamp{0, 0};
                ctx.sentLog.push_back(SentRecord{sendTime, ev.ts, ev.dst, ev.uid});
            }

            validate_invariants_("send:exit");
        }

    private:
        void invariant_or_throw_(bool ok, const char *msg) const
        {
            if (!m_cfg.enableInvariantChecks)
            {
                return;
            }
            if (!ok)
            {
                throw std::runtime_error(msg);
            }
        }

        void validate_invariants_(const char *where) const
        {
            if (!m_cfg.enableInvariantChecks)
            {
                return;
            }

            // Per-LP invariants.
            for (const auto &[id, ctx] : m_lps)
            {
                (void)id;
                invariant_or_throw_(ctx.nextSeqNormal != 0, "invariant: nextSeqNormal must be non-zero");
                invariant_or_throw_(ctx.nextSeqControl != 0, "invariant: nextSeqControl must be non-zero");
                invariant_or_throw_(ctx.nextUid != 0, "invariant: nextUid must be non-zero");

                // Processed records must be in non-decreasing timestamp order.
                for (std::size_t i = 1; i < ctx.processed.size(); ++i)
                {
                    invariant_or_throw_(!(ctx.processed[i].ev.ts < ctx.processed[i - 1].ev.ts), "invariant: processed not sorted by timestamp");
                }

                // Virtual time must not be behind the latest processed record.
                if (!ctx.processed.empty())
                {
                    invariant_or_throw_(!(ctx.vt < ctx.processed.back().ev.ts), "invariant: vt behind last processed timestamp");
                }

                if (ctx.inEvent)
                {
                    invariant_or_throw_(ctx.currentExecTs == ctx.vt, "invariant: inEvent implies currentExecTs==vt");
                }
            }

            // Inflight bookkeeping must match multiset (AckInflight mode).
            if (m_cfg.gvtMode == SimulationConfig::GvtMode::AckInflight)
            {
                invariant_or_throw_(m_inflightByUid.size() == m_inflightTs.size(), "invariant: inflightByUid size != inflightTs size");
                std::multiset<TimeStamp> derived;
                for (const auto &kv : m_inflightByUid)
                {
                    derived.insert(kv.second);
                }
                invariant_or_throw_(derived.size() == m_inflightTs.size(), "invariant: derived inflight multiset size mismatch");
                invariant_or_throw_(std::equal(derived.begin(), derived.end(), m_inflightTs.begin(), m_inflightTs.end()),
                                    "invariant: inflightTs does not match inflightByUid timestamps");
            }

            // Pending queue ordering: verify the priority_queue would pop in non-decreasing order
            // (skipping cancelled tombstones), using the same comparison semantics.
            {
                auto pq = m_pending;
                std::optional<Event> prev;
                while (!pq.empty())
                {
                    Event ev = pq.top();
                    pq.pop();

                    if (!ev.isAnti && m_cancelledByUid.count(UidKey{ev.src, ev.uid}) != 0)
                    {
                        continue;
                    }

                    if (prev)
                    {
                        // Must be non-decreasing in (ts, src, uid).
                        invariant_or_throw_(!((ev.ts < prev->ts) ||
                                              ((ev.ts == prev->ts) && (ev.src < prev->src)) ||
                                              ((ev.ts == prev->ts) && (ev.src == prev->src) && (ev.uid < prev->uid))),
                                            "invariant: pending queue pops out of order");
                    }
                    prev = ev;
                }
            }

            (void)where;
        }

        struct CommittedOutput
        {
            TimeStamp ts;
            Payload payload;
        };

        struct CommittedEventFlushItem
        {
            TimeStamp ts;
            LPId lp = 0;
            Event ev;
        };

        struct CommittedFlushItem
        {
            TimeStamp ts;
            LPId lp = 0;
            LPId eventSrc = 0;
            EventUid eventUid = 0;
            std::uint32_t index = 0;
            Payload payload;
        };

        struct InflightKey
        {
            LPId src = 0;
            EventUid uid = 0;
            bool isAnti = false;

            bool operator==(const InflightKey &o) const noexcept
            {
                return src == o.src && uid == o.uid && isAnti == o.isAnti;
            }
        };

        struct InflightKeyHash
        {
            std::size_t operator()(const InflightKey &k) const noexcept
            {
                const std::uint64_t x = (static_cast<std::uint64_t>(k.src) << 33) ^
                                        (static_cast<std::uint64_t>(k.uid) << 1) ^
                                        (k.isAnti ? 1ULL : 0ULL);
                return static_cast<std::size_t>(x ^ (x >> 33) ^ (x >> 17));
            }
        };

        struct UidKey
        {
            LPId src = 0;
            EventUid uid = 0;

            bool operator==(const UidKey &o) const noexcept { return src == o.src && uid == o.uid; }
        };

        struct UidKeyHash
        {
            std::size_t operator()(const UidKey &k) const noexcept
            {
                const std::uint64_t x = (static_cast<std::uint64_t>(k.src) << 32) ^ static_cast<std::uint64_t>(k.uid);
                return static_cast<std::size_t>(x ^ (x >> 33) ^ (x >> 17));
            }
        };

        struct SentRecord
        {
            // Timestamp of the event being executed when this message was sent.
            // Rollback cancels messages based on this value.
            TimeStamp sendTime;

            // Timestamp of the message itself (its delivery timestamp at the receiver).
            TimeStamp msgTime;

            LPId dst;
            EventUid uid;
        };

        struct ProcessedRecord
        {
            Event ev;

            // Rollback-able kernel metadata.
            // This is separate from Event.uid (which must never be reused).
            std::uint64_t preNextSeqNormal = 1;
            std::uint64_t preNextSeqControl = 1;

            bool hasLpSnapshot = false;
            ByteBuffer preLpState;
            std::unordered_map<EntityId, ByteBuffer> preEntityState;

            // Committed side-effects recorded during this event execution.
            // These are only emitted once the record is fossil-collected (ts < GVT).
            std::vector<CommittedOutput> committed;

            // Anti-message bookkeeping (rollbackable).
            // If ev.isAnti is true, we record the prior state of the cancellation map for (ev.src, ev.uid)
            // so rollback can restore it.
            bool cancelHadEntry = false;
            TimeStamp cancelPrevTs{0, 0};
        };

        struct LPContext
        {
            std::unique_ptr<ILogicalProcess> lp;
            TimeStamp vt{0, 0};

            bool inEvent = false;
            TimeStamp currentExecTs{0, 0};

            std::deque<ProcessedRecord> processed;
            std::deque<SentRecord> sentLog;

            std::uint64_t nextSeqNormal = 1;
            std::uint64_t nextSeqControl = 1;
            std::uint64_t nextUid = 1;
        };

        struct PendingOrder
        {
            bool operator()(const Event &a, const Event &b) const
            {
                // priority_queue is max-heap, so invert
                if (b.ts < a.ts)
                {
                    return true;
                }
                if (a.ts < b.ts)
                {
                    return false;
                }
                if (a.src != b.src)
                {
                    return b.src < a.src;
                }
                return b.uid < a.uid;
            }
        };

        LPContext &ctx_(LPId id)
        {
            auto it = m_lps.find(id);
            if (it == m_lps.end())
            {
                throw std::runtime_error("Unknown LPId");
            }
            return it->second;
        }

        void enqueue_(Event ev)
        {
            m_pending.push(std::move(ev));
        }

        static bool is_missing_entity_error_(const std::exception &e) noexcept
        {
            // World throws: "World: unknown EntityId=...".
            // Treat only this as an install-before-use buffering trigger.
            const std::string_view msg(e.what());
            return msg.rfind("World: unknown EntityId=", 0) == 0;
        }

        static std::uint64_t deferred_key_(LPId lp, EntityId entity) noexcept
        {
            return (static_cast<std::uint64_t>(lp) << 32) ^ static_cast<std::uint64_t>(entity);
        }

        void defer_targeted_event_(const Event &ev)
        {
            auto &q = m_deferredTargeted[deferred_key_(ev.dst, ev.target)];
            q.push_back(ev);
        }

        void release_deferred_for_(LPId lp, EntityId entity)
        {
            const std::uint64_t key = deferred_key_(lp, entity);
            auto it = m_deferredTargeted.find(key);
            if (it == m_deferredTargeted.end())
            {
                return;
            }

            auto &q = it->second;
            std::sort(q.begin(), q.end(), EventTimeOrder{});
            for (auto &ev : q)
            {
                enqueue_(std::move(ev));
            }
            m_deferredTargeted.erase(it);
        }

        static constexpr TimeStamp max_stamp_() noexcept
        {
            return TimeStamp{std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()};
        }

        static constexpr TimeStamp min_stamp_(TimeStamp a, TimeStamp b) noexcept
        {
            return (b < a) ? b : a;
        }

        void track_inflight_send_(const Event &ev)
        {
            auto [it, inserted] = m_inflightByUid.emplace(InflightKey{ev.src, ev.uid, ev.isAnti}, ev.ts);
            if (inserted)
            {
                m_inflightTs.insert(ev.ts);
            }
        }

        void track_inflight_recv_(const Event &ev)
        {
            auto it = m_inflightByUid.find(InflightKey{ev.src, ev.uid, ev.isAnti});
            if (it == m_inflightByUid.end())
            {
                return;
            }

            auto msIt = m_inflightTs.find(it->second);
            if (msIt != m_inflightTs.end())
            {
                m_inflightTs.erase(msIt);
            }
            m_inflightByUid.erase(it);
        }

        void clear_inflight_key_(const InflightKey &key)
        {
            auto it = m_inflightByUid.find(key);
            if (it == m_inflightByUid.end())
            {
                return;
            }

            auto msIt = m_inflightTs.find(it->second);
            if (msIt != m_inflightTs.end())
            {
                m_inflightTs.erase(msIt);
            }
            m_inflightByUid.erase(it);
        }

        std::optional<TimeStamp> peek_next_pending_ts_()
        {
            while (!m_pending.empty())
            {
                const auto &top = m_pending.top();
                if (m_cancelledByUid.count(UidKey{top.src, top.uid}) == 0)
                {
                    return top.ts;
                }
                m_pending.pop();
            }
            return std::nullopt;
        }

        TimeStamp compute_gvt_()
        {
            TimeStamp gvt = max_stamp_();

            for (const auto &[id, ctx] : m_lps)
            {
                (void)id;
                gvt = min_stamp_(gvt, ctx.vt);
            }

            if (auto pendingMin = peek_next_pending_ts_(); pendingMin)
            {
                gvt = min_stamp_(gvt, *pendingMin);
            }

            if (m_cfg.gvtMode == SimulationConfig::GvtMode::AckInflight && !m_inflightTs.empty())
            {
                gvt = min_stamp_(gvt, *m_inflightTs.begin());
            }

            // If we have no local information about any outstanding work, contribute +inf
            // so global min-reduction is driven by ranks that still have work.
            return gvt;
        }

        void prune_to_gvt_(TimeStamp gvt)
        {
            std::vector<CommittedFlushItem> flush;
            std::vector<CommittedEventFlushItem> committedEvents;
            for (auto &[id, ctx] : m_lps)
            {
                (void)id;

                while (!ctx.processed.empty() && ctx.processed.front().ev.ts < gvt)
                {
                    auto rec = std::move(ctx.processed.front());
                    ctx.processed.pop_front();

                    invariant_or_throw_(rec.ev.ts < gvt, "prune_to_gvt_: attempted to commit non-GVT-safe event");

                    const LPId committedEventSrc = rec.ev.src;
                    const EventUid committedEventUid = rec.ev.uid;

                    if (m_cfg.enableDeterminismOracle)
                    {
                        m_determinism.on_committed_event(m_cfg.rank, id, rec.ev);
                    }

                    if (m_cfg.committedEventSink)
                    {
                        committedEvents.push_back(CommittedEventFlushItem{
                            .ts = rec.ev.ts,
                            .lp = id,
                            .ev = std::move(rec.ev),
                        });
                    }

                    if (m_cfg.committedSink)
                    {
                        for (std::size_t i = 0; i < rec.committed.size(); ++i)
                        {
                            auto &c = rec.committed[i];

                            if (m_cfg.enableDeterminismOracle)
                            {
                                m_determinism.on_committed_output(m_cfg.rank, id, c.ts, c.payload);
                            }
                            flush.push_back(CommittedFlushItem{
                                .ts = c.ts,
                                .lp = id,
                                .eventSrc = committedEventSrc,
                                .eventUid = committedEventUid,
                                .index = static_cast<std::uint32_t>(i),
                                .payload = std::move(c.payload),
                            });
                        }
                    }
                }

                while (!ctx.sentLog.empty() && ctx.sentLog.front().sendTime < gvt)
                {
                    ctx.sentLog.pop_front();
                }
            }

            for (auto it = m_cancelledByUid.begin(); it != m_cancelledByUid.end();)
            {
                if (it->second < gvt)
                {
                    it = m_cancelledByUid.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            if (m_cfg.committedSink && !flush.empty())
            {
                std::sort(flush.begin(), flush.end(), [](const CommittedFlushItem &a, const CommittedFlushItem &b)
                          {
                              if (a.ts < b.ts)
                              {
                                  return true;
                              }
                              if (b.ts < a.ts)
                              {
                                  return false;
                              }
                              if (a.lp != b.lp)
                              {
                                  return a.lp < b.lp;
                              }
                              if (a.eventSrc != b.eventSrc)
                              {
                                  return a.eventSrc < b.eventSrc;
                              }
                              if (a.eventUid != b.eventUid)
                              {
                                  return a.eventUid < b.eventUid;
                              }
                              return a.index < b.index; });

                for (auto &it : flush)
                {
                    m_cfg.committedSink(m_cfg.rank, it.lp, it.ts, it.payload);
                }
            }

            if (m_cfg.committedEventSink && !committedEvents.empty())
            {
                std::sort(committedEvents.begin(), committedEvents.end(), [](const CommittedEventFlushItem &a, const CommittedEventFlushItem &b)
                          {
                              if (a.ts < b.ts)
                              {
                                  return true;
                              }
                              if (b.ts < a.ts)
                              {
                                  return false;
                              }
                              if (a.lp != b.lp)
                              {
                                  return a.lp < b.lp;
                              }
                              if (a.ev.src != b.ev.src)
                              {
                                  return a.ev.src < b.ev.src;
                              }
                              // Tie-break on (src,uid) for deterministic ordering.
                              if (a.ev.uid != b.ev.uid)
                              {
                                  return a.ev.uid < b.ev.uid;
                              }
                              // As a last resort, sort non-anti before anti.
                              return (a.ev.isAnti ? 1 : 0) < (b.ev.isAnti ? 1 : 0); });

                for (const auto &it : committedEvents)
                {
                    m_cfg.committedEventSink(m_cfg.rank, it.lp, it.ev);
                }
            }
        }

        void fossil_collect_(bool doGlobalReduction)
        {
            validate_invariants_("fossil_collect:entry");

            // Fossil collection is only safe once we have a *global* lower bound on
            // timestamps of any future messages. For single-rank, compute_gvt_ is
            // sufficient. For multi-rank, callers must provide gvtReduceMin (e.g.
            // MPI_Allreduce lexicographic min). If a rank mapping is configured
            // but no global reduction exists, refuse to advance GVT or prune.
            if (m_cfg.lpToRank && !m_cfg.gvtReduceMin)
            {
                return;
            }

            // Always prune to the last known (global) GVT.
            // This keeps memory bounded even if we only advance GVT periodically.
            prune_to_gvt_(m_lastGvt);
            apply_ready_migrations_();

            // Single-rank: advance/prune every call.
            // Multi-rank: only advance when it's time to participate in the global collective.
            if (m_cfg.lpToRank && !doGlobalReduction)
            {
                validate_invariants_("fossil_collect:early_exit_no_collective");
                return;
            }

            if (m_cfg.gvtMode == SimulationConfig::GvtMode::ColoredCounters)
            {
                advance_gvt_colored_();
                apply_ready_migrations_();
                notify_directories_gvt_(m_lastGvt);
                validate_invariants_("fossil_collect:exit_colored");
                return;
            }

            // AckInflight mode.
            TimeStamp gvt = compute_gvt_();
            if (m_cfg.gvtReduceMin)
            {
                gvt = m_cfg.gvtReduceMin(gvt);
            }
            if (gvt < m_lastGvt)
            {
                validate_invariants_("fossil_collect:exit_no_advance");
                return;
            }
            m_lastGvt = gvt;
            prune_to_gvt_(m_lastGvt);
            apply_ready_migrations_();
            notify_directories_gvt_(m_lastGvt);

            validate_invariants_("fossil_collect:exit");
        }

        void notify_directories_gvt_(TimeStamp gvt)
        {
            if (!m_cfg.directoryForEntity)
            {
                return;
            }
            if (m_cfg.directoryFossilCollectKind == 0)
            {
                return;
            }
            if (!m_systemSrcSet)
            {
                return;
            }
            if (!(m_lastDirectoryGcGvt < gvt))
            {
                return;
            }

            for (const LPId dir : m_directoryLps)
            {
                Event ev;
                ev.ts = gvt;
                ev.src = m_systemSrc;
                ev.dst = dir;
                ev.target = 0;
                ev.payload.kind = m_cfg.directoryFossilCollectKind;
                ev.payload.bytes = bytes_from_trivially_copyable(gvt);
                send(std::move(ev));
            }

            m_lastDirectoryGcGvt = gvt;
        }

        void apply_ready_migrations_()
        {
            // GVT-safe: a migration is only committed/applied once we have proven that no
            // message with timestamp < effectiveTs can arrive in the future.
            while (!m_pendingMigrations.empty())
            {
                auto it = m_pendingMigrations.begin();
                const TimeStamp ts = it->first;
                if (m_lastGvt < ts)
                {
                    break;
                }

                const Migration mig = std::move(it->second);
                m_pendingMigrations.erase(it);

                auto &ctx = ctx_(mig.newOwner);
                ctx.lp->on_migration(mig.entity, std::span<const std::byte>(mig.state.data(), mig.state.size()));
                m_entityOwner[mig.entity] = mig.newOwner;
            }
        }

        void advance_gvt_colored_()
        {
            if (!m_cfg.gvtReduceMin || !m_cfg.gvtReduceSum)
            {
                return;
            }

            // Phase 0 (white): synchronize a global flip to red.
            if (!m_gvtRedPhase)
            {
                // Barrier via a sum-reduction; guarantees all ranks flip together.
                (void)m_cfg.gvtReduceSum(0);
                m_gvtRedPhase = true;
                m_gvtColor = static_cast<std::uint8_t>(m_gvtWhiteColor ^ 1);
                m_gvtWhiteSentAtFlip = m_gvtSentByColor[static_cast<std::size_t>(m_gvtWhiteColor)];
                m_gvtMinRedSentTs = max_stamp_();
                return;
            }

            // Phase 1 (red): wait until all white messages (sent before the flip) have been received.
            const std::int64_t localDelta = static_cast<std::int64_t>(m_gvtWhiteSentAtFlip) -
                                            static_cast<std::int64_t>(m_gvtRecvByColor[static_cast<std::size_t>(m_gvtWhiteColor)]);
            const std::int64_t globalDelta = m_cfg.gvtReduceSum(localDelta);
            if (globalDelta != 0)
            {
                return;
            }

            // All pre-flip messages are accounted for; safe to compute a global lower bound.
            TimeStamp gvt = compute_gvt_();
            gvt = min_stamp_(gvt, m_gvtMinRedSentTs);
            gvt = m_cfg.gvtReduceMin(gvt);
            if (gvt < m_lastGvt)
            {
                return;
            }
            m_lastGvt = gvt;
            prune_to_gvt_(m_lastGvt);
            notify_directories_gvt_(m_lastGvt);

            // Reset for next round.
            m_gvtRedPhase = false;
            m_gvtWhiteColor = static_cast<std::uint8_t>(m_gvtWhiteColor ^ 1);
            m_gvtColor = m_gvtWhiteColor;
            m_gvtWhiteSentAtFlip = 0;
            m_gvtMinRedSentTs = max_stamp_();
        }

        std::optional<Event> pop_next_()
        {
            while (!m_pending.empty())
            {
                Event ev = m_pending.top();
                m_pending.pop();
                if (!ev.isAnti && m_cancelledByUid.count(UidKey{ev.src, ev.uid}) != 0)
                {
                    continue;
                }
                return ev;
            }
            return std::nullopt;
        }

        void dispatch_(const Event &ev)
        {
            validate_invariants_("dispatch:entry");

            // Anti-messages cancel a matching event if still pending or unprocessed.
            if (ev.isAnti)
            {
                auto &dstCtx = ctx_(ev.dst);

                // Anti-messages are still time-stamped inputs. If they arrive in the past
                // relative to this LP's current virtual time, roll back and reschedule.
                if (ev.ts < dstCtx.vt)
                {
                    ++m_rollbacksStraggler;
                    rollback_(dstCtx, ev.ts);
                    enqueue_(ev);
                    return;
                }

                ProcessedRecord rec;
                rec.ev = ev;
                rec.preNextSeqNormal = dstCtx.nextSeqNormal;
                rec.preNextSeqControl = dstCtx.nextSeqControl;

                if (auto it = m_cancelledByUid.find(UidKey{ev.src, ev.uid}); it != m_cancelledByUid.end())
                {
                    rec.cancelHadEntry = true;
                    rec.cancelPrevTs = it->second;
                }

                cancel_(ev);

                // Advance local virtual time: anti-messages are processed at their timestamp.
                dstCtx.vt = ev.ts;

                // Record anti processing so its cancellation effect can be rolled back.
                dstCtx.processed.push_back(std::move(rec));

                validate_invariants_("dispatch:exit_anti");
                return;
            }

            auto &dstCtx = ctx_(ev.dst);

            // Straggler => rollback.
            if (ev.ts < dstCtx.vt)
            {
                ++m_rollbacksStraggler;
                Logger::instance().logf(LogLevel::Debug, m_cfg.rank, ev.dst, ev.ts,
                                        "straggler rollback: dst_vt=%llu.%llu ev_uid=%llu src=%u",
                                        static_cast<unsigned long long>(dstCtx.vt.time),
                                        static_cast<unsigned long long>(dstCtx.vt.sequence),
                                        static_cast<unsigned long long>(ev.uid),
                                        static_cast<unsigned>(ev.src));
                rollback_(dstCtx, ev.ts);

                // After rolling back, re-enqueue the straggler and let the scheduler
                // select the next event in timestamp order. This avoids processing the
                // straggler out-of-order relative to other events reintroduced by rollback.
                enqueue_(ev);
                validate_invariants_("dispatch:exit_straggler");
                return;
            }

            ProcessedRecord rec;
            rec.ev = ev;
            rec.preNextSeqNormal = dstCtx.nextSeqNormal;
            rec.preNextSeqControl = dstCtx.nextSeqControl;

            class EventContext final : public IEventContext
            {
            public:
                EventContext(Simulation &sim, LPContext &lpCtx, ProcessedRecord &rec) : m_sim(sim), m_lpCtx(lpCtx), m_rec(rec) {}

                void send(Event ev) override { m_sim.send(std::move(ev)); }

                void request_write(EntityId id) override
                {
                    if (m_rec.preEntityState.find(id) != m_rec.preEntityState.end())
                    {
                        return;
                    }

                    if (m_lpCtx.lp->supports_entity_snapshots())
                    {
                        m_rec.preEntityState.emplace(id, m_lpCtx.lp->save_entity(id));
                        return;
                    }

                    if (!m_rec.hasLpSnapshot)
                    {
                        m_rec.hasLpSnapshot = true;
                        m_rec.preLpState = m_lpCtx.lp->save_state();
                    }
                }

                void emit_committed(TimeStamp ts, Payload payload) override
                {
                    // Keep the API simple: committed output is tied to the current event timestamp.
                    if (!(ts == m_rec.ev.ts))
                    {
                        throw std::runtime_error("emit_committed: ts must match the current event timestamp");
                    }
                    m_rec.committed.push_back(CommittedOutput{ts, std::move(payload)});
                }

            private:
                Simulation &m_sim;
                LPContext &m_lpCtx;
                ProcessedRecord &m_rec;
            };

            EventContext ctx(*this, dstCtx, rec);

            const TimeStamp prevVt = dstCtx.vt;

            try
            {
                dstCtx.vt = ev.ts;
                dstCtx.inEvent = true;
                dstCtx.currentExecTs = ev.ts;
                dstCtx.lp->on_event(ev, ctx);
                dstCtx.inEvent = false;
                dstCtx.processed.push_back(std::move(rec));

                if (m_cfg.enableInstallBeforeUseBuffering && ev.target != 0 && ev.payload.kind == m_cfg.installEventKind)
                {
                    release_deferred_for_(ev.dst, ev.target);
                }
            }
            catch (const std::exception &e)
            {
                dstCtx.inEvent = false;

                if (m_cfg.enableInstallBeforeUseBuffering && ev.target != 0 && ev.payload.kind != m_cfg.installEventKind &&
                    is_missing_entity_error_(e))
                {
                    // The model could not process this targeted event because the entity is not present yet.
                    // Do not advance local virtual time; defer until install arrives.
                    dstCtx.vt = prevVt;
                    dstCtx.currentExecTs = prevVt;
                    defer_targeted_event_(ev);
                    validate_invariants_("dispatch:exit_deferred_missing_entity");
                    return;
                }

                throw;
            }

            ++m_totalProcessed;

            validate_invariants_("dispatch:exit");
        }

        void cancel_(const Event &anti)
        {
            // If the corresponding event is still pending, we do a lazy cancel:
            // record tombstone (src, uid) and skip when popped.
            auto [it, inserted] = m_cancelledByUid.emplace(UidKey{anti.src, anti.uid}, anti.ts);
            if (!inserted && anti.ts < it->second)
            {
                it->second = anti.ts;
            }

            // If already processed, optimistic cancellation implies a rollback to before that event.
            auto &dstCtx = ctx_(anti.dst);
            for (auto it = dstCtx.processed.rbegin(); it != dstCtx.processed.rend(); ++it)
            {
                // Match only the original (non-anti) event. Anti-messages reuse the cancelled
                // event's UID, so matching anti records here would cause spurious rollbacks.
                if (!it->ev.isAnti && it->ev.src == anti.src && it->ev.uid == anti.uid)
                {
                    ++m_antiCancelledProcessed;
                    ++m_rollbacksAnti;
                    Logger::instance().logf(LogLevel::Debug, m_cfg.rank, anti.dst, anti.ts,
                                            "anti rollback: uid=%llu src=%u",
                                            static_cast<unsigned long long>(anti.uid),
                                            static_cast<unsigned>(anti.src));
                    rollback_(dstCtx, it->ev.ts);
                    return;
                }
            }

            // Otherwise the corresponding event is either still pending or has not arrived yet.
            ++m_antiCancelledPending;
        }

        void rollback_(LPContext &ctx, TimeStamp to)
        {
            validate_invariants_("rollback:entry");
            ++m_rollbacksTotal;

            // Undo all processed events with timestamp >= `to`.
            // Restores state using per-entity snapshots taken on first write.
            std::vector<ProcessedRecord> undone;
            while (!ctx.processed.empty())
            {
                const auto &last = ctx.processed.back();
                if (last.ev.ts < to)
                {
                    break;
                }

                // Undo rollbackable kernel effects of anti-messages.
                if (last.ev.isAnti)
                {
                    if (last.cancelHadEntry)
                    {
                        m_cancelledByUid[UidKey{last.ev.src, last.ev.uid}] = last.cancelPrevTs;
                    }
                    else
                    {
                        m_cancelledByUid.erase(UidKey{last.ev.src, last.ev.uid});
                    }
                }

                undone.push_back(ctx.processed.back());
                enqueue_(ctx.processed.back().ev);
                ctx.processed.pop_back();
            }

            m_rollbackUndoneEvents += static_cast<std::uint64_t>(undone.size());

            if (!undone.empty())
            {
                // Restore kernel metadata from the oldest undone record.
                const auto &oldest = undone.back();

                // Restore rollback-able kernel metadata.
                ctx.nextSeqNormal = oldest.preNextSeqNormal;
                ctx.nextSeqControl = oldest.preNextSeqControl;

                // Restore LP state. The oldest undone record may be an anti-message (no snapshot),
                // but later undone records may have mutated state. Use the oldest undone record
                // that captured a snapshot.
                const ProcessedRecord *snap = nullptr;
                for (auto it = undone.rbegin(); it != undone.rend(); ++it)
                {
                    if (it->hasLpSnapshot)
                    {
                        snap = &(*it);
                        break;
                    }
                }
                if (snap)
                {
                    ctx.lp->load_state(std::span<const std::byte>(snap->preLpState.data(), snap->preLpState.size()));
                }

                if (ctx.lp->supports_entity_snapshots())
                {
                    std::unordered_set<EntityId> restored;
                    // Apply per-entity pre-states from oldest->newest undone.
                    for (auto it = undone.rbegin(); it != undone.rend(); ++it)
                    {
                        for (const auto &[entityId, bytes] : it->preEntityState)
                        {
                            if (!restored.insert(entityId).second)
                            {
                                continue;
                            }
                            ctx.lp->load_entity(entityId, std::span<const std::byte>(bytes.data(), bytes.size()));
                        }
                    }
                }
            }

            ctx.vt = prev_stamp(to);

            // Send anti-messages for events sent after rollback point.
            while (!ctx.sentLog.empty())
            {
                const auto &last = ctx.sentLog.back();
                if (last.sendTime < to)
                {
                    break;
                }

                Event anti;
                anti.ts = last.msgTime;
                anti.src = ctx.lp->id();
                anti.dst = last.dst;
                anti.target = 0;
                anti.uid = last.uid;
                anti.isAnti = true;

                // Route via Simulation::send to keep transport/inflight bookkeeping consistent.
                send(std::move(anti));

                ctx.sentLog.pop_back();
            }

            validate_invariants_("rollback:exit");
        }

        void apply_migration_(const Migration &mig)
        {
            // Buffer until GVT proves this cutover is safe.
            m_pendingMigrations.emplace(mig.effectiveTs, mig);
            apply_ready_migrations_();
        }

        SimulationConfig m_cfg;
        std::shared_ptr<ITransport> m_transport;

        std::unordered_map<LPId, LPContext> m_lps;

        LPId m_systemSrc = 0;
        bool m_systemSrcSet = false;
        bool m_started = false;
        std::priority_queue<Event, std::vector<Event>, PendingOrder> m_pending;

        // Tombstones for cancelled (src,uid) pairs (anti-messages). Timestamp used for fossil collection.
        std::unordered_map<UidKey, TimeStamp, UidKeyHash> m_cancelledByUid;

        void start_if_needed_()
        {
            if (m_started)
            {
                return;
            }
            m_started = true;

            // allow LPs to seed work
            for (auto &[id, ctx] : m_lps)
            {
                (void)id;
                ctx.lp->on_start(*this);
            }
        }

        // GVT-safe entity migration: migrations are applied only once GVT >= effectiveTs.
        std::multimap<TimeStamp, Migration> m_pendingMigrations;
        std::unordered_map<EntityId, LPId> m_entityOwner;

        DeterminismAccumulator m_determinism;

        // Deferred targeted events (install-before-use buffering).
        // Key is (dst LPId, EntityId) packed into u64.
        std::unordered_map<std::uint64_t, std::vector<Event>> m_deferredTargeted;

        // Conservative single-rank GVT accounts for messages delayed in the transport.
        std::unordered_map<InflightKey, TimeStamp, InflightKeyHash> m_inflightByUid;
        std::multiset<TimeStamp> m_inflightTs;

        // Algorithmic GVT (colored counters) bookkeeping.
        // Each round alternates which bit value is considered "white" so we
        // eventually account for all in-flight messages without ACKs.
        std::uint8_t m_gvtWhiteColor = 0;
        std::uint8_t m_gvtColor = 0; // current send color (whiteColor or whiteColor^1)
        bool m_gvtRedPhase = false;
        std::uint64_t m_gvtSentByColor[2] = {0, 0};
        std::uint64_t m_gvtRecvByColor[2] = {0, 0};
        std::uint64_t m_gvtWhiteSentAtFlip = 0;
        TimeStamp m_gvtMinRedSentTs = max_stamp_();

        TimeStamp m_lastGvt{0, 0};

        std::unordered_set<LPId> m_directoryLps;
        TimeStamp m_lastDirectoryGcGvt{0, 0};

        // Monotonic counters.
        std::uint64_t m_totalProcessed = 0;
        std::uint64_t m_rollbacksTotal = 0;
        std::uint64_t m_rollbacksStraggler = 0;
        std::uint64_t m_rollbacksAnti = 0;
        std::uint64_t m_rollbackUndoneEvents = 0;
        std::uint64_t m_sentEvents = 0;
        std::uint64_t m_sentAnti = 0;
        std::uint64_t m_receivedEvents = 0;
        std::uint64_t m_receivedAnti = 0;

        std::uint64_t m_antiCancelledPending = 0;
        std::uint64_t m_antiCancelledProcessed = 0;
    };
}
