/*
Purpose: Unit tests for modeler-facing helper APIs.

What this tests: modeling::pack/unpack validates kind/size with exceptions, typed event
helpers build/send/require correctly, WriteGuard calls request_write, and modeling RNG
helpers match the recommended (src,uid)-salted RNG.
*/

#include "modeling.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace
{
    constexpr std::uint32_t KindFoo = 1234;

    struct Foo
    {
        std::uint64_t a = 0;
        std::uint32_t b = 0;
    };

    class CaptureCtx final : public warpsim::IEventContext
    {
    public:
        void send(warpsim::Event ev) override { sent.push_back(std::move(ev)); }

        void request_write(warpsim::EntityId id) override { writes.push_back(id); }

        void emit_committed(warpsim::TimeStamp ts, warpsim::Payload p) override
        {
            committedTs.push_back(ts);
            committed.push_back(std::move(p));
        }

        std::vector<warpsim::EntityId> writes;
        std::vector<warpsim::Event> sent;
        std::vector<warpsim::TimeStamp> committedTs;
        std::vector<warpsim::Payload> committed;
    };

    template <class Fn>
    void expect_throw(Fn &&fn)
    {
        bool threw = false;
        try
        {
            fn();
        }
        catch (...)
        {
            threw = true;
        }
        assert(threw);
    }
}

int main()
{
    // pack/unpack roundtrip.
    {
        Foo f{.a = 42, .b = 7};
        warpsim::Payload p = warpsim::modeling::pack(KindFoo, f);
        const Foo u = warpsim::modeling::unpack<Foo>(p, KindFoo);
        assert(u.a == 42);
        assert(u.b == 7);
    }

    // unpack throws on wrong kind.
    {
        Foo f{.a = 1, .b = 2};
        warpsim::Payload p = warpsim::modeling::pack(KindFoo, f);
        expect_throw([&]
                     { (void)warpsim::modeling::unpack<Foo>(p, /*expectedKind=*/KindFoo + 1); });
    }

    // unpack throws on wrong size.
    {
        Foo f{.a = 1, .b = 2};
        warpsim::Payload p = warpsim::modeling::pack(KindFoo, f);
        p.bytes.push_back(std::byte{0});
        expect_throw([&]
                     { (void)warpsim::modeling::unpack<Foo>(p, KindFoo); });
    }

    // Event overload adds event identity context.
    {
        warpsim::Event ev;
        ev.ts = warpsim::TimeStamp{10, 1};
        ev.src = 3;
        ev.dst = 4;
        ev.uid = 99;
        ev.payload.kind = KindFoo;
        ev.payload.bytes = {std::byte{0xAA}}; // wrong size

        expect_throw([&]
                     { (void)warpsim::modeling::unpack<Foo>(ev, KindFoo); });
    }

    // WriteGuard calls request_write.
    {
        CaptureCtx ctx;
        {
            auto g = warpsim::modeling::write(ctx, /*entity=*/0xBEEF);
            (void)g;
        }
        assert(ctx.writes.size() == 1);
        assert(ctx.writes[0] == 0xBEEF);
    }

    // Typed event helpers: make_event + require.
    {
        Foo f{.a = 9, .b = 10};
        const warpsim::Event ev = warpsim::modeling::make_event(warpsim::TimeStamp{5, 0}, /*src=*/1, /*dst=*/2, KindFoo, f);
        const Foo got = warpsim::modeling::require<Foo>(ev, KindFoo);
        assert(got.a == 9);
        assert(got.b == 10);
    }

    // Typed event helpers: send() packs and forwards the event.
    {
        CaptureCtx ctx;
        Foo f{.a = 123, .b = 456};
        warpsim::modeling::send(ctx, warpsim::TimeStamp{7, 0}, /*src=*/3, /*dst=*/4, KindFoo, f);
        assert(ctx.sent.size() == 1);
        assert(ctx.sent[0].src == 3);
        assert(ctx.sent[0].dst == 4);
        assert(ctx.sent[0].ts.time == 7);
        const Foo got = warpsim::modeling::require<Foo>(ctx.sent[0], KindFoo);
        assert(got.a == 123);
        assert(got.b == 456);
    }

    // Targeted event helpers: model code doesn't need to know the destination LP.
    {
        CaptureCtx ctx;
        Foo f{.a = 1001, .b = 1002};
        warpsim::modeling::send_targeted(ctx, warpsim::TimeStamp{11, 0}, /*src=*/5, /*target=*/0xABCDEFu, KindFoo, f);
        assert(ctx.sent.size() == 1);
        assert(ctx.sent[0].src == 5);
        assert(ctx.sent[0].target == 0xABCDEFu);
        // dst uses an "unset" sentinel; Simulation::send is expected to route when target!=0.
        assert(ctx.sent[0].dst == warpsim::modeling::UnsetDst);
        const Foo got = warpsim::modeling::require<Foo>(ctx.sent[0], KindFoo);
        assert(got.a == 1001);
        assert(got.b == 1002);
    }

    // Typed committed output helper.
    {
        CaptureCtx ctx;
        Foo f{.a = 77, .b = 88};
        warpsim::modeling::emit_committed(ctx, warpsim::TimeStamp{9, 1}, /*kind=*/KindFoo, f);
        assert(ctx.committed.size() == 1);
        assert(ctx.committedTs.size() == 1);
        assert(ctx.committedTs[0].time == 9);
        const Foo got = warpsim::modeling::unpack<Foo>(ctx.committed[0], KindFoo);
        assert(got.a == 77);
        assert(got.b == 88);
    }

    // RNG helper matches the recommended (src,uid)-salted RNG.
    {
        warpsim::Event ev;
        ev.ts = warpsim::TimeStamp{1, 1};
        ev.src = 7;
        ev.dst = 8;
        ev.uid = 123;
        ev.payload.kind = 1;

        const std::uint64_t seed = 555;
        const warpsim::LPId lp = 9;
        const std::uint32_t stream = 2;
        const std::uint32_t draw = 4;

        const std::uint64_t a = warpsim::modeling::rng_u64(seed, lp, ev, stream, draw);
        const std::uint64_t b = warpsim::rng_u64(seed, lp, stream, ev.src, ev.uid, draw);
        assert(a == b);
    }

    return 0;
}
