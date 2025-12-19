#pragma once

#include "state_store.hpp"

#include <mpi.h>

#include <cstdint>
#include <mutex>

namespace warpsim
{
    namespace detail
    {
        struct PackedTimeStamp
        {
            std::uint64_t time;
            std::uint64_t sequence;
        };

        inline void mpi_ts_min_op(void *invec, void *inoutvec, int *len, MPI_Datatype *datatype)
        {
            (void)datatype;
            auto *in = static_cast<PackedTimeStamp *>(invec);
            auto *inout = static_cast<PackedTimeStamp *>(inoutvec);
            for (int i = 0; i < *len; ++i)
            {
                const auto a = in[i];
                const auto b = inout[i];
                const bool aLess = (a.time < b.time) || ((a.time == b.time) && (a.sequence < b.sequence));
                if (aLess)
                {
                    inout[i] = a;
                }
            }
        }

        inline void ensure_ts_min_op(MPI_Datatype &tsType, MPI_Op &tsMinOp)
        {
            static std::once_flag once;
            std::call_once(once,
                           [&]()
                           {
                               MPI_Type_contiguous(2, MPI_UINT64_T, &tsType);
                               MPI_Type_commit(&tsType);
                               MPI_Op_create(&mpi_ts_min_op, /*commute=*/1, &tsMinOp);
                           });
        }
    }

    // Lexicographic min Allreduce for TimeStamp {time, sequence}.
    // Intended to be used as SimulationConfig::gvtReduceMin.
    inline TimeStamp mpi_allreduce_min_timestamp(MPI_Comm comm, TimeStamp local)
    {
        static MPI_Datatype tsType = MPI_DATATYPE_NULL;
        static MPI_Op tsMinOp = MPI_OP_NULL;
        detail::ensure_ts_min_op(tsType, tsMinOp);

        detail::PackedTimeStamp in{local.time, local.sequence};
        detail::PackedTimeStamp out{0, 0};

        const int rc = MPI_Allreduce(&in, &out, 1, tsType, tsMinOp, comm);
        if (rc != MPI_SUCCESS)
        {
            MPI_Abort(comm, 3);
        }
        return TimeStamp{out.time, out.sequence};
    }

    // Allreduce logical-OR for "any rank has work" termination coordination.
    inline bool mpi_allreduce_any_work(MPI_Comm comm, bool localHasWork)
    {
        int in = localHasWork ? 1 : 0;
        int out = 0;
        const int rc = MPI_Allreduce(&in, &out, 1, MPI_INT, MPI_LOR, comm);
        if (rc != MPI_SUCCESS)
        {
            MPI_Abort(comm, 4);
        }
        return out != 0;
    }

    // Allreduce sum for int64 counters.
    inline std::int64_t mpi_allreduce_sum_i64(MPI_Comm comm, std::int64_t local)
    {
        std::int64_t out = 0;
        const int rc = MPI_Allreduce(&local, &out, 1, MPI_INT64_T, MPI_SUM, comm);
        if (rc != MPI_SUCCESS)
        {
            MPI_Abort(comm, 5);
        }
        return out;
    }
}
