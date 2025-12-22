/*
Purpose: Stress test for rollback + determinism.

What this tests: A bounded randomized workload (seeded) triggers rollbacks/anti messages
and asserts that committed-event trace and committed output are identical across runs even
with different rank IDs and different LP insertion order.
*/

#include "random.hpp"
#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>

namespace
{
    constexpr std::uint32_t KindSeed = 1;
    constexpr std::uint32_t KindWork = 2;
    constexpr std::uint32_t KindNoop = 3;
    constexpr std::uint32_t KindCommitted = 200;

    struct Work
    {
        std::uint64_t logicalKey = 0;
        std::uint32_t depth = 0;
    };

    struct Committed
    {
        std::uint32_t lp = 0;
        std::uint64_t logicalKey = 0;
        std::uint64_t t = 0;
    };

    struct Shared
    {
        // Out-of-band guard so rollback cannot re-trigger forever.
        std::uint32_t remainingStragglers = 5;
    };

    struct TraceEvent
    {
        warpsim::LPId lp = 0;
        warpsim::LPId src = 0;
        warpsim::TimeStamp ts{};
        warpsim::EventUid uid = 0;
        bool isAnti = false;
        std::uint32_t kind = 0;

        bool operator==(const TraceEvent &o) const noexcept
        {
            return std::tie(lp, src, ts.time, ts.sequence, uid, isAnti, kind) ==
                   std::tie(o.lp, o.src, o.ts.time, o.ts.sequence, o.uid, o.isAnti, o.kind);
        }
    };

    struct TraceOutput
    {
        warpsim::LPId lp = 0;
        warpsim::TimeStamp ts{};
        std::uint32_t kind = 0;
        std::vector<std::byte> bytes;

        bool operator==(const TraceOutput &o) const noexcept
        {
            return std::tie(lp, ts.time, ts.sequence, kind, bytes) ==
                   std::tie(o.lp, o.ts.time, o.ts.sequence, o.kind, o.bytes);
        }
    };

    class StressLP final : public warpsim::ILogicalProcess
    {
    public:
        StressLP(warpsim::LPId id, std::uint64_t seed, warpsim::LPId numLps, Shared &shared)
            : m_id(id), m_seed(seed), m_numLps(numLps), m_shared(&shared)
        {
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            if (m_id != 0)
            {
                return;
            }

            // Seed a few initial work items.
            for (std::uint64_t i = 0; i < 8; ++i)
            {
                Work w{.logicalKey = (0xA11CEULL << 32) ^ i, .depth = 0};

                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{10 + i, 0};
                ev.src = m_id;
                ev.dst = static_cast<warpsim::LPId>(i % m_numLps);
                ev.payload.kind = KindWork;
                ev.payload.bytes = warpsim::bytes_from_trivially_copyable(w);
                sink.send(std::move(ev));
            }

            // Noops to drive GVT/termination.
            for (std::uint64_t t = 200; t <= 240; ++t)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{t, 0};
                ev.src = m_id;
                ev.dst = m_id;
                ev.payload.kind = KindNoop;
                sink.send(std::move(ev));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindWork)
            {
                if (ev.payload.bytes.size() != sizeof(Work))
                {
                    return;
                }

                Work w{};
                std::memcpy(&w, ev.payload.bytes.data(), sizeof(w));

                // Emit committed output for some work items.
                if ((w.logicalKey % 3ULL) == 0)
                {
                    Committed c;
                    c.lp = static_cast<std::uint32_t>(m_id);
                    c.logicalKey = w.logicalKey;
                    c.t = ev.ts.time;

                    warpsim::Payload p;
                    p.kind = KindCommitted;
                    p.bytes = warpsim::bytes_from_trivially_copyable(c);
                    ctx.emit_committed(ev.ts, std::move(p));
                }

                // Bounded expansion.
                if (ev.ts.time < 180 && w.depth < 6)
                {
                    // Use logicalKey as RNG key (model-level stable key).
                    const std::uint64_t r0 = warpsim::rng_u64(m_seed, m_id, /*stream=*/1, static_cast<warpsim::EventUid>(w.logicalKey), /*draw=*/0);
                    const std::uint64_t r1 = warpsim::rng_u64(m_seed, m_id, /*stream=*/2, static_cast<warpsim::EventUid>(w.logicalKey), /*draw=*/1);

                    const warpsim::LPId dst = static_cast<warpsim::LPId>(r0 % m_numLps);
                    const std::uint64_t delta = 1 + (r1 % 7);

                    Work nextW{.logicalKey = (w.logicalKey * 6364136223846793005ULL) ^ (static_cast<std::uint64_t>(m_id) << 16) ^ delta,
                               .depth = static_cast<std::uint32_t>(w.depth + 1)};

                    warpsim::Event next;
                    next.ts = warpsim::TimeStamp{ev.ts.time + delta, 0};
                    next.src = m_id;
                    next.dst = dst;
                    next.payload.kind = KindWork;
                    next.payload.bytes = warpsim::bytes_from_trivially_copyable(nextW);
                    ctx.send(std::move(next));
                }

                // Occasionally inject a straggler to force rollback on LP0.
                // Guarded out-of-band so it happens a finite number of times.
                if (m_shared && m_shared->remainingStragglers > 0 && ev.ts.time >= 60 && (w.logicalKey % 11ULL) == 0)
                {
                    --m_shared->remainingStragglers;
                    warpsim::Event s;
                    s.ts = warpsim::TimeStamp{5, 1};
                    s.src = m_id;
                    s.dst = 0;
                    s.payload.kind = KindSeed;
                    ctx.send(std::move(s));
                }
            }

            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        std::uint64_t m_seed = 1;
        warpsim::LPId m_numLps = 1;
        Shared *m_shared = nullptr;
    };

    static std::pair<std::vector<TraceEvent>, std::vector<TraceOutput>> run_once(std::uint64_t seed, warpsim::RankId rank, bool reverseLpOrder)
    {
        std::vector<TraceEvent> eventTrace;
        std::vector<TraceOutput> outputTrace;

        auto transport = std::make_shared<warpsim::InProcTransport>();

        warpsim::SimulationConfig cfg;
        cfg.rank = rank;
        cfg.logLevel = warpsim::LogLevel::Off;
        cfg.seed = seed;

        cfg.committedEventSink = [&](warpsim::RankId, warpsim::LPId lp, const warpsim::Event &ev)
        {
            eventTrace.push_back(TraceEvent{.lp = lp,
                                            .src = ev.src,
                                            .ts = ev.ts,
                                            .uid = ev.uid,
                                            .isAnti = ev.isAnti,
                                            .kind = ev.payload.kind});
        };

        cfg.committedSink = [&](warpsim::RankId, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload &p)
        {
            outputTrace.push_back(TraceOutput{.lp = lp,
                                              .ts = ts,
                                              .kind = p.kind,
                                              .bytes = p.bytes});
        };

        warpsim::Simulation sim(cfg, transport);

        constexpr warpsim::LPId kNumLps = 8;
        Shared shared;

        if (!reverseLpOrder)
        {
            for (warpsim::LPId i = 0; i < kNumLps; ++i)
            {
                sim.add_lp(std::make_unique<StressLP>(i, cfg.seed, kNumLps, shared));
            }
        }
        else
        {
            for (warpsim::LPId i = kNumLps; i-- > 0;)
            {
                sim.add_lp(std::make_unique<StressLP>(i, cfg.seed, kNumLps, shared));
            }
        }

        sim.run();
        return {eventTrace, outputTrace};
    }

    template <typename T>
    static bool equal_or_report(const char *label, std::uint64_t seed, const std::vector<T> &a, const std::vector<T> &b)
    {
        if (a == b)
        {
            return true;
        }

        std::cerr << "Determinism mismatch (" << label << ") seed=" << seed << "\n";
        std::cerr << "sizes: a=" << a.size() << " b=" << b.size() << "\n";

        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!(a[i] == b[i]))
            {
                std::cerr << "first differing index: " << i << "\n";
                break;
            }
        }
        return false;
    }
}

int main()
{
    // Fixed seeds so this test is deterministic (not flaky), while still covering lots of paths.
    const std::uint64_t seeds[] = {1, 2, 3, 4, 5, 123, 999};
    for (std::uint64_t seed : seeds)
    {
        const auto [ev0, out0] = run_once(seed, /*rank=*/0, /*reverseLpOrder=*/false);
        const auto [ev1, out1] = run_once(seed, /*rank=*/7, /*reverseLpOrder=*/true);

        // Exact-match oracle: if rollback/delivery ordering changes across rank or LP add order,
        // the committed traces will diverge.
        const bool okEv = equal_or_report("committedEventTrace", seed, ev0, ev1);
        const bool okOut = equal_or_report("committedOutput", seed, out0, out1);
        assert(okEv);
        assert(okOut);

        // Ensure we actually committed some work.
        assert(!out0.empty());
    }
    return 0;
}
