#pragma once

#include "transport.hpp"
#include "wire.hpp"

#include <mpi.h>

#include <cstdint>
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
            WireWriter w;
            w.write_u8(static_cast<std::uint8_t>(msg.kind));
            w.write_u32(msg.srcRank);
            w.write_u32(msg.dstRank);
            w.write_bytes(std::span<const std::byte>(msg.bytes.data(), msg.bytes.size()));

            ByteBuffer buf = w.take();
            const int dst = static_cast<int>(msg.dstRank);

            int rc = MPI_Send(buf.data(), static_cast<int>(buf.size()), MPI_BYTE, dst, m_tag, m_comm);
            if (rc != MPI_SUCCESS)
            {
                throw std::runtime_error("MpiTransport: MPI_Send failed");
            }
        }

        std::optional<WireMessage> poll() override
        {
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
            MPI_Status status;
            int flag = 0;
            int rc = MPI_Iprobe(MPI_ANY_SOURCE, m_tag, m_comm, &flag, &status);
            if (rc != MPI_SUCCESS)
            {
                return false;
            }
            return flag != 0;
        }

    private:
        MPI_Comm m_comm;
        int m_tag = 0;
    };
}
