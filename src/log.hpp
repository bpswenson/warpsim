#pragma once

#include "common.hpp"
#include "state_store.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace warpsim
{
    enum class LogLevel : std::uint8_t
    {
        Error = 0,
        Warn = 1,
        Info = 2,
        Debug = 3,
        Trace = 4,
        Off = 255,
    };

    inline const char *log_level_name(LogLevel lvl) noexcept
    {
        switch (lvl)
        {
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Off:
            return "OFF";
        }
        return "UNKNOWN";
    }

    inline bool log_enabled(LogLevel configured, LogLevel msg) noexcept
    {
        if (configured == LogLevel::Off)
        {
            return false;
        }
        return static_cast<std::uint8_t>(msg) <= static_cast<std::uint8_t>(configured);
    }

    class Logger
    {
    public:
        static Logger &instance()
        {
            static Logger g;
            return g;
        }

        void set_level(LogLevel lvl)
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_level = lvl;
        }

        LogLevel level() const noexcept { return m_level; }

        void set_sink(FILE *f)
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_sink = f;
        }

        void logf(LogLevel lvl, RankId rank, LPId lp, TimeStamp ts, const char *fmt, ...)
        {
            if (!log_enabled(m_level, lvl))
            {
                return;
            }

            char buf[1024];
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);

            std::lock_guard<std::mutex> lk(m_mu);
            if (!m_sink)
            {
                return;
            }
            std::fprintf(m_sink, "[%s][rank=%u][lp=%u][t=%llu.%llu] %s\n",
                         log_level_name(lvl),
                         static_cast<unsigned>(rank),
                         static_cast<unsigned>(lp),
                         static_cast<unsigned long long>(ts.time),
                         static_cast<unsigned long long>(ts.sequence),
                         buf);
            std::fflush(m_sink);
        }

    private:
        Logger() = default;

        mutable std::mutex m_mu;
        LogLevel m_level = LogLevel::Off;
        FILE *m_sink = stderr;
    };
}
