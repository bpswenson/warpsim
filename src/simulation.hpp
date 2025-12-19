#pragma once

#include "event.hpp"
#include "event_wire.hpp"
#include "log.hpp"
#include "lp.hpp"
#include "migration.hpp"
#include "transport.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stdexcept>
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
        // Models should derive random values from this seed + LPId + EventUid so results
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
            ctx.nextSeq = 1;

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
                    const auto uid = static_cast<EventUid>(r.read_u64());
                    const bool isAnti = (r.read_u8() != 0);
                    clear_inflight_key_(InflightKey{uid, isAnti});
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
            // Assign uid if caller didn't.
            if (ev.uid == 0)
            {
                // UIDs must be globally unique so anti-messages cancel the right event.
                // Also keep them deterministic regardless of MPI rank mapping.
                // Format: [src LPId:32][per-LP counter:32].
                auto &ctx = ctx_(ev.src);
                if (ctx.nextUid == 0)
                {
                    throw std::runtime_error("Event UID counter overflow for LPId (exhausted 2^32-1 UIDs)");
                }
                const std::uint32_t local = ctx.nextUid++;
                ev.uid = (static_cast<std::uint64_t>(ev.src) << 32) | static_cast<std::uint64_t>(local);
            }

            // Auto-assign sequence if caller didn't set it.
            if (ev.ts.sequence == 0)
            {
                auto &ctx = ctx_(ev.src);
                ev.ts.sequence = ctx.nextSeq++;
            }

            // Entity routing indirection: if a migration has established an owner for this
            // entity, route to that owner regardless of the caller-provided dst.
            if (ev.target != 0)
            {
                if (m_cfg.directoryForEntity)
                {
                    const LPId dir = m_cfg.directoryForEntity(ev.target);
                    m_directoryLps.insert(dir);
                    // Avoid re-routing forwarded events emitted by the directory itself.
                    if (ev.src != dir)
                    {
                        ev.dst = dir;
                    }
                }
                else
                {
                    auto it = m_entityOwner.find(ev.target);
                    if (it != m_entityOwner.end())
                    {
                        ev.dst = it->second;
                    }
                }
            }

            // Local rank routing for now (multirank caller can plug in rank mapping).
            WireMessage msg;
            msg.kind = MessageKind::Event;
            msg.srcRank = m_cfg.rank;
            msg.dstRank = m_cfg.lpToRank ? m_cfg.lpToRank(ev.dst) : m_cfg.rank;
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
                ctx.sentLog.push_back(SentRecord{ev.ts, ev.dst, ev.uid});
            }
        }

    private:
        struct CommittedOutput
        {
            TimeStamp ts;
            Payload payload;
        };

        struct CommittedFlushItem
        {
            TimeStamp ts;
            LPId lp = 0;
            EventUid eventUid = 0;
            std::uint32_t index = 0;
            Payload payload;
        };

        struct InflightKey
        {
            EventUid uid = 0;
            bool isAnti = false;

            bool operator==(const InflightKey &o) const noexcept
            {
                return uid == o.uid && isAnti == o.isAnti;
            }
        };

        struct InflightKeyHash
        {
            std::size_t operator()(const InflightKey &k) const noexcept
            {
                const std::uint64_t x = (static_cast<std::uint64_t>(k.uid) << 1) ^ (k.isAnti ? 1ULL : 0ULL);
                return static_cast<std::size_t>(x ^ (x >> 33) ^ (x >> 17));
            }
        };

        struct SentRecord
        {
            TimeStamp ts;
            LPId dst;
            EventUid uid;
        };

        struct ProcessedRecord
        {
            Event ev;

            // Rollback-able kernel metadata.
            // This is separate from Event.uid (which must never be reused).
            std::uint64_t preNextSeq = 1;

            bool hasLpSnapshot = false;
            ByteBuffer preLpState;
            std::unordered_map<EntityId, ByteBuffer> preEntityState;

            // Committed side-effects recorded during this event execution.
            // These are only emitted once the record is fossil-collected (ts < GVT).
            std::vector<CommittedOutput> committed;
        };

        struct LPContext
        {
            std::unique_ptr<ILogicalProcess> lp;
            TimeStamp vt{0, 0};

            std::deque<ProcessedRecord> processed;
            std::deque<SentRecord> sentLog;

            std::uint64_t nextSeq = 1;
            std::uint32_t nextUid = 1;
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
            auto [it, inserted] = m_inflightByUid.emplace(InflightKey{ev.uid, ev.isAnti}, ev.ts);
            if (inserted)
            {
                m_inflightTs.insert(ev.ts);
            }
        }

        void track_inflight_recv_(const Event &ev)
        {
            auto it = m_inflightByUid.find(InflightKey{ev.uid, ev.isAnti});
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
                if (m_cancelledByUid.count(top.uid) == 0)
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

            if (gvt == max_stamp_())
            {
                return TimeStamp{0, 0};
            }
            return gvt;
        }

        void prune_to_gvt_(TimeStamp gvt)
        {
            std::vector<CommittedFlushItem> flush;
            for (auto &[id, ctx] : m_lps)
            {
                (void)id;

                while (!ctx.processed.empty() && ctx.processed.front().ev.ts < gvt)
                {
                    auto rec = std::move(ctx.processed.front());
                    ctx.processed.pop_front();

                    if (m_cfg.committedSink)
                    {
                        for (std::size_t i = 0; i < rec.committed.size(); ++i)
                        {
                            auto &c = rec.committed[i];
                            flush.push_back(CommittedFlushItem{
                                .ts = c.ts,
                                .lp = id,
                                .eventUid = rec.ev.uid,
                                .index = static_cast<std::uint32_t>(i),
                                .payload = std::move(c.payload),
                            });
                        }
                    }
                }

                while (!ctx.sentLog.empty() && ctx.sentLog.front().ts < gvt)
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
        }

        void fossil_collect_(bool doGlobalReduction)
        {
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
                return;
            }

            if (m_cfg.gvtMode == SimulationConfig::GvtMode::ColoredCounters)
            {
                advance_gvt_colored_();
                apply_ready_migrations_();
                notify_directories_gvt_(m_lastGvt);
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
                return;
            }
            m_lastGvt = gvt;
            prune_to_gvt_(m_lastGvt);
            apply_ready_migrations_();
            notify_directories_gvt_(m_lastGvt);
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
                if (m_cancelledByUid.count(ev.uid) != 0)
                {
                    continue;
                }
                return ev;
            }
            return std::nullopt;
        }

        void dispatch_(const Event &ev)
        {
            // Anti-messages cancel a matching event if still pending or unprocessed.
            if (ev.isAnti)
            {
                cancel_(ev);
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
            }

            ProcessedRecord rec;
            rec.ev = ev;
            rec.preNextSeq = dstCtx.nextSeq;

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

            dstCtx.vt = ev.ts;
            dstCtx.lp->on_event(ev, ctx);
            dstCtx.processed.push_back(std::move(rec));

            ++m_totalProcessed;
        }

        void cancel_(const Event &anti)
        {
            // If the corresponding event is still pending, we do a lazy cancel:
            // record tombstone uid and skip when popped.
            auto [it, inserted] = m_cancelledByUid.emplace(anti.uid, anti.ts);
            if (!inserted && anti.ts < it->second)
            {
                it->second = anti.ts;
            }

            // If already processed, optimistic cancellation implies a rollback to before that event.
            auto &dstCtx = ctx_(anti.dst);
            for (auto it = dstCtx.processed.rbegin(); it != dstCtx.processed.rend(); ++it)
            {
                if (it->ev.uid == anti.uid)
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
                undone.push_back(ctx.processed.back());
                enqueue_(ctx.processed.back().ev);
                ctx.processed.pop_back();
            }

            m_rollbackUndoneEvents += static_cast<std::uint64_t>(undone.size());

            if (!undone.empty())
            {
                // Restore LP snapshot if we have one (oldest undone record).
                const auto &oldest = undone.back();

                // Restore rollback-able kernel metadata.
                ctx.nextSeq = oldest.preNextSeq;

                if (oldest.hasLpSnapshot)
                {
                    ctx.lp->load_state(std::span<const std::byte>(oldest.preLpState.data(), oldest.preLpState.size()));
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
                if (last.ts < to)
                {
                    break;
                }

                Event anti;
                anti.ts = last.ts;
                anti.src = ctx.lp->id();
                anti.dst = last.dst;
                anti.target = 0;
                anti.uid = last.uid;
                anti.isAnti = true;

                // Route via Simulation::send to keep transport/inflight bookkeeping consistent.
                send(std::move(anti));

                ctx.sentLog.pop_back();
            }
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

        // Tombstones for cancelled UIDs (anti-messages). Timestamp used for fossil collection.
        std::unordered_map<EventUid, TimeStamp> m_cancelledByUid;

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
