/**
 * @file PcmFrameAssembler.h
 * @brief Reconstructs complete provider-native PCM frames across byte-mode pipe reads.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct PcmFrameSpan
{
    const uint8_t* data;
    size_t size;
};

struct PcmFrameBatch
{
    std::array<PcmFrameSpan, 2> spans{};
    size_t count{0};

    PcmFrameSpan* begin() noexcept { return spans.data(); }
    PcmFrameSpan* end() noexcept { return spans.data() + count; }
    const PcmFrameSpan* begin() const noexcept { return spans.data(); }
    const PcmFrameSpan* end() const noexcept { return spans.data() + count; }
    bool empty() const noexcept { return count == 0; }
    size_t size() const noexcept { return count; }
    PcmFrameSpan& operator[](size_t index) noexcept { return spans[index]; }
    const PcmFrameSpan& operator[](size_t index) const noexcept { return spans[index]; }
};

class PcmFrameAssembler
{
public:
    /** @brief Creates an assembler for one negotiated provider PCM block alignment. */
    explicit PcmFrameAssembler(size_t blockAlign);

    /** @brief Accepts an arbitrary pipe fragment and returns complete frame-aligned spans. */
    PcmFrameBatch Process(const uint8_t* data, size_t size);
    /** @brief Discards the partial frame retained for the active request. */
    void Reset() noexcept;
    /** @brief Reports whether an incomplete frame is currently retained. */
    bool HasCarry() const noexcept;
    /** @brief Returns the provider-native PCM block alignment. */
    size_t BlockAlign() const noexcept;

private:
    size_t m_blockAlign;
    std::vector<uint8_t> m_carry;
    std::vector<uint8_t> m_completedFrame;
};
