/*
Purpose: Minimal MPI sanity check.

What this tests: The test harness can start MPI, determine rank/size, and enforce that
the MPI test suite is running with at least two ranks.
*/

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

    check(size >= 2, "MPI smoke test requires at least 2 ranks");
    check(rank >= 0, "MPI_Comm_rank returned invalid rank");

    MPI_Finalize();
    return 0;
}
