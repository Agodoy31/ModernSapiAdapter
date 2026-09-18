#pragma once

#include "SpeechWorkerTypes.h"
#include <cstdint>

namespace PcmDeliveryPolicy
{
enum class ChunkAction : uint8_t
{
    AssembleSpeakingAudio,
    DrainCancellationAudio,
    FaultUnexpectedIdleAudio,
    DrainFaultedSessionAudio,
};

struct ChunkDecision
{
    ChunkAction action;
    uint32_t bytesToFrame;
    bool refreshProgress;
    bool countRawBytes;
};

enum class SpanAdmission : uint8_t
{
    Allow,
    Reject,
};

enum class BatchAction : uint8_t
{
    IgnoreStaleOrInactive,
    CreditAndCheckBoundary,
    CreditAndBeginCancellation,
    CheckCancellationBoundary,
};

struct BatchDecision
{
    BatchAction action;
    uint64_t acceptedBytesToCredit;
};

struct TerminalBoundaryFacts
{
    bool speakingAudioOverrun;
    bool speakingTerminalReached;
    bool cancellationTerminalReached;
};

[[nodiscard]] ChunkDecision EvaluateChunkIngest(
    const RequestContext& context,
    uint32_t bytesRead) noexcept;

[[nodiscard]] SpanAdmission EvaluateSpanAdmission(
    const RequestContext& context,
    const RequestToken& batchToken) noexcept;

[[nodiscard]] BatchDecision EvaluateBatchOutcome(
    const RequestContext& context,
    const RequestToken& batchToken,
    uint64_t fullyAcceptedBytes,
    bool sapiWriteRejected) noexcept;

[[nodiscard]] TerminalBoundaryFacts EvaluateTerminalBoundaryFacts(
    const RequestContext& context,
    bool assemblerHasCarry) noexcept;

[[nodiscard]] bool IsTerminalByteCountFrameAligned(
    uint64_t bytes,
    uint16_t blockAlign) noexcept;
}
