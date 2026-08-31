#include "pch.h"
#include "PcmFrameAssembler.h"

PcmFrameAssembler::PcmFrameAssembler(size_t blockAlign)
    : m_blockAlign(blockAlign)
{
    if (blockAlign == 0)
    {
        throw std::invalid_argument("PCM block alignment must be positive.");
    }

    m_carry.reserve(blockAlign - 1);
    m_completedFrame.reserve(blockAlign);
}

size_t PcmFrameAssembler::CalculateAlignedBytes(size_t byteCount) const noexcept
{
    return byteCount - (byteCount % m_blockAlign);
}

PcmFrameBatch PcmFrameAssembler::Process(const uint8_t* data, size_t size)
{
    PcmFrameBatch batch;
    if (size == 0)
    {
        return batch;
    }

    if (data == nullptr)
    {
        throw std::invalid_argument("PCM input cannot be null when its size is nonzero.");
    }

    const uint8_t* currentData = data;
    size_t remainingSize = size;

    // Stage 1: Prefix carry assembly (if carry buffer is non-empty)
    if (!m_carry.empty())
    {
        const size_t bytesNeeded = m_blockAlign - m_carry.size();
        if (remainingSize < bytesNeeded)
        {
            m_carry.insert(m_carry.end(), currentData, currentData + remainingSize);
            return batch;
        }

        m_completedFrame = m_carry;
        m_completedFrame.insert(m_completedFrame.end(), currentData, currentData + bytesNeeded);
        m_carry.clear();
        batch.spans[batch.count++] = { m_completedFrame.data(), m_completedFrame.size() };

        currentData += bytesNeeded;
        remainingSize -= bytesNeeded;
    }

    // Stage 2: Direct aligned chunk slicing (zero-copy when carry is empty)
    const size_t alignedSize = CalculateAlignedBytes(remainingSize);
    if (alignedSize > 0)
    {
        batch.spans[batch.count++] = { currentData, alignedSize };
        currentData += alignedSize;
        remainingSize -= alignedSize;
    }

    // Stage 3: Tail carry buffer retention
    if (remainingSize > 0)
    {
        m_carry.assign(currentData, currentData + remainingSize);
    }

    return batch;
}

void PcmFrameAssembler::Reset() noexcept
{
    m_carry.clear();
    m_completedFrame.clear();
}

bool PcmFrameAssembler::HasCarry() const noexcept
{
    return !m_carry.empty();
}

size_t PcmFrameAssembler::BlockAlign() const noexcept
{
    return m_blockAlign;
}
