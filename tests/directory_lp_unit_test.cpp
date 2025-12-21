#include "directory_lp.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
    struct UpdateArgs
    {
        warpsim::EntityId entity = 0;
        warpsim::LPId newOwner = 0;
        std::uint64_t value = 0;
    };

    static constexpr std::uint32_t KindUpdate = 9001;
    static constexpr std::uint32_t KindInstall = 9002;
    static constexpr std::uint32_t KindFossilCollect = 9003;
    static constexpr warpsim::EntityId kDirState = 0xD1EC7000ULL;

    class CaptureCtx final : public warpsim::IEventContext
    {
    public:
        void send(warpsim::Event ev) override { sent.push_back(std::move(ev)); }

        void request_write(warpsim::EntityId id) override { requestedWrites.push_back(id); }

        void emit_committed(warpsim::TimeStamp, warpsim::Payload) override {}

        std::vector<warpsim::Event> sent;
        std::vector<warpsim::EntityId> requestedWrites;
    };

    warpsim::Payload pack_update(UpdateArgs a)
    {
        warpsim::Payload p;
        p.kind = KindUpdate;
        p.bytes = warpsim::bytes_from_trivially_copyable(a);
        return p;
    }

    warpsim::Payload pack_fossil_collect(warpsim::TimeStamp gvt)
    {
        warpsim::Payload p;
        p.kind = KindFossilCollect;
        p.bytes = warpsim::bytes_from_trivially_copyable(gvt);
        return p;
    }
}

int main()
{
    // Forwarding semantics: uid/sequence reset, src rewritten, dst determined by owner-at-time.
    {
        warpsim::DirectoryLPConfig cfg;
        cfg.id = 10;
        cfg.stateEntityId = kDirState;
        cfg.updateOwnerKind = KindUpdate;
        cfg.decodeUpdate = [](const warpsim::Event &ev) -> warpsim::DirectoryUpdate
        {
            if (ev.payload.bytes.size() != sizeof(UpdateArgs))
            {
                throw std::runtime_error("bad update args");
            }
            UpdateArgs a{};
            std::memcpy(&a, ev.payload.bytes.data(), sizeof(UpdateArgs));

            warpsim::DirectoryUpdate u;
            u.entity = a.entity;
            u.newOwner = a.newOwner;
            u.sendInstall = true;
            u.installPayload.kind = KindInstall;
            u.installPayload.bytes = warpsim::bytes_from_trivially_copyable(a.value);
            return u;
        };
        cfg.defaultOwner = [](warpsim::EntityId, warpsim::TimeStamp) -> warpsim::LPId
        { return 7; };

        warpsim::DirectoryLP dir(cfg);
        CaptureCtx ctx;

        // No history -> default owner.
        warpsim::Event in;
        in.ts = warpsim::TimeStamp{10, 123};
        in.src = 1;
        in.dst = 10;
        in.target = 0xAAAAULL;
        in.uid = 999;
        in.payload.kind = 111;
        dir.on_event(in, ctx);

        assert(ctx.sent.size() == 1);
        const auto &fwd = ctx.sent[0];
        assert(fwd.src == 10);
        assert(fwd.dst == 7);
        assert(fwd.target == in.target);
        assert(fwd.payload.kind == in.payload.kind);
        assert(fwd.uid == 0);
        assert(fwd.ts.time == in.ts.time);
        assert(fwd.ts.sequence == 0);

        ctx.sent.clear();
        ctx.requestedWrites.clear();

        // Update owner at (5,1) -> should request_write + emit install.
        UpdateArgs a{in.target, /*newOwner=*/3, /*value=*/1234};
        warpsim::Event upd;
        upd.ts = warpsim::TimeStamp{5, 1};
        upd.src = 2;
        upd.dst = 10;
        upd.target = in.target;
        upd.payload = pack_update(a);
        dir.on_event(upd, ctx);

        assert(ctx.requestedWrites.size() == 1);
        assert(ctx.requestedWrites[0] == kDirState);

        assert(ctx.sent.size() == 1);
        const auto &install = ctx.sent[0];
        assert(install.ts.time == upd.ts.time);
        assert(install.ts.sequence == upd.ts.sequence);
        assert(install.src == 10);
        assert(install.dst == 3);
        assert(install.target == in.target);
        assert(install.payload.kind == KindInstall);

        ctx.sent.clear();

        // After update, forwarding at later time should go to new owner.
        warpsim::Event in2 = in;
        in2.ts = warpsim::TimeStamp{6, 0};
        dir.on_event(in2, ctx);
        assert(ctx.sent.size() == 1);
        assert(ctx.sent[0].dst == 3);
    }

    // Fossil collection pruning: keep correctness for timestamps >= GVT.
    {
        warpsim::DirectoryLPConfig cfg;
        cfg.id = 20;
        cfg.stateEntityId = kDirState;
        cfg.updateOwnerKind = KindUpdate;
        cfg.decodeUpdate = [](const warpsim::Event &ev) -> warpsim::DirectoryUpdate
        {
            UpdateArgs a{};
            if (ev.payload.bytes.size() != sizeof(UpdateArgs))
            {
                throw std::runtime_error("bad update args");
            }
            std::memcpy(&a, ev.payload.bytes.data(), sizeof(UpdateArgs));
            warpsim::DirectoryUpdate u;
            u.entity = a.entity;
            u.newOwner = a.newOwner;
            u.sendInstall = false;
            return u;
        };
        cfg.fossilCollectKind = KindFossilCollect;
        cfg.decodeFossilCollect = [](const warpsim::Event &ev) -> warpsim::TimeStamp
        {
            if (ev.payload.bytes.size() != sizeof(warpsim::TimeStamp))
            {
                throw std::runtime_error("bad fossil payload");
            }
            warpsim::TimeStamp gvt{};
            std::memcpy(&gvt, ev.payload.bytes.data(), sizeof(warpsim::TimeStamp));
            return gvt;
        };
        cfg.defaultOwner = [](warpsim::EntityId, warpsim::TimeStamp) -> warpsim::LPId
        { return 0; };

        warpsim::DirectoryLP dir(cfg);
        CaptureCtx ctx;

        const warpsim::EntityId ent = 0xBBBBULL;

        // Ownership timeline: (1)->0, (3)->1, (5)->2
        {
            warpsim::Event u;
            u.src = 1;
            u.dst = 20;
            u.target = ent;

            u.ts = warpsim::TimeStamp{1, 1};
            u.payload = pack_update(UpdateArgs{ent, 0, 0});
            dir.on_event(u, ctx);

            u.ts = warpsim::TimeStamp{3, 1};
            u.payload = pack_update(UpdateArgs{ent, 1, 0});
            dir.on_event(u, ctx);

            u.ts = warpsim::TimeStamp{5, 1};
            u.payload = pack_update(UpdateArgs{ent, 2, 0});
            dir.on_event(u, ctx);
        }

        ctx.sent.clear();
        ctx.requestedWrites.clear();

        // Prune at GVT=(4,0). Should keep correctness for ts >= (4,0).
        {
            warpsim::Event fc;
            fc.ts = warpsim::TimeStamp{4, 0};
            fc.src = 99;
            fc.dst = 20;
            fc.payload = pack_fossil_collect(warpsim::TimeStamp{4, 0});
            dir.on_event(fc, ctx);
        }

        assert(!ctx.requestedWrites.empty());
        assert(ctx.requestedWrites.back() == kDirState);

        ctx.sent.clear();

        // Query at ts=(4,0) -> owner should be 1.
        {
            warpsim::Event q;
            q.ts = warpsim::TimeStamp{4, 0};
            q.src = 2;
            q.dst = 20;
            q.target = ent;
            q.payload.kind = 1;
            dir.on_event(q, ctx);
            assert(ctx.sent.size() == 1);
            assert(ctx.sent[0].dst == 1);
        }

        ctx.sent.clear();

        // Query at ts=(5,1) -> owner should be 2.
        {
            warpsim::Event q;
            q.ts = warpsim::TimeStamp{5, 1};
            q.src = 2;
            q.dst = 20;
            q.target = ent;
            q.payload.kind = 1;
            dir.on_event(q, ctx);
            assert(ctx.sent.size() == 1);
            assert(ctx.sent[0].dst == 2);
        }
    }

    return 0;
}
