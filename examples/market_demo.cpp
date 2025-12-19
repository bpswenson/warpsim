#include "event_dispatcher.hpp"
#include "simulation.hpp"
#include "transport.hpp"
#include "world.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <span>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
    using EventTypeId = std::uint32_t;

    enum : EventTypeId
    {
        Trader_Fill = 5001,
        Exchange_PlaceOrder = 5101,
        Exchange_Match = 5102,
    };

    enum class Side : std::uint32_t
    {
        Bid = 0,
        Ask = 1,
    };

    struct PlaceOrderArgs
    {
        warpsim::EntityId trader = 0;
        std::uint32_t side = 0;
        std::uint32_t qty = 0;
        std::uint64_t price = 0;
    };

    struct MatchArgs
    {
        std::uint32_t maxTrades = 16;
    };

    struct FillArgs
    {
        std::int64_t cashDelta = 0;
        std::int64_t posDelta = 0;
    };

    struct Trader
    {
        std::uint64_t cash = 0;
        std::int64_t position = 0;
    };

    struct Order
    {
        std::uint64_t id = 0;
        warpsim::EntityId trader = 0;
        std::uint32_t side = 0;
        std::uint32_t qty = 0;
        std::uint64_t price = 0;
    };

    struct OrderBook
    {
        std::uint64_t nextOrderId = 1;
        std::uint64_t lastTradePrice = 0;
        std::vector<Order> bids;
        std::vector<Order> asks;
    };

    struct AuditEntry
    {
        std::uint64_t t = 0;
        char msg[32]{};
        std::uint64_t a = 0;
        std::uint64_t b = 0;
    };

    struct AuditLog
    {
        std::vector<AuditEntry> entries;
    };

    static constexpr warpsim::EntityTypeId TraderType = 6001;
    static constexpr warpsim::EntityTypeId BookType = 6002;
    static constexpr warpsim::EntityTypeId AuditType = 6003;

    static constexpr warpsim::EntityId make_id(warpsim::LPId owner, std::uint32_t local)
    {
        return (static_cast<warpsim::EntityId>(owner) << 32) | static_cast<warpsim::EntityId>(local);
    }

    // Demo transport: returns at most one message per poll_transport() call.
    // This simulates "late" arrival so the kernel must handle stragglers/rollback.
    class ThrottledInProcTransport final : public warpsim::ITransport
    {
    public:
        void send(warpsim::WireMessage msg) override
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_queue.push_back(std::move(msg));
        }

        std::optional<warpsim::WireMessage> poll() override
        {
            std::lock_guard<std::mutex> lk(m_mu);

            if (m_throttle)
            {
                m_throttle = false;
                return std::nullopt;
            }

            if (m_queue.empty())
            {
                return std::nullopt;
            }

            auto msg = std::move(m_queue.front());
            m_queue.pop_front();
            m_throttle = true;
            return msg;
        }

        bool has_pending() const override
        {
            std::lock_guard<std::mutex> lk(m_mu);
            return !m_queue.empty();
        }

    private:
        mutable std::mutex m_mu;
        std::deque<warpsim::WireMessage> m_queue;
        bool m_throttle = false;
    };

    // Helpers for compact, deterministic serialization.
    template <class T>
    void append_bytes(warpsim::ByteBuffer &out, const T &pod)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::byte *p = reinterpret_cast<const std::byte *>(&pod);
        out.insert(out.end(), p, p + sizeof(T));
    }

    inline void append_span(warpsim::ByteBuffer &out, std::span<const std::byte> bytes)
    {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    template <class T>
    void append_vec(warpsim::ByteBuffer &out, const std::vector<T> &v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t n = static_cast<std::uint32_t>(v.size());
        append_bytes(out, n);
        if (!v.empty())
        {
            append_span(out, std::span<const std::byte>(reinterpret_cast<const std::byte *>(v.data()), v.size() * sizeof(T)));
        }
    }

    template <class T>
    void read_bytes(std::span<const std::byte> in, std::size_t &off, T &pod)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (off + sizeof(T) > in.size())
        {
            throw std::runtime_error("deserialize: truncated");
        }
        std::memcpy(&pod, in.data() + off, sizeof(T));
        off += sizeof(T);
    }

    template <class T>
    void read_vec(std::span<const std::byte> in, std::size_t &off, std::vector<T> &v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        std::uint32_t n = 0;
        read_bytes(in, off, n);
        const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(T);
        if (off + bytes > in.size())
        {
            throw std::runtime_error("deserialize: truncated vec");
        }
        v.resize(n);
        if (bytes != 0)
        {
            std::memcpy(v.data(), in.data() + off, bytes);
            off += bytes;
        }
    }

    class ExchangeLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit ExchangeLP(warpsim::LPId id, warpsim::LPId traderLp) : m_id(id), m_traderLp(traderLp)
        {
            m_types.register_trivial<Trader>(TraderType);

            // OrderBook (dynamic vectors): custom ops.
            warpsim::EntityTypeOps bookOps;
            bookOps.type = BookType;
            bookOps.create = []() -> void *
            { return new OrderBook{}; };
            bookOps.destroy = [](void *p)
            { delete static_cast<OrderBook *>(p); };
            bookOps.serialize = [](const void *p) -> warpsim::ByteBuffer
            {
                const auto &b = *static_cast<const OrderBook *>(p);
                warpsim::ByteBuffer out;
                append_bytes(out, b.nextOrderId);
                append_bytes(out, b.lastTradePrice);
                append_vec(out, b.bids);
                append_vec(out, b.asks);
                return out;
            };
            bookOps.deserialize = [](void *p, std::span<const std::byte> bytes)
            {
                auto &b = *static_cast<OrderBook *>(p);
                std::size_t off = 0;
                read_bytes(bytes, off, b.nextOrderId);
                read_bytes(bytes, off, b.lastTradePrice);
                read_vec(bytes, off, b.bids);
                read_vec(bytes, off, b.asks);
            };
            m_types.register_type(std::move(bookOps));

            // AuditLog (dynamic vector): custom ops.
            warpsim::EntityTypeOps auditOps;
            auditOps.type = AuditType;
            auditOps.create = []() -> void *
            { return new AuditLog{}; };
            auditOps.destroy = [](void *p)
            { delete static_cast<AuditLog *>(p); };
            auditOps.serialize = [](const void *p) -> warpsim::ByteBuffer
            {
                const auto &a = *static_cast<const AuditLog *>(p);
                warpsim::ByteBuffer out;
                append_vec(out, a.entries);
                return out;
            };
            auditOps.deserialize = [](void *p, std::span<const std::byte> bytes)
            {
                auto &a = *static_cast<AuditLog *>(p);
                std::size_t off = 0;
                read_vec(bytes, off, a.entries);
            };
            m_types.register_type(std::move(auditOps));

            m_dispatcher.register_trivial<PlaceOrderArgs>(Exchange_PlaceOrder,
                                                          [this](warpsim::EntityId target, const PlaceOrderArgs &args, const warpsim::Event &ev, warpsim::IEventContext &ctx, warpsim::WorldTxn &world)
                                                          {
                                                              (void)ctx;
                                                              auto &book = world.write<OrderBook>(target, BookType);
                                                              Order o;
                                                              o.id = book.nextOrderId++;
                                                              o.trader = args.trader;
                                                              o.side = args.side;
                                                              o.qty = args.qty;
                                                              o.price = args.price;

                                                              if (static_cast<Side>(args.side) == Side::Bid)
                                                              {
                                                                  book.bids.push_back(o);
                                                              }
                                                              else
                                                              {
                                                                  book.asks.push_back(o);
                                                              }

                                                              // Record audit.
                                                              auto &audit = world.write<AuditLog>(m_auditId, AuditType);
                                                              AuditEntry e{};
                                                              e.t = ev.ts.time;
                                                              std::snprintf(e.msg, sizeof(e.msg), "%s", "place");
                                                              e.a = o.id;
                                                              e.b = o.price;
                                                              audit.entries.push_back(e);
                                                          });

            m_dispatcher.register_trivial<MatchArgs>(Exchange_Match,
                                                     [this](warpsim::EntityId target, const MatchArgs &args, const warpsim::Event &ev, warpsim::IEventContext &ctx, warpsim::WorldTxn &world)
                                                     {
                                                         (void)ctx;
                                                         auto &book = world.write<OrderBook>(target, BookType);

                                                         auto bestBid = [](const Order &a, const Order &b)
                                                         {
                                                             if (a.price != b.price)
                                                             {
                                                                 return a.price > b.price;
                                                             }
                                                             return a.id < b.id;
                                                         };
                                                         auto bestAsk = [](const Order &a, const Order &b)
                                                         {
                                                             if (a.price != b.price)
                                                             {
                                                                 return a.price < b.price;
                                                             }
                                                             return a.id < b.id;
                                                         };

                                                         std::uint32_t trades = 0;
                                                         while (trades < args.maxTrades && !book.bids.empty() && !book.asks.empty())
                                                         {
                                                             auto itBid = std::max_element(book.bids.begin(), book.bids.end(), [&](const Order &x, const Order &y)
                                                                                           { return bestBid(y, x); });
                                                             auto itAsk = std::min_element(book.asks.begin(), book.asks.end(), [&](const Order &x, const Order &y)
                                                                                           { return bestAsk(x, y); });

                                                             if (itBid == book.bids.end() || itAsk == book.asks.end())
                                                             {
                                                                 break;
                                                             }
                                                             if (itBid->price < itAsk->price)
                                                             {
                                                                 break;
                                                             }

                                                             const std::uint32_t qty = std::min(itBid->qty, itAsk->qty);
                                                             const std::uint64_t price = itAsk->price; // trade at ask
                                                             book.lastTradePrice = price;

                                                             // Buyer loses cash, gains position.
                                                             send_fill_(ctx, ev.ts, itBid->trader, -static_cast<std::int64_t>(qty) * static_cast<std::int64_t>(price), +static_cast<std::int64_t>(qty));
                                                             // Seller gains cash, loses position.
                                                             send_fill_(ctx, ev.ts, itAsk->trader, +static_cast<std::int64_t>(qty) * static_cast<std::int64_t>(price), -static_cast<std::int64_t>(qty));

                                                             // Adjust order quantities.
                                                             itBid->qty -= qty;
                                                             itAsk->qty -= qty;
                                                             if (itBid->qty == 0)
                                                             {
                                                                 book.bids.erase(itBid);
                                                             }
                                                             if (itAsk->qty == 0)
                                                             {
                                                                 book.asks.erase(itAsk);
                                                             }

                                                             auto &audit = world.write<AuditLog>(m_auditId, AuditType);
                                                             AuditEntry a{};
                                                             a.t = ev.ts.time;
                                                             std::snprintf(a.msg, sizeof(a.msg), "%s", "trade");
                                                             a.a = price;
                                                             a.b = qty;
                                                             audit.entries.push_back(a);

                                                             ++trades;
                                                         }
                                                     });
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &) override
        {
            // Exchange owns a book + audit log.
            m_bookId = make_id(m_id, 1);
            m_auditId = make_id(m_id, 2);
            m_world.emplace<OrderBook>(m_bookId, BookType, OrderBook{});
            m_world.emplace<AuditLog>(m_auditId, AuditType, AuditLog{});
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            warpsim::WorldTxn txn(m_world, ctx);
            m_dispatcher.dispatch(ev, ctx, txn);
        }

        bool supports_entity_snapshots() const override { return true; }

        warpsim::ByteBuffer save_entity(warpsim::EntityId id) const override
        {
            return m_world.serialize_entity(id);
        }

        void load_entity(warpsim::EntityId id, std::span<const std::byte> bytes) override
        {
            m_world.deserialize_entity(id, bytes);
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

        std::uint64_t last_trade_price() const
        {
            return m_world.read<OrderBook>(m_bookId, BookType).lastTradePrice;
        }

        std::size_t audit_count() const
        {
            return m_world.read<AuditLog>(m_auditId, AuditType).entries.size();
        }

        std::size_t remaining_orders() const
        {
            const auto &b = m_world.read<OrderBook>(m_bookId, BookType);
            return b.bids.size() + b.asks.size();
        }

        warpsim::EntityId book_id() const { return m_bookId; }

    private:
        void send_fill_(warpsim::IEventContext &ctx, warpsim::TimeStamp ts, warpsim::EntityId trader, std::int64_t cashDelta, std::int64_t posDelta)
        {
            warpsim::Event ev;
            ev.ts = ts;
            ev.src = m_id;
            ev.dst = m_traderLp;
            ev.target = trader;

            FillArgs a;
            a.cashDelta = cashDelta;
            a.posDelta = posDelta;
            ev.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Trader_Fill, a);
            ctx.send(std::move(ev));
        }

        warpsim::LPId m_id = 0;
        warpsim::LPId m_traderLp = 0;

        warpsim::EntityTypeRegistry m_types;
        warpsim::World m_world{m_types};
        warpsim::EventDispatcher<warpsim::WorldTxn> m_dispatcher;

        warpsim::EntityId m_bookId = 0;
        warpsim::EntityId m_auditId = 0;
    };

    class TraderLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit TraderLP(warpsim::LPId id, warpsim::LPId exchange) : m_id(id), m_exchange(exchange)
        {
            m_types.register_trivial<Trader>(TraderType);

            m_dispatcher.register_trivial<FillArgs>(Trader_Fill,
                                                    [this](warpsim::EntityId target, const FillArgs &args, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                    {
                                                        auto &t = world.write<Trader>(target, TraderType);
                                                        const std::int64_t cashDelta = args.cashDelta;
                                                        t.cash = static_cast<std::uint64_t>(static_cast<std::int64_t>(t.cash) + cashDelta);
                                                        t.position += args.posDelta;
                                                    });
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Two traders live on this LP.
            m_traderA = make_id(m_id, 1);
            m_traderB = make_id(m_id, 2);
            m_world.emplace<Trader>(m_traderA, TraderType, Trader{.cash = 10000, .position = 0});
            m_world.emplace<Trader>(m_traderB, TraderType, Trader{.cash = 10000, .position = 0});

            // Exchange book EntityId is deterministic.
            m_exchangeBook = make_id(m_exchange, 1);

            // Send orders with intentionally out-of-order timestamps.
            // The throttled transport will delay delivery, producing stragglers/rollback.
            const auto send_order = [&](warpsim::SimTime t, Side side, std::uint32_t qty, std::uint64_t price, warpsim::EntityId trader)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{t, 0};
                ev.src = m_id;
                ev.dst = m_exchange;
                ev.target = m_exchangeBook; // set later by main

                PlaceOrderArgs a;
                a.trader = trader;
                a.side = static_cast<std::uint32_t>(side);
                a.qty = qty;
                a.price = price;
                ev.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Exchange_PlaceOrder, a);
                sink.send(std::move(ev));
            };

            // These are queued in this order (not timestamp order).
            send_order(10, Side::Bid, 10, 100, m_traderA);
            send_order(12, Side::Ask, 10, 99, m_traderB);
            send_order(8, Side::Ask, 10, 100, m_traderB);
            // Straggler: earlier timestamp arrives after some later processing.
            send_order(5, Side::Bid, 10, 101, m_traderA);
            send_order(6, Side::Ask, 10, 100, m_traderB);

            // Schedule a match at t=15.
            warpsim::Event match;
            match.ts = warpsim::TimeStamp{15, 0};
            match.src = m_id;
            match.dst = m_exchange;
            match.target = m_exchangeBook;
            match.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Exchange_Match, MatchArgs{.maxTrades = 16});
            sink.send(std::move(match));
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            warpsim::WorldTxn txn(m_world, ctx);
            m_dispatcher.dispatch(ev, ctx, txn);
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

        bool supports_entity_snapshots() const override { return true; }

        warpsim::ByteBuffer save_entity(warpsim::EntityId id) const override
        {
            return m_world.serialize_entity(id);
        }

        void load_entity(warpsim::EntityId id, std::span<const std::byte> bytes) override
        {
            m_world.deserialize_entity(id, bytes);
        }

        void set_exchange_book(warpsim::EntityId book)
        {
            m_exchangeBook = book;
        }

        std::uint64_t cash_a() const { return m_world.read<Trader>(m_traderA, TraderType).cash; }
        std::int64_t pos_a() const { return m_world.read<Trader>(m_traderA, TraderType).position; }
        std::uint64_t cash_b() const { return m_world.read<Trader>(m_traderB, TraderType).cash; }
        std::int64_t pos_b() const { return m_world.read<Trader>(m_traderB, TraderType).position; }

    private:
        warpsim::LPId m_id = 0;
        warpsim::LPId m_exchange = 0;

        warpsim::EntityTypeRegistry m_types;
        warpsim::World m_world{m_types};
        warpsim::EventDispatcher<warpsim::WorldTxn> m_dispatcher;

        warpsim::EntityId m_traderA = 0;
        warpsim::EntityId m_traderB = 0;
        warpsim::EntityId m_exchangeBook = 0;
    };
}

int main()
{
    auto transport = std::make_shared<ThrottledInProcTransport>();

    warpsim::Simulation sim(warpsim::SimulationConfig{.rank = 0}, transport);

    auto exchange = std::make_unique<ExchangeLP>(2, /*traderLp=*/1);
    auto traders = std::make_unique<TraderLP>(1, /*exchange=*/2);

    auto *exchangeP = exchange.get();
    auto *tradersP = traders.get();

    sim.add_lp(std::move(traders));
    sim.add_lp(std::move(exchange));

    sim.run();

    // Expected: two trades happen at prices 99 and 100, qty 10 each.
    // Trader A buys 20, spends 1990. Trader B sells 20, earns 1990.
    assert(tradersP->pos_a() == 20);
    assert(tradersP->cash_a() == 10000 - 1990);

    assert(tradersP->pos_b() == -20);
    assert(tradersP->cash_b() == 10000 + 1990);

    assert(exchangeP->last_trade_price() == 100);
    assert(exchangeP->audit_count() >= 2); // place/trade entries

    std::cout << "market_demo ok\n";
    std::cout << "A cash=" << tradersP->cash_a() << " pos=" << tradersP->pos_a() << "\n";
    std::cout << "B cash=" << tradersP->cash_b() << " pos=" << tradersP->pos_b() << "\n";
    std::cout << "exchange last_price=" << exchangeP->last_trade_price() << " audit=" << exchangeP->audit_count() << " remaining_orders=" << exchangeP->remaining_orders() << "\n";
    return 0;
}
