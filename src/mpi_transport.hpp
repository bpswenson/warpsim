#pragma once

#include "transport.hpp"
#include "wire.hpp"

#include <mpi.h>

#include <cstdint>
#include <utility>
#include <vector>
#include <optional>
#include <span>
#include <stdexcept>

namespace warpsim
{
    // Minimal MPI transport for point-to-point WireMessage delivery.
    // Intended for tests and experimentation; not tuned for performance.
    class MpiTransport final : public ITransport
    {
    public:
        explicit MpiTransport(MPI_Comm comm = MPI_COMM_WORLD, int tag = 0)
            : m_comm(comm), m_tag(tag)
        {
            if (m_comm == MPI_COMM_NULL)
            {
                throw std::runtime_error("MpiTransport: null communicator");
            }
        }

        void send(WireMessage msg) override
        {
            drain_completed_sends_();

            WireWriter w;
            w.write_u8(static_cast<std::uint8_t>(msg.kind));
            w.write_u32(msg.srcRank);
            w.write_u32(msg.dstRank);
            w.write_bytes(std::span<const std::byte>(msg.bytes.data(), msg.bytes.size()));

            ByteBuffer buf = w.take();
            const int dst = static_cast<int>(msg.dstRank);

            PendingSend pending;
            pending.buf = std::move(buf);
            pending.req = MPI_REQUEST_NULL;

            const int rc = MPI_Isend(pending.buf.data(), static_cast<int>(pending.buf.size()), MPI_BYTE, dst, m_tag, m_comm, &pending.req);
            if (rc != MPI_SUCCESS)
            {
                throw std::runtime_error("MpiTransport: MPI_Isend failed");
            }
            m_pendingSends.push_back(std::move(pending));
        }

        std::optional<WireMessage> poll() override
        {
            drain_completed_sends_();

            MPI_Status status;
            int flag = 0;
            int rc = MPI_Iprobe(MPI_ANY_SOURCE, m_tag, m_comm, &flag, &status);
            if (rc != MPI_SUCCESS)
            {
                throw std::runtime_error("MpiTransport: MPI_Iprobe failed");
            }
            if (!flag)
            {
                return std::nullopt;
            }

            int count = 0;
            rc = MPI_Get_count(&status, MPI_BYTE, &count);
            if (rc != MPI_SUCCESS || count <= 0)
            {
                throw std::runtime_error("MpiTransport: MPI_Get_count failed");
            }

            ByteBuffer buf(static_cast<std::size_t>(count));
            rc = MPI_Recv(buf.data(), count, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, m_comm, MPI_STATUS_IGNORE);
            if (rc != MPI_SUCCESS)
            {
                throw std::runtime_error("MpiTransport: MPI_Recv failed");
            }

            WireReader r(std::span<const std::byte>(buf.data(), buf.size()));

            WireMessage out;
            out.kind = static_cast<MessageKind>(r.read_u8());
            out.srcRank = r.read_u32();
            out.dstRank = r.read_u32();
            out.bytes = r.read_bytes();
            return out;
        }

        bool has_pending() const override
        {
            drain_completed_sends_();

            MPI_Status status;
            int flag = 0;
            int rc = MPI_Iprobe(MPI_ANY_SOURCE, m_tag, m_comm, &flag, &status);
            if (rc != MPI_SUCCESS)
            {
                return false;
            }

            if (flag != 0)
            {
                return true;
            }
            return !m_pendingSends.empty();
        }

    private:
        struct PendingSend
        {
            ByteBuffer buf;
            MPI_Request req = MPI_REQUEST_NULL;
        };

        void drain_completed_sends_() const
        {
            for (std::size_t i = 0; i < m_pendingSends.size();)
            {
                int done = 0;
                const int rc = MPI_Test(&m_pendingSends[i].req, &done, MPI_STATUS_IGNORE);
                if (rc != MPI_SUCCESS)
                {
                    throw std::runtime_error("MpiTransport: MPI_Test failed");
                }
                if (done)
                {
                    m_pendingSends[i] = std::move(m_pendingSends.back());
                    m_pendingSends.pop_back();
                    continue;
                }
                ++i;
            }
        }

        MPI_Comm m_comm;
        int m_tag = 0;

        // Mutable because has_pending() and poll() are logically const but need to
        // advance completion of in-flight nonblocking sends.
        mutable std::vector<PendingSend> m_pendingSends;
    };
}
