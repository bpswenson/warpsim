#pragma once

#include "common.hpp"

#include <stdexcept>

namespace warpsim
{
    class WireWriter
    {
    public:
        void write_u8(std::uint8_t v) { m_buf.push_back(static_cast<std::byte>(v)); }
        void write_u32(std::uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
            {
                m_buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
            }
        }
        void write_u64(std::uint64_t v)
        {
            for (int i = 0; i < 8; ++i)
            {
                m_buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
            }
        }
        void write_bytes(std::span<const std::byte> bytes)
        {
            write_u32(static_cast<std::uint32_t>(bytes.size()));
            m_buf.insert(m_buf.end(), bytes.begin(), bytes.end());
        }

        ByteBuffer take() { return std::move(m_buf); }

    private:
        ByteBuffer m_buf;
    };

    class WireReader
    {
    public:
        explicit WireReader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

        bool eof() const { return m_pos >= m_bytes.size(); }

        std::uint8_t read_u8()
        {
            require_(1);
            return static_cast<std::uint8_t>(m_bytes[m_pos++]);
        }

        std::uint32_t read_u32()
        {
            require_(4);
            std::uint32_t out = 0;
            for (int i = 0; i < 4; ++i)
            {
                out |= (static_cast<std::uint32_t>(static_cast<std::uint8_t>(m_bytes[m_pos++])) << (8 * i));
            }
            return out;
        }

        std::uint64_t read_u64()
        {
            require_(8);
            std::uint64_t out = 0;
            for (int i = 0; i < 8; ++i)
            {
                out |= (static_cast<std::uint64_t>(static_cast<std::uint8_t>(m_bytes[m_pos++])) << (8 * i));
            }
            return out;
        }

        ByteBuffer read_bytes()
        {
            auto n = read_u32();
            require_(n);
            ByteBuffer out;
            out.insert(out.end(), m_bytes.begin() + static_cast<std::ptrdiff_t>(m_pos), m_bytes.begin() + static_cast<std::ptrdiff_t>(m_pos + n));
            m_pos += n;
            return out;
        }

    private:
        void require_(std::size_t n) const
        {
            if (m_pos + n > m_bytes.size())
            {
                throw std::runtime_error("WireReader: truncated buffer");
            }
        }

        std::span<const std::byte> m_bytes;
        std::size_t m_pos = 0;
    };
}
