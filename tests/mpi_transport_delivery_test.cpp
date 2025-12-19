#include "mpi_transport.hpp"

#include <mpi.h>

#include <cstdio>

namespace
{
    [[noreturn]] void fail(const char *msg)
    {
        std::fprintf(stderr, "%s\n", msg);
        MPI_Abort(MPI_COMM_WORLD, 1);
        std::abort();
    }

    void check(bool ok, const char *msg)
    {
        if (!ok)
        {
            fail(msg);
        }
    }
}

int main(int argc, char **argv)
{
    int rc = MPI_Init(&argc, &argv);
    if (rc != MPI_SUCCESS)
    {
        return 2;
    }

    int rank = -1;
    int size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    check(size >= 2, "MPI transport test requires at least 2 ranks");

    warpsim::MpiTransport tx(MPI_COMM_WORLD, /*tag=*/0);

    if (rank == 0)
    {
        warpsim::WireMessage msg;
        msg.kind = warpsim::MessageKind::Event;
        msg.srcRank = 0;
        msg.dstRank = 1;
        msg.bytes = {std::byte{0xAB}, std::byte{0xCD}};
        tx.send(std::move(msg));
    }

    if (rank == 1)
    {
        bool got = false;
        for (int i = 0; i < 200000; ++i)
        {
            auto m = tx.poll();
            if (!m)
            {
                continue;
            }
            check(m->kind == warpsim::MessageKind::Event, "wrong message kind");
            check(m->srcRank == 0, "wrong srcRank");
            check(m->dstRank == 1, "wrong dstRank");
            check(m->bytes.size() == 2, "wrong payload size");
            check(m->bytes[0] == std::byte{0xAB} && m->bytes[1] == std::byte{0xCD}, "wrong payload bytes");
            got = true;
            break;
        }
        check(got, "timed out waiting for message");
    }

    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    return 0;
}
