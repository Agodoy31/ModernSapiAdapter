#pragma once
#include <string>
#include <string_view>
#include <cstddef>

/**
 * @struct ControlStreamBuffer
 * @brief Encapsulates line-delimited streaming buffer framing and compaction logic for IPC control pipes.
 */
struct ControlStreamBuffer
{
    std::string buffer;
    size_t readOffset{0};
    size_t searchOffset{0};

    static constexpr size_t DefaultInitialCapacity = 8192;
    static constexpr size_t MaxRecordBytes = 16 * 1024 * 1024; // 16 MB limit

    ControlStreamBuffer()
    {
        buffer.reserve(DefaultInitialCapacity);
    }

    void Append(const char* data, size_t size)
    {
        buffer.append(data, size);
    }

    [[nodiscard]] bool TryExtractLine(std::string_view& outLine) noexcept
    {
        const size_t newlinePos = buffer.find('\n', searchOffset);
        if (newlinePos == std::string::npos)
        {
            searchOffset = buffer.size();
            return false;
        }

        const size_t lineLength = newlinePos - readOffset;
        outLine = std::string_view(buffer.data() + readOffset, lineLength);
        if (outLine.ends_with('\r'))
        {
            outLine.remove_suffix(1);
        }

        readOffset = newlinePos + 1;
        searchOffset = readOffset;
        return true;
    }

    [[nodiscard]] bool HasPendingLine() const noexcept
    {
        return buffer.find('\n', readOffset) != std::string::npos;
    }

    [[nodiscard]] bool IsOverCapacity(size_t maxBytes = MaxRecordBytes) const noexcept
    {
        return (buffer.size() - readOffset) > maxBytes;
    }

    void Compact() noexcept
    {
        if (readOffset == buffer.size())
        {
            buffer.clear();
            readOffset = 0;
            searchOffset = 0;
        }
        else if (readOffset > 0)
        {
            buffer.erase(0, readOffset);
            readOffset = 0;
            searchOffset = 0;
        }
    }

    void Clear() noexcept
    {
        buffer.clear();
        readOffset = 0;
        searchOffset = 0;
    }

    [[nodiscard]] size_t UnconsumedBytes() const noexcept
    {
        return buffer.size() - readOffset;
    }
};
