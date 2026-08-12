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

PcmFrameBatch PcmFrameAssembler::Process(const uint8_t* data, size_t size)
{
    PcmFrameBatch batch;
    if (size == 0)
    {
        return batch;
    }
    if (!data)
    {
        throw std::invalid_argument("PCM input cannot be null when its size is nonzero.");
    }

    if (m_carry.empty())
    {
        const size_t alignedSize = size - (size % m_blockAlign);
        if (alignedSize != 0)
        {
            batch.spans[batch.count++] = { data, alignedSize };
        }

        m_carry.assign(data + alignedSize, data + size);
        return batch;
    }

    const size_t bytesNeeded = m_blockAlign - m_carry.size();
    if (size < bytesNeeded)
    {
        m_carry.insert(m_carry.end(), data, data + size);
        return batch;
    }

    m_completedFrame = m_carry;
    m_completedFrame.insert(m_completedFrame.end(), data, data + bytesNeeded);
    m_carry.clear();
    batch.spans[batch.count++] = { m_completedFrame.data(), m_completedFrame.size() };

    const uint8_t* remainingData = data + bytesNeeded;
    const size_t remainingSize = size - bytesNeeded;
    const size_t alignedSize = remainingSize - (remainingSize % m_blockAlign);
    if (alignedSize != 0)
    {
        batch.spans[batch.count++] = { remainingData, alignedSize };
    }
    m_carry.assign(remainingData + alignedSize, remainingData + remainingSize);
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
